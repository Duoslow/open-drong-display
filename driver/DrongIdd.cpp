/*++

DrongIdd - IddCx display driver for the "Drong Display" USB screen dongle (USB 1CBE:0003, interface 0).

Based on Microsoft's IndirectDisplay sample (IddSampleDriver, Microsoft Public License, see LICENSE-IddSample.txt).

The driver binds to the dongle's vendor interface, so Windows gets a "USB-Monitor" (1920x1080 @ 50 Hz) exactly while
the dongle is plugged in. Each frame Windows composes for that monitor is copied, encoded to H.264 with AMD AMF on the
render GPU and sent over USB with the protocol documented in docs/PROTOCOL.md.

50 Hz is deliberate: the dongle drains its frame queue at roughly 50 frames/s, faster input makes the delay grow.

Environment:

    User Mode, UMDF 2.25, IddCx 1.4

--*/

#include "DrongIdd.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#include "core/Factory.h"
#include "components/VideoConverter.h"
#include "components/VideoEncoderVCE.h"

#include <initguid.h> // defines the CODECAPI_* GUIDs used below
#include <codecapi.h>

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

#pragma region logging

static const wchar_t* kLogDir = L"C:\\ProgramData\\drong-display";
static const wchar_t* kLogFile = L"C:\\ProgramData\\drong-display\\driver.log";
static std::mutex g_logLock;

void DrongLog(const char* fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    OutputDebugStringA(line);

    std::lock_guard<std::mutex> g(g_logLock);
    HANDLE f = CreateFileW(kLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size;
        if (GetFileSizeEx(f, &size) && size.QuadPart > 4 * 1024 * 1024)
        {
            // keep the log small: start over once it passes 4 MB
            CloseHandle(f);
            f = CreateFileW(kLogFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        DWORD w;
        WriteFile(f, line, (DWORD)n, &w, nullptr);
        CloseHandle(f);
    }
}

static double NowMs()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

#pragma endregion

#pragma region monitor description

// One monitor named "USB-Monitor". EDID 1.4 with the CEA 1080p50 timing (148.5 MHz, 2640x1125 total) as its
// preferred mode; Windows can also pick the smaller modes below. Every mode is 50 Hz: the dongle drains its frame
// queue at about 50 frames/s, faster input makes the delay grow.
static constexpr DWORD kWidth = 1920, kHeight = 1080, kRefresh = 50;

struct DisplayMode
{
    DWORD Width, Height;
};
static constexpr DisplayMode kModes[] = {{1920, 1080}, {1600, 900}, {1280, 720}}; // [0] = preferred
static constexpr UINT kModeCount = ARRAYSIZE(kModes);

// Stable container id so Windows remembers the monitor's position across replugs.
// {6C0C4F65-1E44-4E8D-9C8B-2F0A1D7E5B31}
static const GUID kMonitorContainerId = {0x6c0c4f65, 0x1e44, 0x4e8d, {0x9c, 0x8b, 0x2f, 0x0a, 0x1d, 0x7e, 0x5b, 0x31}};

static void BuildEdid(BYTE e[128])
{
    static const BYTE base[] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
        0x56, 0x62,             // manufacturer "USB"
        0x50, 0x00,             // product code
        0x01, 0x00, 0x00, 0x00, // serial
        0x01, 0x24,             // week 1, 2026
        0x01, 0x04,             // EDID 1.4
        0x80, 0x3C, 0x22, 0x78, 0x02, // digital, 60x34 cm, gamma 2.2, preferred timing is native
        0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54, // sRGB chromaticity
        0x00, 0x00, 0x00,       // no established timings
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        // detailed timing: 1920x1080 @ 50 Hz (CEA 1080p50), +h +v sync
        0x02, 0x3A, 0x80, 0xD0, 0x72, 0x38, 0x2D, 0x40, 0x10, 0x2C, 0x45, 0x80, 0x58, 0x54, 0x21, 0x00, 0x00, 0x1E,
        // monitor name
        0x00, 0x00, 0x00, 0xFC, 0x00, 'U', 'S', 'B', '-', 'M', 'o', 'n', 'i', 't', 'o', 'r', 0x0A, 0x20,
        // range limits: 49-61 Hz, 30-83 kHz, 150 MHz
        0x00, 0x00, 0x00, 0xFD, 0x00, 49, 61, 30, 83, 15, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        // dummy descriptor
        0x00, 0x00, 0x00, 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x00, // no extensions
    };
    static_assert(sizeof base == 127, "EDID body must be 127 bytes");
    memcpy(e, base, 127);
    BYTE sum = 0;
    for (int i = 0; i < 127; i++)
        sum = (BYTE)(sum + e[i]);
    e[127] = (BYTE)(0x100 - sum);
}

static BYTE g_Edid[128];

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

#pragma endregion

#pragma region USB transport

namespace proto
{
constexpr ULONG CMD_INIT = 0x02, CMD_CURSOR = 0x63, CMD_H264 = 0x67, CMD_NONCE_REPLY = 0x58, CMD_CHECK = 0x74,
                CMD_GET_INFO = 0x77;
constexpr ULONG EV_NONCE_REQ = 0x57, EV_CHECK_REPLY = 0x75, EV_INFO = 0x77, EV_CONNECTED = 0xA0, EV_DISCONNECTED = 0xA1;
constexpr size_t PKT = 512, CHUNK = 0xA0000;
}

static void PutBE(BYTE* p, ULONG v)
{
    p[0] = (BYTE)(v >> 24), p[1] = (BYTE)(v >> 16), p[2] = (BYTE)(v >> 8), p[3] = (BYTE)v;
}
static ULONG GetBE(const BYTE* p)
{
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

static void RandomBytes(BYTE* p, size_t n)
{
    static ULONG64 s = (ULONG64)NowMs() * 6364136223846793005ULL ^ GetCurrentProcessId();
    for (size_t i = 0; i < n; i++)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = (BYTE)(s >> 56);
    }
}

NTSTATUS DongleUsb::Open(WDFDEVICE Device)
{
    NTSTATUS Status = WdfUsbTargetDeviceCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &m_UsbDevice);
    if (!NT_SUCCESS(Status))
    {
        DrongLog("WdfUsbTargetDeviceCreate failed 0x%08x", Status);
        return Status;
    }

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS Params;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&Params);
    Status = WdfUsbTargetDeviceSelectConfig(m_UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &Params);
    if (!NT_SUCCESS(Status))
    {
        DrongLog("WdfUsbTargetDeviceSelectConfig failed 0x%08x", Status);
        return Status;
    }

    WDFUSBINTERFACE Interface = Params.Types.SingleInterface.ConfiguredUsbInterface;
    BYTE PipeCount = WdfUsbInterfaceGetNumConfiguredPipes(Interface);
    for (BYTE i = 0; i < PipeCount; i++)
    {
        WDF_USB_PIPE_INFORMATION Info;
        WDF_USB_PIPE_INFORMATION_INIT(&Info);
        WDFUSBPIPE Pipe = WdfUsbInterfaceGetConfiguredPipe(Interface, i, &Info);
        if (Info.PipeType != WdfUsbPipeTypeBulk)
            continue;
        if (WdfUsbTargetPipeIsInEndpoint(Pipe))
            m_In = Pipe;
        else
            m_Out = Pipe;
        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(Pipe);
    }
    if (!m_In || !m_Out)
    {
        DrongLog("dongle interface has no bulk in/out pipes");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

// Returns bytes transferred, -1 on timeout, -2 on error.
long DongleUsb::Xfer(WDFUSBPIPE Pipe, BYTE* Buf, ULONG Len, ULONG TimeoutMs)
{
    WDF_MEMORY_DESCRIPTOR Mem;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Mem, Buf, Len);
    WDF_REQUEST_SEND_OPTIONS Opt;
    WDF_REQUEST_SEND_OPTIONS_INIT(&Opt, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Opt, WDF_REL_TIMEOUT_IN_MS(TimeoutMs));
    ULONG Done = 0;
    NTSTATUS Status = Pipe == m_In ? WdfUsbTargetPipeReadSynchronously(Pipe, nullptr, &Opt, &Mem, &Done)
                                   : WdfUsbTargetPipeWriteSynchronously(Pipe, nullptr, &Opt, &Mem, &Done);
    if (Status == STATUS_IO_TIMEOUT || Status == STATUS_TIMEOUT || Status == STATUS_CANCELLED)
        return -1;
    if (!NT_SUCCESS(Status))
        return -2;
    return (long)Done;
}

// Writes data zero-padded to a multiple of 512 bytes, like the vendor driver. Caller holds m_WriteLock.
bool DongleUsb::WriteLocked(const BYTE* Data, size_t Len, ULONG TimeoutMs)
{
    size_t Padded = (Len + proto::PKT - 1) / proto::PKT * proto::PKT;
    if (Padded == 0)
        Padded = proto::PKT;
    if (m_Scratch.size() < Padded)
        m_Scratch.resize(Padded);
    memcpy(m_Scratch.data(), Data, Len);
    memset(m_Scratch.data() + Len, 0, Padded - Len);
    return Xfer(m_Out, m_Scratch.data(), (ULONG)Padded, TimeoutMs) == (long)Padded;
}

// Stops all writes after the one in flight has completed. A frame cut off in the middle (cancelled transfer) can
// crash the dongle's firmware until it is power-cycled, so this runs before Windows tears the pipes down.
void DongleUsb::Close()
{
    std::lock_guard<std::mutex> g(m_WriteLock);
    m_Closed = true;
}

void DongleUsb::Resume()
{
    std::lock_guard<std::mutex> g(m_WriteLock);
    m_Closed = false;
}

bool DongleUsb::Cmd(ULONG Cmd, ULONG A1, ULONG A2, const BYTE* Payload, size_t PayloadLen)
{
    std::lock_guard<std::mutex> g(m_WriteLock);
    if (m_Closed)
        return false;
    BYTE h[24] = {};
    PutBE(h, Cmd), PutBE(h + 4, A1), PutBE(h + 12, A2);
    if (!WriteLocked(h, sizeof h, 3000))
        return false;
    return Payload ? WriteLocked(Payload, PayloadLen, 3000) : true;
}

bool DongleUsb::SendFrame(const BYTE* Au, size_t Total, ULONG W, ULONG H)
{
    std::lock_guard<std::mutex> g(m_WriteLock);
    if (m_Closed)
        return false;
    ULONG n = (ULONG)max<size_t>(1, (Total + proto::CHUNK - 1) / proto::CHUNK);
    for (ULONG i = 0; i < n; i++)
    {
        size_t Off = i * proto::CHUNK, ChunkLen = min(proto::CHUNK, Total - Off);
        BYTE Hdr[64] = {};
        PutBE(Hdr, proto::CMD_H264), PutBE(Hdr + 4, 2), PutBE(Hdr + 12, (ULONG)ChunkLen);
        ULONG Sub[7] = {(ULONG)Total, (ULONG)ChunkLen, n, i + 1, 1, W, H};
        for (int k = 0; k < 7; k++)
            PutBE(Hdr + 24 + 4 * k, Sub[k]);
        if (!WriteLocked(Hdr, sizeof Hdr, 3000) || !WriteLocked(Au + Off, ChunkLen, 3000))
            return false;
    }
    return true;
}

// Reads one 512-byte event packet: 1 = event, 0 = timeout/empty, -1 = error.
int DongleUsb::ReadEvent(ULONG Ev[6], ULONG TimeoutMs)
{
    BYTE b[proto::PKT];
    long n = Xfer(m_In, b, sizeof b, TimeoutMs);
    if (n == -2)
        return -1;
    if (n < 24)
        return 0;
    for (int i = 0; i < 6; i++)
        Ev[i] = GetBE(b + 4 * i);
    return Ev[0] != 0 ? 1 : 0;
}

std::vector<BYTE> DongleUsb::ReadData(size_t n)
{
    std::vector<BYTE> Out;
    BYTE b[4096];
    while (Out.size() < n)
    {
        size_t Want = min(sizeof b, (n - Out.size() + proto::PKT - 1) / proto::PKT * proto::PKT);
        long Got = Xfer(m_In, b, (ULONG)Want, 2000);
        if (Got <= 0)
            break;
        Out.insert(Out.end(), b, b + Got);
    }
    Out.resize(min(Out.size(), n));
    return Out;
}

bool DongleUsb::ControlMsg9()
{
    BYTE Reply[4] = {};
    WDF_MEMORY_DESCRIPTOR Mem;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Mem, Reply, 1);
    WDF_USB_CONTROL_SETUP_PACKET Packet;
    WDF_USB_CONTROL_SETUP_PACKET_INIT_CLASS(&Packet, BmRequestDeviceToHost, BmRequestToInterface, 9, 0, 0);
    WDF_REQUEST_SEND_OPTIONS Opt;
    WDF_REQUEST_SEND_OPTIONS_INIT(&Opt, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Opt, WDF_REL_TIMEOUT_IN_MS(3000));
    ULONG Done = 0;
    NTSTATUS Status = WdfUsbTargetDeviceSendControlTransferSynchronously(m_UsbDevice, nullptr, &Opt, &Packet, &Mem, &Done);
    return NT_SUCCESS(Status) && Done == 1;
}

#pragma endregion

#pragma region DongleSession

// Handshake + event handling, on its own thread for as long as the device is powered.
void DongleSession::Start(DongleUsb* Usb)
{
    Stop();
    m_Usb = Usb;
    m_Quit = false;
    m_Thread = std::thread([this] { Run(); });
}

void DongleSession::Stop()
{
    m_Quit = true;
    if (m_Thread.joinable())
        m_Thread.join();
    m_Ready = false;
}

bool DongleSession::Handshake()
{
    if (!m_Usb->ControlMsg9())
    {
        DrongLog("dongle not answering the control request");
        return false;
    }
    Sleep(5);
    BYTE Nonce[16];
    RandomBytes(Nonce, sizeof Nonce);
    if (!m_Usb->Cmd(proto::CMD_CHECK, 1, 16, Nonce, sizeof Nonce))
        return false;
    double t = NowMs();
    ULONG Ev[6];
    bool GotCheck = false;
    while (!m_Quit && NowMs() - t < 3000)
    {
        int r = m_Usb->ReadEvent(Ev, 100);
        if (r < 0)
            return false;
        if (r > 0 && Ev[0] == proto::EV_CHECK_REPLY)
        {
            m_Usb->ReadData(Ev[3]);
            GotCheck = true;
            break;
        }
    }
    if (!GotCheck)
    {
        DrongLog("dongle sent no check reply");
        return false;
    }
    if (!m_Usb->Cmd(proto::CMD_INIT, 1, 0))
        return false;
    // After a previous session the dongle does not announce "monitor connected" again; ask for the info directly.
    m_Usb->Cmd(proto::CMD_GET_INFO, 1, 0);
    return true;
}

void DongleSession::Run()
{
    while (!m_Quit)
    {
        if (!Handshake())
        {
            for (int i = 0; i < 20 && !m_Quit; i++)
                Sleep(100);
            continue;
        }
        DrongLog("dongle handshake ok");
        // Like the vendor driver, only stream once the dongle has reported its info (or stayed quiet for 3 s).
        double HandshakeT = NowMs();
        auto GoReady = [&](const char* Why)
        {
            if (m_Ready)
                return;
            // hide the dongle's own cursor; Windows draws the pointer into our frames
            BYTE Blank[28] = {};
            m_Usb->Cmd(proto::CMD_CURSOR, 2, sizeof Blank, Blank, sizeof Blank);
            m_NeedIdr = true;
            m_Ready = true;
            DrongLog("streaming (%s)", Why);
        };

        std::string LastInfo;
        while (!m_Quit)
        {
            ULONG Ev[6];
            int r = m_Usb->ReadEvent(Ev, 100);
            if (r < 0)
            {
                DrongLog("usb read error; restarting session");
                break;
            }
            if (!m_Ready && NowMs() - HandshakeT > 3000)
                GoReady("no device info within 3 s");
            if (r == 0)
                continue;
            switch (Ev[0])
            {
            case proto::EV_CONNECTED:
                m_Usb->Cmd(proto::CMD_GET_INFO, 1, 0);
                break;
            case proto::EV_INFO:
            {
                auto Raw = m_Usb->ReadData(Ev[3]);
                std::string Info(Raw.begin(), Raw.end());
                if (Info != LastInfo)
                    DrongLog("device info: %s", Info.c_str());
                LastInfo = Info;
                GoReady("device info received");
                break;
            }
            case proto::EV_NONCE_REQ:
            {
                BYTE n[16];
                RandomBytes(n, sizeof n);
                m_Usb->Cmd(proto::CMD_NONCE_REPLY, 1, 16, n, sizeof n);
                break;
            }
            case proto::EV_DISCONNECTED:
                DrongLog("dongle reports its monitor disconnected");
                break;
            default:
                if (Ev[3] && Ev[3] < (1u << 20))
                    m_Usb->ReadData(Ev[3]); // e.g. 0x71: firmware download URL, ignored
            }
        }
        m_Ready = false;
    }
}

#pragma endregion

#pragma region Encoders

// Common interface: encode one BGRA texture (the copy of Windows' frame) into one H.264 access unit (Annex-B,
// SPS/PPS in front of every IDR).
struct FrameEncoder
{
    virtual ~FrameEncoder() = default;
    virtual const char* Name() const = 0;
    virtual HRESULT Encode(ID3D11Texture2D* Bgra, bool ForceIdr) = 0;

    std::vector<BYTE> Au;
    UINT Width = 0, Height = 0;
};

// 15 Mbit/s at 1920x1080, scaled by pixel count for the smaller modes
static amf_int64 BitrateFor(UINT W, UINT H)
{
    return max<amf_int64>(6'000'000, 15'000'000LL * W * H / (1920 * 1080));
}

// ---- AMD AMF ---------------------------------------------------------------------------------------------------

struct AmfEncoder : FrameEncoder
{
    amf::AMFFactory* Factory = nullptr;
    amf::AMFContextPtr Context;
    amf::AMFComponentPtr Converter, Encoder;

    const char* Name() const override { return "AMD AMF"; }

    ~AmfEncoder() override
    {
        if (Encoder) Encoder->Terminate();
        if (Converter) Converter->Terminate();
        if (Context) Context->Terminate();
    }

    HRESULT Init(ID3D11Device* Dev, UINT W, UINT H)
    {
        HMODULE Lib = LoadLibraryW(AMF_DLL_NAME);
        if (!Lib)
        {
            return E_NOINTERFACE; // no AMD driver
        }
        auto InitFn = (AMFInit_Fn)GetProcAddress(Lib, AMF_INIT_FUNCTION_NAME);
        auto QueryFn = (AMFQueryVersion_Fn)GetProcAddress(Lib, AMF_QUERY_VERSION_FUNCTION_NAME);
        amf_uint64 Rt = 0;
        if (QueryFn)
            QueryFn(&Rt);
        AMF_RESULT r = InitFn ? InitFn(min<amf_uint64>(Rt ? Rt : AMF_FULL_VERSION, AMF_FULL_VERSION), &Factory) : AMF_FAIL;
        if (r == AMF_OK) r = Factory->CreateContext(&Context);
        if (r == AMF_OK) r = Context->InitDX11(Dev);
        if (r == AMF_OK) r = Factory->CreateComponent(Context, AMFVideoConverter, &Converter);
        if (r == AMF_OK)
        {
            Converter->SetProperty(AMF_VIDEO_CONVERTER_MEMORY_TYPE, amf::AMF_MEMORY_DX11);
            Converter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, amf::AMF_SURFACE_NV12);
            Converter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_SIZE, AMFConstructSize((amf_int32)W, (amf_int32)H));
            Converter->SetProperty(AMF_VIDEO_CONVERTER_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
            r = Converter->Init(amf::AMF_SURFACE_BGRA, (amf_int32)W, (amf_int32)H);
        }
        if (r == AMF_OK) r = Factory->CreateComponent(Context, AMFVideoEncoderVCE_AVC, &Encoder);
        if (r == AMF_OK)
        {
            Encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_CONSTRAINED_BASELINE);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize((amf_int32)W, (amf_int32)H));
            Encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(kRefresh, 1));
            Encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
            const amf_int64 Bitrate = BitrateFor(W, H);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, Bitrate);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, Bitrate * 2);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, Bitrate / kRefresh * 2);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64)0);
            // 1 reference frame -> SPS max_dec_frame_buffering=1: the dongle shows each frame immediately
            Encoder->SetProperty(AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, (amf_int64)1);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64)kRefresh * 2);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, (amf_int64)kRefresh * 2);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_CABAC_ENABLE, AMF_VIDEO_ENCODER_CALV);
            Encoder->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, (amf_int64)50);
            r = Encoder->Init(amf::AMF_SURFACE_NV12, (amf_int32)W, (amf_int32)H);
        }
        if (r != AMF_OK)
        {
            DrongLog("AMF init failed (%d)", (int)r);
            return E_FAIL;
        }
        Width = W, Height = H;
        return S_OK;
    }

    HRESULT Encode(ID3D11Texture2D* Tex, bool ForceIdr) override
    {
        amf::AMFSurfacePtr In;
        if (Context->CreateSurfaceFromDX11Native(Tex, &In, nullptr) != AMF_OK || Converter->SubmitInput(In) != AMF_OK)
            return E_FAIL;
        amf::AMFDataPtr Nv12;
        for (int i = 0; i < 100 && !Nv12; i++)
        {
            AMF_RESULT r = Converter->QueryOutput(&Nv12);
            if (r != AMF_OK && r != AMF_REPEAT)
                return E_FAIL;
            if (!Nv12)
                Sleep(0);
        }
        if (!Nv12)
            return E_FAIL;
        if (ForceIdr)
        {
            Nv12->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            Nv12->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            Nv12->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
        AMF_RESULT r;
        while ((r = Encoder->SubmitInput(Nv12)) == AMF_INPUT_FULL)
            Drain(false);
        if (r != AMF_OK)
            return E_FAIL;
        Au.clear();
        return Drain(true) ? S_OK : E_FAIL;
    }

    bool Drain(bool WaitOne)
    {
        for (int Tries = 0; Tries < 20; Tries++)
        {
            amf::AMFDataPtr Out;
            AMF_RESULT r = Encoder->QueryOutput(&Out); // blocks up to QUERY_TIMEOUT
            if (Out)
            {
                amf::AMFBufferPtr b(Out);
                auto p = (const BYTE*)b->GetNative();
                Au.assign(p, p + b->GetSize());
                return true;
            }
            if (!WaitOne)
                return true;
            if (r != AMF_OK && r != AMF_REPEAT)
                return false;
        }
        return false;
    }
};

// ---- Media Foundation (any GPU vendor's hardware encoder, or Microsoft's software encoder) ---------------------

// BGRA -> NV12 (BT.709, limited range) on the GPU with the D3D11 video processor; every vendor implements it.
struct Nv12Converter
{
    ComPtr<ID3D11VideoDevice> VDev;
    ComPtr<ID3D11VideoContext> VCtx;
    ComPtr<ID3D11VideoProcessorEnumerator> Enum;
    ComPtr<ID3D11VideoProcessor> Proc;
    ComPtr<ID3D11Texture2D> Nv12;
    ComPtr<ID3D11VideoProcessorOutputView> OutView;
    ComPtr<ID3D11VideoProcessorInputView> InView;
    ID3D11Texture2D* InTex = nullptr;

    HRESULT Init(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, UINT W, UINT H)
    {
        HRESULT hr = Dev->QueryInterface(IID_PPV_ARGS(&VDev));
        if (SUCCEEDED(hr)) hr = Ctx->QueryInterface(IID_PPV_ARGS(&VCtx));
        if (FAILED(hr))
            return hr;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC Cd = {D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {kRefresh, 1}, W, H,
                                                 {kRefresh, 1}, W, H, D3D11_VIDEO_USAGE_OPTIMAL_SPEED};
        hr = VDev->CreateVideoProcessorEnumerator(&Cd, &Enum);
        if (SUCCEEDED(hr)) hr = VDev->CreateVideoProcessor(Enum.Get(), 0, &Proc);
        if (FAILED(hr))
            return hr;
        D3D11_TEXTURE2D_DESC Td = {W, H, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        hr = Dev->CreateTexture2D(&Td, nullptr, &Nv12);
        if (FAILED(hr))
            return hr;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC Od = {D3D11_VPOV_DIMENSION_TEXTURE2D};
        hr = VDev->CreateVideoProcessorOutputView(Nv12.Get(), Enum.Get(), &Od, &OutView);
        if (FAILED(hr))
            return hr;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE In = {}, Out = {};
        In.RGB_Range = 0;                 // full-range RGB desktop
        Out.YCbCr_Matrix = 1;             // BT.709
        Out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        VCtx->VideoProcessorSetStreamColorSpace(Proc.Get(), 0, &In);
        VCtx->VideoProcessorSetOutputColorSpace(Proc.Get(), &Out);
        VCtx->VideoProcessorSetStreamFrameFormat(Proc.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        VCtx->VideoProcessorSetStreamAutoProcessingMode(Proc.Get(), 0, FALSE);
        return S_OK;
    }

    HRESULT Convert(ID3D11Texture2D* Bgra)
    {
        if (Bgra != InTex)
        {
            InView.Reset();
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC Id = {0, D3D11_VPIV_DIMENSION_TEXTURE2D};
            HRESULT hr = VDev->CreateVideoProcessorInputView(Bgra, Enum.Get(), &Id, &InView);
            if (FAILED(hr))
                return hr;
            InTex = Bgra;
        }
        D3D11_VIDEO_PROCESSOR_STREAM S = {};
        S.Enable = TRUE;
        S.pInputSurface = InView.Get();
        return VCtx->VideoProcessorBlt(Proc.Get(), OutView.Get(), 0, 1, &S);
    }
};

static void SetCodecValue(ICodecAPI* Api, const GUID& Key, VARTYPE Vt, ULONG Value, const char* What)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = Vt;
    if (Vt == VT_BOOL)
        v.boolVal = Value ? VARIANT_TRUE : VARIANT_FALSE;
    else
        v.ulVal = Value;
    HRESULT hr = Api->SetValue(&Key, &v);
    if (FAILED(hr))
        DrongLog("  encoder ignores %s (0x%08x)", What, hr);
}

static int NalType(const BYTE* p) { return p[0] & 0x1F; }

// Calls Fn(type, offset) for every NAL unit start in an Annex-B buffer.
template <typename F> static void ForEachNal(const std::vector<BYTE>& B, F Fn)
{
    for (size_t i = 0; i + 3 < B.size(); i++)
    {
        if (B[i] == 0 && B[i + 1] == 0 && B[i + 2] == 1)
        {
            Fn(NalType(&B[i + 3]), i);
            i += 2;
        }
    }
}

struct MfEncoder : FrameEncoder
{
    bool Hardware = false, Async = false, D3DAware = false;
    std::string FriendlyName;
    ComPtr<IMFTransform> Mft;
    ComPtr<IMFMediaEventGenerator> Events;
    ComPtr<ICodecAPI> Codec;
    ComPtr<IMFDXGIDeviceManager> DevManager;
    ComPtr<ID3D11Device> Device;
    ComPtr<ID3D11DeviceContext> DeviceContext;
    ComPtr<ID3D11Texture2D> Staging; // NV12 readback for encoders that can't take GPU textures
    Nv12Converter Converter;
    DWORD InId = 0, OutId = 0;
    UINT NeedInput = 0;
    LONGLONG Time = 0;
    std::vector<BYTE> SequenceHeader; // SPS/PPS, prepended to IDR frames that come without them

    const char* Name() const override { return FriendlyName.c_str(); }

    ~MfEncoder() override
    {
        if (Mft)
        {
            Mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            Mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        if (Events) Events.Reset();
        if (Mft) Mft.Reset();
    }

    HRESULT Init(ID3D11Device* Dev, ID3D11DeviceContext* Ctx, LUID Adapter, UINT W, UINT H, bool UseHardware)
    {
        static HRESULT Startup = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(Startup))
            return Startup;
        Device = Dev, DeviceContext = Ctx, Hardware = UseHardware;

        // find an H.264 encoder: the hardware one of the GPU rendering this monitor, or the software one
        MFT_REGISTER_TYPE_INFO InType = {MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO OutType = {MFMediaType_Video, MFVideoFormat_H264};
        ComPtr<IMFAttributes> EnumAttr;
        MFCreateAttributes(&EnumAttr, 1);
        EnumAttr->SetBlob(MFT_ENUM_ADAPTER_LUID, (const UINT8*)&Adapter, sizeof Adapter);
        IMFActivate** Acts = nullptr;
        UINT32 Count = 0;
        UINT32 Flags = (UseHardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
        HRESULT hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, Flags, &InType, &OutType, UseHardware ? EnumAttr.Get() : nullptr, &Acts, &Count);
        if (FAILED(hr) || Count == 0)
        {
            CoTaskMemFree(Acts);
            return FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND;
        }
        for (UINT32 i = 0; i < Count && !Mft; i++)
        {
            if (SUCCEEDED(Acts[i]->ActivateObject(IID_PPV_ARGS(&Mft))))
            {
                WCHAR* N = nullptr;
                UINT32 Len = 0;
                if (SUCCEEDED(Acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &N, &Len)))
                {
                    char Buf[256];
                    WideCharToMultiByte(CP_UTF8, 0, N, -1, Buf, sizeof Buf, nullptr, nullptr);
                    FriendlyName = std::string("Media Foundation: ") + Buf;
                    CoTaskMemFree(N);
                }
            }
        }
        for (UINT32 i = 0; i < Count; i++)
            Acts[i]->Release();
        CoTaskMemFree(Acts);
        if (!Mft)
            return MF_E_TOPO_CODEC_NOT_FOUND;
        if (FriendlyName.empty())
            FriendlyName = UseHardware ? "Media Foundation hardware encoder" : "Media Foundation software encoder";

        ComPtr<IMFAttributes> Attr;
        if (SUCCEEDED(Mft->GetAttributes(&Attr)))
        {
            Async = MFGetAttributeUINT32(Attr.Get(), MF_TRANSFORM_ASYNC, FALSE) != 0;
            if (Async)
                Attr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            D3DAware = MFGetAttributeUINT32(Attr.Get(), MF_SA_D3D11_AWARE, FALSE) != 0;
            Attr->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        if (Async && FAILED(Mft.As(&Events)))
            return E_NOINTERFACE;
        if (D3DAware)
        {
            UINT Token = 0;
            hr = MFCreateDXGIDeviceManager(&Token, &DevManager);
            if (SUCCEEDED(hr)) hr = DevManager->ResetDevice(Dev, Token);
            if (SUCCEEDED(hr)) hr = Mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)DevManager.Get());
            if (FAILED(hr))
            {
                DrongLog("  encoder rejected the D3D11 device (0x%08x); using system memory", hr);
                DevManager.Reset();
                D3DAware = false;
            }
        }
        if (FAILED(Mft->GetStreamIDs(1, &InId, 1, &OutId)))
            InId = OutId = 0;

        // low-latency settings, the same stream the dongle gets from AMF
        if (SUCCEEDED(Mft.As(&Codec)))
        {
            SetCodecValue(Codec.Get(), CODECAPI_AVLowLatencyMode, VT_BOOL, TRUE, "low-latency mode");
            // low-delay VBR rather than CBR: CBR pads frames with filler data, which freezes the dongle
            SetCodecValue(Codec.Get(), CODECAPI_AVEncCommonRateControlMode, VT_UI4, eAVEncCommonRateControlMode_LowDelayVBR, "low-delay VBR");
            SetCodecValue(Codec.Get(), CODECAPI_AVEncCommonMeanBitRate, VT_UI4, (ULONG)BitrateFor(W, H), "bitrate");
            SetCodecValue(Codec.Get(), CODECAPI_AVEncMPVGOPSize, VT_UI4, kRefresh * 2, "GOP size");
            SetCodecValue(Codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, VT_UI4, 0, "no B-frames");
            SetCodecValue(Codec.Get(), CODECAPI_AVEncVideoMaxNumRefFrame, VT_UI4, 1, "1 reference frame");
            SetCodecValue(Codec.Get(), CODECAPI_AVEncH264CABACEnable, VT_BOOL, FALSE, "CAVLC");
            SetCodecValue(Codec.Get(), CODECAPI_AVEncCommonQualityVsSpeed, VT_UI4, 0, "speed preset");
        }

        // output type first (encoders derive the input type from it), constrained baseline with fallbacks
        static const UINT32 Profiles[] = {eAVEncH264VProfile_ConstrainedBase, eAVEncH264VProfile_Base, eAVEncH264VProfile_Main};
        hr = E_FAIL;
        for (UINT32 Profile : Profiles)
        {
            ComPtr<IMFMediaType> Out;
            MFCreateMediaType(&Out);
            Out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            Out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            Out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)BitrateFor(W, H));
            MFSetAttributeSize(Out.Get(), MF_MT_FRAME_SIZE, W, H);
            MFSetAttributeRatio(Out.Get(), MF_MT_FRAME_RATE, kRefresh, 1);
            MFSetAttributeRatio(Out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            Out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            Out->SetUINT32(MF_MT_MPEG2_PROFILE, Profile);
            hr = Mft->SetOutputType(OutId, Out.Get(), 0);
            if (SUCCEEDED(hr))
            {
                DrongLog("  H.264 profile %u", Profile);
                break;
            }
        }
        if (FAILED(hr))
        {
            DrongLog("  encoder rejected the output type (0x%08x)", hr);
            return hr;
        }
        ComPtr<IMFMediaType> In;
        MFCreateMediaType(&In);
        In->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        In->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(In.Get(), MF_MT_FRAME_SIZE, W, H);
        MFSetAttributeRatio(In.Get(), MF_MT_FRAME_RATE, kRefresh, 1);
        MFSetAttributeRatio(In.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        In->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        In->SetUINT32(MF_MT_DEFAULT_STRIDE, W);
        hr = Mft->SetInputType(InId, In.Get(), 0);
        if (FAILED(hr))
        {
            DrongLog("  encoder rejected NV12 input (0x%08x)", hr);
            return hr;
        }

        hr = Converter.Init(Dev, Ctx, W, H);
        if (FAILED(hr))
        {
            DrongLog("  D3D11 video processor unavailable (0x%08x)", hr);
            return hr;
        }
        if (!D3DAware)
        {
            D3D11_TEXTURE2D_DESC Sd = {W, H, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            hr = Dev->CreateTexture2D(&Sd, nullptr, &Staging);
            if (FAILED(hr))
                return hr;
        }

        Mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        Mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        Width = W, Height = H;
        return S_OK;
    }

    // Handles the asynchronous MFT's events until Pred() holds or the timeout passes.
    template <typename P> HRESULT PumpEvents(P Pred, DWORD TimeoutMs)
    {
        double T0 = NowMs();
        while (!Pred())
        {
            ComPtr<IMFMediaEvent> Ev;
            HRESULT hr = Events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &Ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE)
            {
                if (NowMs() - T0 > TimeoutMs)
                    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                Sleep(1);
                continue;
            }
            if (FAILED(hr))
                return hr;
            MediaEventType Type = MEUnknown;
            Ev->GetType(&Type);
            if (Type == METransformNeedInput)
                NeedInput++;
            else if (Type == METransformHaveOutput)
            {
                hr = Collect();
                if (FAILED(hr))
                    return hr;
            }
        }
        return S_OK;
    }

    HRESULT MakeInputSample(ComPtr<IMFSample>& Sample)
    {
        HRESULT hr = MFCreateSample(&Sample);
        if (FAILED(hr))
            return hr;
        ComPtr<IMFMediaBuffer> Buf;
        if (D3DAware)
        {
            hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), Converter.Nv12.Get(), 0, FALSE, &Buf);
        }
        else
        {
            // software encoder: read NV12 back from the GPU
            DeviceContext->CopyResource(Staging.Get(), Converter.Nv12.Get());
            D3D11_MAPPED_SUBRESOURCE M;
            hr = DeviceContext->Map(Staging.Get(), 0, D3D11_MAP_READ, 0, &M);
            if (FAILED(hr))
                return hr;
            DWORD Size = Width * Height * 3 / 2;
            hr = MFCreateMemoryBuffer(Size, &Buf);
            BYTE* Dst = nullptr;
            if (SUCCEEDED(hr) && SUCCEEDED(hr = Buf->Lock(&Dst, nullptr, nullptr)))
            {
                const BYTE* Src = (const BYTE*)M.pData;
                for (UINT y = 0; y < Height * 3 / 2; y++) // Y rows, then interleaved UV rows
                    memcpy(Dst + (size_t)y * Width, Src + (size_t)y * M.RowPitch, Width);
                Buf->Unlock();
                Buf->SetCurrentLength(Size);
            }
            DeviceContext->Unmap(Staging.Get(), 0);
        }
        if (SUCCEEDED(hr)) hr = Sample->AddBuffer(Buf.Get());
        if (SUCCEEDED(hr)) hr = Sample->SetSampleTime(Time);
        if (SUCCEEDED(hr)) hr = Sample->SetSampleDuration(10'000'000 / kRefresh);
        Time += 10'000'000 / kRefresh;
        return hr;
    }

    // Pulls one output sample into Au.
    HRESULT Collect()
    {
        MFT_OUTPUT_STREAM_INFO Info = {};
        Mft->GetOutputStreamInfo(OutId, &Info);
        MFT_OUTPUT_DATA_BUFFER Odb = {OutId};
        ComPtr<IMFSample> Own;
        if (!(Info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
        {
            ComPtr<IMFMediaBuffer> Buf;
            MFCreateMemoryBuffer(max<DWORD>(Info.cbSize, Width * Height * 2), &Buf);
            MFCreateSample(&Own);
            Own->AddBuffer(Buf.Get());
            Odb.pSample = Own.Get();
        }
        DWORD Status = 0;
        HRESULT hr = Mft->ProcessOutput(0, 1, &Odb, &Status);
        if (Odb.pEvents)
            Odb.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            // the encoder settled its output type: re-apply it and keep going
            ComPtr<IMFMediaType> T;
            if (SUCCEEDED(Mft->GetOutputAvailableType(OutId, 0, &T)))
                Mft->SetOutputType(OutId, T.Get(), 0);
            return S_OK;
        }
        if (FAILED(hr))
            return hr;
        ComPtr<IMFSample> Out;
        Out.Attach(Odb.pSample);
        if (Own)
            Out->AddRef(); // we still hold our own reference via Own
        ComPtr<IMFMediaBuffer> Buf;
        hr = Out->ConvertToContiguousBuffer(&Buf);
        BYTE* P = nullptr;
        DWORD Len = 0;
        if (SUCCEEDED(hr) && SUCCEEDED(hr = Buf->Lock(&P, nullptr, &Len)))
        {
            Au.insert(Au.end(), P, P + Len);
            Buf->Unlock();
        }
        return hr;
    }

    // Makes sure every IDR frame carries SPS/PPS (some encoders only put them in the media type).
    // Removes filler-data NAL units (type 12). Some encoders pad CBR streams with them; the dongle freezes on them.
    void StripFiller()
    {
        std::vector<size_t> Starts; // offsets of the start codes (including a leading zero of 4-byte codes)
        std::vector<int> Types;
        ForEachNal(Au, [&](int T, size_t Off) {
            Starts.push_back(Off > 0 && Au[Off - 1] == 0 ? Off - 1 : Off);
            Types.push_back(T);
        });
        if (std::find(Types.begin(), Types.end(), 12) == Types.end())
            return;
        std::vector<BYTE> Kept;
        Kept.reserve(Au.size());
        for (size_t i = 0; i < Starts.size(); i++)
        {
            size_t End = i + 1 < Starts.size() ? Starts[i + 1] : Au.size();
            if (Types[i] != 12)
                Kept.insert(Kept.end(), Au.begin() + Starts[i], Au.begin() + End);
        }
        Au.swap(Kept);
    }

    void EnsureHeaders()
    {
        bool HasSps = false, HasIdr = false;
        ForEachNal(Au, [&](int T, size_t) { HasSps |= T == 7; HasIdr |= T == 5; });
        if (HasSps)
        {
            // remember the newest SPS/PPS in case later IDRs come without them
            size_t Start = SIZE_MAX, End = Au.size();
            ForEachNal(Au, [&](int T, size_t Off) {
                if ((T == 7 || T == 8) && Start == SIZE_MAX) Start = Off;
                else if (T != 7 && T != 8 && Start != SIZE_MAX && End == Au.size()) End = Off;
            });
            if (Start != SIZE_MAX)
            {
                size_t S = Start > 0 && Au[Start - 1] == 0 ? Start - 1 : Start;
                size_t E = End < Au.size() && End > 0 && Au[End - 1] == 0 ? End - 1 : End;
                SequenceHeader.assign(Au.begin() + S, Au.begin() + E);
            }
            return;
        }
        if (!HasIdr)
            return;
        if (SequenceHeader.empty())
        {
            ComPtr<IMFMediaType> T;
            UINT32 Size = 0;
            if (SUCCEEDED(Mft->GetOutputCurrentType(OutId, &T)) && SUCCEEDED(T->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &Size)) && Size)
            {
                SequenceHeader.resize(Size);
                T->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, SequenceHeader.data(), Size, nullptr);
            }
        }
        Au.insert(Au.begin(), SequenceHeader.begin(), SequenceHeader.end());
    }

    HRESULT Encode(ID3D11Texture2D* Bgra, bool ForceIdr) override
    {
        HRESULT hr = Converter.Convert(Bgra);
        if (FAILED(hr))
            return hr;
        if (ForceIdr && Codec)
        {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = 1;
            Codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        }
        ComPtr<IMFSample> Sample;
        hr = MakeInputSample(Sample);
        if (FAILED(hr))
            return hr;
        Au.clear();
        if (Async)
        {
            hr = PumpEvents([&] { return NeedInput > 0; }, 200);
            if (FAILED(hr))
                return hr;
            hr = Mft->ProcessInput(InId, Sample.Get(), 0);
            if (FAILED(hr))
                return hr;
            NeedInput--;
            // low-latency mode: the frame comes out before the encoder asks for the next one
            hr = PumpEvents([&] { return !Au.empty(); }, 200);
        }
        else
        {
            hr = Mft->ProcessInput(InId, Sample.Get(), 0);
            if (FAILED(hr))
                return hr;
            hr = Collect();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                hr = S_OK; // software encoder warming up: nothing to send for this frame
        }
        if (FAILED(hr))
            return hr;
        StripFiller();
        EnsureHeaders();
        return S_OK;
    }
};

// ---- selection ---------------------------------------------------------------------------------------------------

// HKLM\SOFTWARE\open-drong-display  Encoder = auto | amf | mf | mf-software  (default auto)
static std::string EncoderSetting()
{
    char Buf[32] = {};
    DWORD Size = sizeof Buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\open-drong-display", "Encoder", RRF_RT_REG_SZ, nullptr, Buf, &Size) != ERROR_SUCCESS)
        return "auto";
    std::string S = Buf;
    for (auto& c : S)
        c = (char)tolower((unsigned char)c);
    return S;
}

// auto: AMF on AMD GPUs (the tested path), otherwise the GPU's Media Foundation encoder, otherwise software.
static std::unique_ptr<FrameEncoder> CreateEncoder(Direct3DDevice& D, UINT W, UINT H)
{
    DXGI_ADAPTER_DESC1 Ad = {};
    D.Adapter->GetDesc1(&Ad);
    std::string Want = EncoderSetting();
    std::vector<std::string> Order;
    if (Want == "amf" || Want == "mf" || Want == "mf-software")
        Order.push_back(Want);
    if (Ad.VendorId == 0x1002)
        Order.push_back("amf");
    Order.push_back("mf");
    Order.push_back("mf-software");
    DrongLog("render GPU vendor 0x%04x, encoder setting '%s'", Ad.VendorId, Want.c_str());

    for (const auto& Kind : Order)
    {
        std::unique_ptr<FrameEncoder> E;
        HRESULT hr;
        if (Kind == "amf")
        {
            auto A = std::make_unique<AmfEncoder>();
            hr = A->Init(D.Device.Get(), W, H);
            E = std::move(A);
        }
        else
        {
            auto M = std::make_unique<MfEncoder>();
            hr = M->Init(D.Device.Get(), D.DeviceContext.Get(), D.AdapterLuid, W, H, Kind == "mf");
            E = std::move(M);
        }
        if (SUCCEEDED(hr))
        {
            DrongLog("encoder: %s, %ux%u", E->Name(), W, H);
            return E;
        }
        DrongLog("encoder %s not available (0x%08x)", Kind.c_str(), hr);
    }
    return nullptr;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD DrongIddDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE DrongIddPrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY DrongIddDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT DrongIddDeviceD0Exit;

EVT_IDD_CX_ADAPTER_INIT_FINISHED DrongIddAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES DrongIddAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION DrongIddParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES DrongIddMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES DrongIddMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN DrongIddMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN DrongIddMonitorUnassignSwapChain;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT  pDriverObject,
    PUNICODE_STRING pRegistryPath
)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    CreateDirectoryW(kLogDir, nullptr);
    BuildEdid(g_Edid);
    DrongLog("DrongIdd driver loaded");

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config,
        DrongIddDeviceAdd
    );

    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return Status;
}

_Use_decl_annotations_
NTSTATUS DrongIddDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = DrongIddPrepareHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry = DrongIddDeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = DrongIddDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    IddConfig.EvtIddCxAdapterInitFinished = DrongIddAdapterInitFinished;

    IddConfig.EvtIddCxParseMonitorDescription = DrongIddParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = DrongIddMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = DrongIddMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = DrongIddAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = DrongIddMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = DrongIddMonitorUnassignSwapChain;

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status))
    {
        DrongLog("IddCxDeviceInitConfig failed 0x%08x", Status);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        DrongLog("WdfDeviceCreate failed 0x%08x", Status);
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);

    // Create a new device context object and attach it to the WDF device object
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    return Status;
}

_Use_decl_annotations_
NTSTATUS DrongIddPrepareHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    NTSTATUS Status = pContext->pContext->Usb.Open(Device);
    DrongLog("USB pipes %s", NT_SUCCESS(Status) ? "ready" : "NOT available");
    return Status;
}

_Use_decl_annotations_
NTSTATUS DrongIddDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();
    pContext->pContext->Usb.Resume();
    pContext->pContext->Session.Start(&pContext->pContext->Usb);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DrongIddDeviceD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->Usb.Close(); // let the frame in flight finish, send nothing after it
    pContext->pContext->Session.Stop();
    DrongLog("device leaving D0");

    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{

}

Direct3DDevice::Direct3DDevice()
{
    AdapterLuid = LUID{};
}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if (FAILED(hr))
    {
        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        return hr;
    }

    // AMF uses the same device from its own threads
    ComPtr<ID3D10Multithread> Mt;
    if (SUCCEEDED(DeviceContext.As(&Mt)))
    {
        Mt->SetMultithreadProtected(TRUE);
    }

    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent, DongleUsb* Usb, DongleSession* Session)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent), m_Usb(Usb), m_Session(Session)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    // Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Alert the swap-chain processing thread to terminate
    SetEvent(m_hTerminateEvent.Get());

    if (m_hThread.Get())
    {
        // Wait for the thread to terminate
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
    // prioritize this thread for improved throughput in high CPU-load scenarios.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    // Media Foundation encoders are COM objects
    HRESULT ComInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 1 ms timer resolution for this process while streaming: with the default 15.6 ms tick the 20 ms keep-alive
    // wait lasts ~31 ms, and the dongle needs a steady ~50 fps on a still screen
    timeBeginPeriod(1);

    RunCore();

    timeEndPeriod(1);
    if (SUCCEEDED(ComInit))
    {
        CoUninitialize();
    }

    // Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
    // provide a new swap-chain if necessary.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;

    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
    // Get the DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        DrongLog("IddCxSwapChainSetDevice failed 0x%08x", hr);
        return;
    }
    DrongLog("swap-chain assigned");

    // Re-send the last image when Windows has nothing new: the dongle shows black unless it gets a steady
    // stream (~10 fps was not enough), so keep it at the monitor's 50 Hz.
    constexpr double KeepAliveMs = 1000.0 / kRefresh;
    // Windows presents at most 50 Hz on this monitor; this only guards against bursts.
    constexpr double MinIntervalMs = 15;

    ComPtr<ID3D11Texture2D> Last; // our copy of the newest frame (Windows owns the swap-chain surfaces)
    std::unique_ptr<FrameEncoder> Enc;
    bool NewFrame = false, WasReady = false;
    double LastSend = 0, LastIdr = 0, StatT = NowMs(), EncSum = 0, UsbSum = 0;
    ULONG64 Frames = 0, Bytes = 0;
    int DumpFrames = 0;

    // Acquire and release buffers in a loop
    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        // Ask for the next buffer from the producer
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        // AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
        if (hr == E_PENDING)
        {
            double Since = NowMs() - LastSend;
            DWORD Timeout = Last && Since < KeepAliveMs ? (DWORD)(KeepAliveMs - Since) + 1 : (Last ? 0 : 100);
            if (Timeout > 0)
            {
                // We must wait for a new buffer
                HANDLE WaitHandles [] =
                {
                    m_hAvailableBufferEvent,
                    m_hTerminateEvent.Get()
                };
                DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, Timeout);
                if (WaitResult == WAIT_OBJECT_0)
                {
                    // We have a new buffer, so try the AcquireBuffer again
                    continue;
                }
                else if (WaitResult == WAIT_OBJECT_0 + 1)
                {
                    // We need to terminate
                    break;
                }
                else if (WaitResult != WAIT_TIMEOUT)
                {
                    // The wait was cancelled or something unexpected happened
                    hr = HRESULT_FROM_WIN32(WaitResult);
                    break;
                }
            }
            // nothing new from Windows: fall through and re-send the last frame (keep-alive)
        }
        else if (SUCCEEDED(hr))
        {
            // We have new frame to process, the surface has a reference on it that the driver has to release
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

            ComPtr<ID3D11Texture2D> Surface;
            if (SUCCEEDED(AcquiredBuffer.As(&Surface)))
            {
                D3D11_TEXTURE2D_DESC Desc;
                Surface->GetDesc(&Desc);
                if (!Last || !Enc || Desc.Width != Enc->Width || Desc.Height != Enc->Height)
                {
                    DrongLog("swap-chain surface %ux%u format %d bind 0x%x misc 0x%x", Desc.Width, Desc.Height,
                             (int)Desc.Format, Desc.BindFlags, Desc.MiscFlags);
                    D3D11_TEXTURE2D_DESC Own = {Desc.Width, Desc.Height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                                D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
                    Last.Reset();
                    if (FAILED(m_Device->Device->CreateTexture2D(&Own, nullptr, &Last)) ||
                        !(Enc = CreateEncoder(*m_Device, Desc.Width, Desc.Height)))
                    {
                        Last.Reset();
                    }
                }
                if (Last)
                {
                    m_Device->DeviceContext->CopyResource(Last.Get(), Surface.Get());
                    NewFrame = true;
                }
            }

            // Release the surface right after the GPU copy so Windows can render the next frame meanwhile.
            AcquiredBuffer.Reset();

            // Indicate to OS that we have finished inital processing of the frame, it is a hint that
            // OS could start preparing another frame
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }
            if (NowMs() - LastSend < MinIntervalMs)
            {
                continue;
            }
        }
        else
        {
            // The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
            DrongLog("swap-chain abandoned 0x%08x", hr);
            break;
        }

        // Encode + send the newest image (new frame, or keep-alive repeat)
        if (!Last)
        {
            continue;
        }
        bool Ready = m_Session->Ready();
        if (!Ready)
        {
            WasReady = false;
            LastSend = NowMs(); // no dongle session yet: just keep consuming frames
            continue;
        }
        double T0 = NowMs();
        // keyframe on (re)connect and every 2 s, so a lost frame never leaves the dongle stuck for long
        bool ForceIdr = !WasReady || m_Session->TakeIdrRequest() || T0 - LastIdr >= 2000;
        if (ForceIdr)
            LastIdr = T0;
        WasReady = true;
        LastSend = T0;
        if (FAILED(Enc->Encode(Last.Get(), ForceIdr)))
        {
            DrongLog("encode failed (%s); re-initialising encoder", Enc->Name());
            UINT W = Enc->Width, H = Enc->Height;
            Enc.reset();
            Enc = CreateEncoder(*m_Device, W, H);
            WasReady = false; // start the new encoder with an IDR
            if (!Enc)
            {
                Last.Reset();
            }
            continue;
        }
        double T1 = NowMs();
        if (!Enc->Au.empty() && !m_Usb->SendFrame(Enc->Au.data(), Enc->Au.size(), Enc->Width, Enc->Height))
        {
            DrongLog("usb frame write failed");
        }
        if (DumpFrames < 150 && !Enc->Au.empty())
        {
            // diagnostics: the first ~3 s of every stream, decodable with ffmpeg
            HANDLE f = CreateFileW(L"C:\\ProgramData\\drong-display\\stream-start.h264", DumpFrames ? FILE_APPEND_DATA : GENERIC_WRITE,
                                   FILE_SHARE_READ, nullptr, DumpFrames ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD w;
                WriteFile(f, Enc->Au.data(), (DWORD)Enc->Au.size(), &w, nullptr);
                CloseHandle(f);
            }
            DumpFrames++;
        }
        double T2 = NowMs();
        Frames++, Bytes += Enc->Au.size(), EncSum += T1 - T0, UsbSum += T2 - T1;
        NewFrame = false;
        if (T2 - StatT >= 30000)
        {
            double Dt = (T2 - StatT) / 1000;
            DrongLog("%.1f fps, %.1f Mbit/s, encode %.1f ms, usb %.1f ms", Frames / Dt, Bytes * 8 / Dt / 1e6,
                     Frames ? EncSum / Frames : 0, Frames ? UsbSum / Frames : 0);
            StatT = T2, Frames = Bytes = 0, EncSum = UsbSum = 0;
        }
    }
    (void)NewFrame;
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
    m_WdfDevice(WdfDevice)
{
    m_Adapter = {};
}

IndirectDeviceContext::~IndirectDeviceContext()
{
    Session.Stop();
}

void IndirectDeviceContext::InitAdapter()
{
    if (m_Adapter)
    {
        // D0Entry also runs when coming back from sleep; the adapter only needs to be created once
        return;
    }

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    // Declare basic feature support for the adapter (required)
    AdapterCaps.MaxMonitorsSupported = 1;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // Declare your device strings for telemetry (required)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"USB-Monitor";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"drong-display";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"Drong USB display dongle";

    // Declare your hardware and firmware versions (required)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    // Initialize a WDF context that can store a pointer to the device context object
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    // Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if (NT_SUCCESS(Status))
    {
        // Store a reference to the WDF adapter handle
        m_Adapter = AdapterInitOut.AdapterObject;

        // Store the device context object into the WDF object context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
    else
    {
        DrongLog("IddCxAdapterInitAsync failed 0x%08x", Status);
    }
}

void IndirectDeviceContext::FinishInit()
{
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = 0;

    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = sizeof g_Edid;
    MonitorInfo.MonitorDescription.pData = g_Edid;

    // Stable id: Windows keeps the monitor's arrangement across replugs
    MonitorInfo.MonitorContainerId = kMonitorContainerId;

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    // Create a monitor object with the specified monitor descriptor
    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (NT_SUCCESS(Status))
    {
        // Create a new monitor context object and attach it to the Idd monitor object
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
        pMonitorContextWrapper->pContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject, this);

        // Tell the OS that the monitor has been plugged in
        IDARG_OUT_MONITORARRIVAL ArrivalOut;
        Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
        DrongLog("USB-Monitor arrival 0x%08x", Status);
    }
    else
    {
        DrongLog("IddCxMonitorCreate failed 0x%08x", Status);
    }
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, IndirectDeviceContext* Device) :
    m_Monitor(Monitor), m_DeviceContext(Device)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
        // swap-chain and try again.
        DrongLog("D3D device init on the render adapter failed");
        WdfObjectDelete(SwapChain);
    }
    else
    {
        // Create a new swap-chain processing thread
        m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent, &m_DeviceContext->Usb,
                                                        &m_DeviceContext->Session));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    // Stop processing the last swap-chain
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS DrongIddAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    // This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
    // to report attached monitors. The dongle is plugged in (that is why this driver is loaded), so report its monitor.

    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        pDeviceContextWrapper->pContext->FinishInit();
    }
    else
    {
        DrongLog("adapter init failed 0x%08x", pInArgs->AdapterInitStatus);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DrongIddAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // Nothing to reconfigure on the dongle: the swap-chain carries the mode and the stream carries the size.
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DrongIddParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    // Our EDID stands for the modes in kModes, 1920x1080 @ 50 Hz preferred.
    pOutArgs->MonitorModeBufferOutputCount = kModeCount;

    if (pInArgs->MonitorModeBufferInputCount < kModeCount)
    {
        // Return success if there was no buffer, since the caller was only asking for a count of modes
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }

    if (pInArgs->MonitorDescription.DataSize != sizeof g_Edid ||
        memcmp(pInArgs->MonitorDescription.pData, g_Edid, sizeof g_Edid) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT i = 0; i < kModeCount; i++)
    {
        pInArgs->pMonitorModes[i] = CreateIddCxMonitorMode(kModes[i].Width, kModes[i].Height, kRefresh,
                                                           IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DrongIddMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // Only used for EDID-less monitors; ours always has an EDID, but answer consistently.
    pOutArgs->DefaultMonitorModeBufferOutputCount = kModeCount;
    if (pInArgs->DefaultMonitorModeBufferInputCount >= kModeCount)
    {
        for (UINT i = 0; i < kModeCount; i++)
        {
            pInArgs->pDefaultMonitorModes[i] = CreateIddCxMonitorMode(kModes[i].Width, kModes[i].Height, kRefresh,
                                                                      IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
        }
        pOutArgs->PreferredMonitorModeIdx = 0;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DrongIddMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // The dongle scales any of these to its HDMI output; 50 Hz keeps its frame queue from growing.
    pOutArgs->TargetModeBufferOutputCount = kModeCount;
    if (pInArgs->TargetModeBufferInputCount >= kModeCount)
    {
        for (UINT i = 0; i < kModeCount; i++)
        {
            pInArgs->pTargetModes[i] = CreateIddCxTargetMode(kModes[i].Width, kModes[i].Height, kRefresh);
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DrongIddMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DrongIddMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
