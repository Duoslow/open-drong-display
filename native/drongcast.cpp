// drongcast - native low-latency sender for the "Drong Display" USB screen dongle (USB 1CBE:0003).
//
// Pipeline (everything stays on the GPU until the H.264 bitstream):
//   Desktop Duplication of the "USB-Monitor" output (Virtual Display Driver, PnP id MTT1337)
//   -> copy into a render-target texture, draw the mouse cursor with a tiny shader
//   -> AMF VideoConverter BGRA->NV12 -> AMF H.264 encoder (ultra-low-latency, 1 ref frame)
//   -> WinUSB bulk OUT 0x01 using the protocol documented in ../README.md
//
// Build: build.bat (MSVC). No third-party code linked; AMF headers are from AMD's MIT-licensed SDK,
// the AMF runtime (amfrt64.dll) ships with the AMD driver.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Factory.h"
#include "components/VideoConverter.h"
#include "components/VideoEncoderVCE.h"

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- settings / logging

struct Options {
    int bitrate = 15'000'000;
    int fps = 60;
    int keepaliveMs = 100;  // re-send the last frame this often when the screen is static
    double maxFps = 50;     // must stay below the dongle's 60 Hz output, see main loop
    bool sendStop = false;  // CMD_STOP on exit; it seems to leave the dongle unresponsive afterwards
    bool forever = false;
    bool cursor = true;
    bool verbose = false;
    std::wstring monitorId = L"MTT1337";
};
static Options g_opt;
static std::wstring g_stopFile;

static void logf(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
    fflush(stdout);
}

static bool stopRequested() { return GetFileAttributesW(g_stopFile.c_str()) != INVALID_FILE_ATTRIBUTES; }

static double nowMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};
static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char b[256];
        snprintf(b, sizeof b, "%s failed (0x%08lx)", what, (unsigned long)hr);
        throw Fail(b);
    }
}
static void checkAmf(AMF_RESULT r, const char* what) {
    if (r != AMF_OK) {
        char b[256];
        snprintf(b, sizeof b, "%s failed (AMF %d)", what, (int)r);
        throw Fail(b);
    }
}

// ---------------------------------------------------------------- USB transport (WinUSB)

namespace proto {
constexpr uint32_t CMD_INIT = 0x02, CMD_STOP = 0x03, CMD_CURSOR = 0x63, CMD_H264 = 0x67, CMD_NONCE_REPLY = 0x58,
                   CMD_CHECK = 0x74, CMD_GET_INFO = 0x77;
constexpr uint32_t EV_NONCE_REQ = 0x57, EV_CHECK_REPLY = 0x75, EV_INFO = 0x77, EV_CONNECTED = 0xA0, EV_DISCONNECTED = 0xA1;
constexpr UCHAR EP_OUT = 0x01, EP_IN = 0x81;
constexpr size_t PKT = 512, CHUNK = 0xA0000;
}  // namespace proto

// {F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}: interface GUID registered by the generic "WinUsb Device" install
static const GUID kWinUsbIface = {0xF72FE0D4, 0xCBCB, 0x407D, {0x88, 0x14, 0x9E, 0xD6, 0x73, 0xD0, 0xDD, 0x6B}};

static void putBE(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
static uint32_t getBE(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

class Dongle {
public:
    ~Dongle() { close(); }

    void open() {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, (GUID*)&kWinUsbIface, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2)
            throw Fail("dongle not found (no WinUSB device present)");
        std::vector<wchar_t> list(len);
        CM_Get_Device_Interface_ListW((GUID*)&kWinUsbIface, nullptr, list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        std::wstring path;
        for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1) {
            std::wstring s = p;
            for (auto& c : s) c = towlower(c);
            if (s.find(L"vid_1cbe&pid_0003&mi_00") != std::wstring::npos) path = p;
        }
        if (path.empty()) throw Fail("dongle not found (1CBE:0003)");
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw Fail("cannot open dongle (in use by another program?)");
        if (!WinUsb_Initialize(file_, &usb_)) throw Fail("WinUsb_Initialize failed");
    }

    void close() {
        if (usb_) WinUsb_Free(usb_), usb_ = nullptr;
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_), file_ = INVALID_HANDLE_VALUE;
    }

    // Returns bytes transferred, or -1 on timeout; throws on errors.
    long xfer(UCHAR pipe, uint8_t* buf, ULONG len, DWORD timeoutMs) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = (pipe & 0x80) ? WinUsb_ReadPipe(usb_, pipe, buf, len, nullptr, &ov)
                                : WinUsb_WritePipe(usb_, pipe, buf, len, nullptr, &ov);
        DWORD err = ok ? 0 : GetLastError();
        if (!ok && err != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            throw Fail("usb transfer failed (device gone?)");
        }
        ULONG n = 0;
        if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_TIMEOUT) {
            WinUsb_AbortPipe(usb_, pipe);
            WinUsb_GetOverlappedResult(usb_, &ov, &n, TRUE);
            CloseHandle(ov.hEvent);
            return -1;
        }
        ok = WinUsb_GetOverlappedResult(usb_, &ov, &n, FALSE);
        err = ok ? 0 : GetLastError();
        CloseHandle(ov.hEvent);
        if (!ok) {
            if (err == ERROR_SEM_TIMEOUT || err == ERROR_OPERATION_ABORTED) return -1;
            throw Fail("usb transfer failed (device gone?)");
        }
        return (long)n;
    }

    // Writes data zero-padded to a multiple of 512 bytes, like the vendor driver.
    void write(const uint8_t* data, size_t len, DWORD timeoutMs = 3000) {
        size_t padded = (len + proto::PKT - 1) / proto::PKT * proto::PKT;
        if (padded == 0) padded = proto::PKT;
        if (scratch_.size() < padded) scratch_.resize(padded);
        memcpy(scratch_.data(), data, len);
        memset(scratch_.data() + len, 0, padded - len);
        if (xfer(proto::EP_OUT, scratch_.data(), (ULONG)padded, timeoutMs) < 0) throw Fail("usb write timed out (dongle stalled)");
    }

    void cmd(uint32_t c, uint32_t a1, uint32_t a2, const uint8_t* payload = nullptr, size_t plen = 0) {
        std::lock_guard<std::mutex> g(writeLock_);
        uint8_t h[24] = {};
        putBE(h, c), putBE(h + 4, a1), putBE(h + 12, a2);
        write(h, sizeof h);
        if (payload) write(payload, plen);
    }

    void sendFrame(const uint8_t* au, size_t total, uint32_t w, uint32_t h) {
        std::lock_guard<std::mutex> g(writeLock_);
        uint32_t n = (uint32_t)std::max<size_t>(1, (total + proto::CHUNK - 1) / proto::CHUNK);
        for (uint32_t i = 0; i < n; i++) {
            size_t off = i * proto::CHUNK, clen = std::min(proto::CHUNK, total - off);
            uint8_t hdr[64] = {};
            putBE(hdr, proto::CMD_H264), putBE(hdr + 4, 2), putBE(hdr + 12, (uint32_t)clen);
            uint32_t sub[7] = {(uint32_t)total, (uint32_t)clen, n, i + 1, 1, w, h};
            for (int k = 0; k < 7; k++) putBE(hdr + 24 + 4 * k, sub[k]);
            write(hdr, sizeof hdr);
            write(au + off, clen);
        }
    }

    // Reads one 512-byte event packet; false on timeout / empty.
    bool readEvent(uint32_t ev[6], DWORD timeoutMs) {
        uint8_t b[proto::PKT];
        long n = xfer(proto::EP_IN, b, sizeof b, timeoutMs);
        if (n < 24) return false;
        for (int i = 0; i < 6; i++) ev[i] = getBE(b + 4 * i);
        return ev[0] != 0;
    }

    std::vector<uint8_t> readData(size_t n) {
        std::vector<uint8_t> out;
        uint8_t b[4096];
        while (out.size() < n) {
            size_t want = std::min(sizeof b, (n - out.size() + proto::PKT - 1) / proto::PKT * proto::PKT);
            long got = xfer(proto::EP_IN, b, (ULONG)want, 2000);
            if (got <= 0) break;
            out.insert(out.end(), b, b + got);
        }
        out.resize(std::min(out.size(), n));
        return out;
    }

    bool controlMsg9(uint8_t* reply) {
        WINUSB_SETUP_PACKET sp{0xA1, 9, 0, 0, 1};
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ULONG n = 0;
        BOOL ok = WinUsb_ControlTransfer(usb_, sp, reply, 1, nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 3000) == WAIT_TIMEOUT) {
                WinUsb_AbortPipe(usb_, 0);
                WinUsb_GetOverlappedResult(usb_, &ov, &n, TRUE);
                CloseHandle(ov.hEvent);
                return false;
            }
            ok = WinUsb_GetOverlappedResult(usb_, &ov, &n, FALSE);
        }
        CloseHandle(ov.hEvent);
        return ok && n == 1;
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb_ = nullptr;
    std::mutex writeLock_;
    std::vector<uint8_t> scratch_;
};

static void randomBytes(uint8_t* p, size_t n) {
    static uint64_t s = (uint64_t)nowMs() * 6364136223846793005ULL ^ GetCurrentProcessId();
    for (size_t i = 0; i < n; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = uint8_t(s >> 56);
    }
}

// Device session: handshake + event thread (connected / info keep-alive / nonce / disconnected).
class Session {
public:
    explicit Session(Dongle& d) : d_(d) {}
    ~Session() { stopEvents(); }

    void handshake() {
        uint8_t r = 0;
        if (!d_.controlMsg9(&r)) throw Fail("dongle not answering (control request)");
        Sleep(5);
        uint8_t nonce[16];
        randomBytes(nonce, sizeof nonce);
        d_.cmd(proto::CMD_CHECK, 1, 16, nonce, sizeof nonce);
        double t = nowMs();
        uint32_t ev[6];
        bool gotCheck = false;
        while (nowMs() - t < 3000) {
            bool got = d_.readEvent(ev, 100);
            if (got && g_opt.verbose) logf("<- handshake event 0x%02x (%u, %u, %u)", ev[0], ev[1], ev[2], ev[3]);
            if (got && ev[0] == proto::EV_CHECK_REPLY) {
                d_.readData(ev[3]);
                gotCheck = true;
                break;
            }
        }
        if (!gotCheck) throw Fail("dongle not answering (no check reply)");
        d_.cmd(proto::CMD_INIT, 1, 0);
        events_ = std::thread([this] { eventLoop(); });
    }

    bool waitReady(DWORD ms) {
        double t = nowMs();
        while (nowMs() - t < ms) {
            if (ready_) return true;
            if (dead_) return false;
            Sleep(10);
        }
        return ready_;
    }

    bool alive() const { return !dead_; }
    bool ready() const { return ready_; }

    void stopEvents() {
        quit_ = true;
        if (events_.joinable()) events_.join();
    }

private:
    void eventLoop() {
        std::string lastInfo;
        try {
            while (!quit_) {
                uint32_t ev[6];
                if (!d_.readEvent(ev, 100)) continue;
                if (g_opt.verbose) logf("<- event 0x%02x (%u, %u, %u)", ev[0], ev[1], ev[2], ev[3]);
                switch (ev[0]) {
                case proto::EV_CONNECTED:
                    d_.cmd(proto::CMD_GET_INFO, 1, 0);
                    break;
                case proto::EV_INFO: {
                    auto raw = d_.readData(ev[3]);
                    std::string info(raw.begin(), raw.end());
                    if (info != lastInfo) logf("device info: %s", info.c_str());
                    lastInfo = info;
                    ready_ = true;
                    break;
                }
                case proto::EV_NONCE_REQ: {
                    uint8_t n[16];
                    randomBytes(n, sizeof n);
                    d_.cmd(proto::CMD_NONCE_REPLY, 1, 16, n, sizeof n);
                    break;
                }
                case proto::EV_DISCONNECTED:
                    logf("dongle reports monitor disconnected");
                    ready_ = false;
                    break;
                default:
                    if (g_opt.verbose) logf("event 0x%02x (%u, %u)", ev[0], ev[1], ev[3]);
                    if (ev[3] && ev[3] < (1u << 20)) d_.readData(ev[3]);  // e.g. 0x71: firmware URL, ignored
                }
            }
        } catch (const std::exception& e) {
            logf("usb event thread: %s", e.what());
            dead_ = true;
        }
    }

    Dongle& d_;
    std::thread events_;
    std::atomic<bool> quit_{false}, ready_{false}, dead_{false};
};

// ---------------------------------------------------------------- capture (Desktop Duplication)

// GDI name (\\.\DISPLAYn) of the monitor whose PnP id contains `id`.
static std::wstring findDisplay(const std::wstring& id) {
    std::wstring want = id;
    for (auto& c : want) c = towlower(c);
    DISPLAY_DEVICEW ad{sizeof ad};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &ad, 0); i++, ad = {sizeof ad}) {
        DISPLAY_DEVICEW mon{sizeof mon};
        for (DWORD j = 0; EnumDisplayDevicesW(ad.DeviceName, j, &mon, 0); j++, mon = {sizeof mon}) {
            std::wstring mid = mon.DeviceID;
            for (auto& c : mid) c = towlower(c);
            if (mid.find(want) != std::wstring::npos && (ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) return ad.DeviceName;
        }
    }
    return {};
}

static const char* kCursorShader = R"(
cbuffer R : register(b0) { float4 rect; };   // x0, y0, x1, y1 in NDC
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD; };
V vs(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    V o;
    o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1);
    o.uv = t;
    return o;
}
float4 ps(V i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

class Capture {
public:
    UINT width = 0, height = 0;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;

    void init() {
        std::wstring gdi = findDisplay(g_opt.monitorId);
        if (gdi.empty()) throw Fail("USB-Monitor (Virtual Display Driver) not found or not active");
        ComPtr<IDXGIFactory1> f;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&f)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> ad;
        for (UINT a = 0; !output_ && f->EnumAdapters1(a, &ad) == S_OK; a++) {
            ComPtr<IDXGIOutput> o;
            for (UINT k = 0; ad->EnumOutputs(k, &o) == S_OK; k++) {
                DXGI_OUTPUT_DESC d;
                o->GetDesc(&d);
                if (gdi == d.DeviceName) {
                    check(o.As(&output_), "IDXGIOutput5");
                    adapter_ = ad;
                    break;
                }
            }
        }
        if (!output_) throw Fail("USB-Monitor output not found in DXGI");
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        check(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &fl, 1,
                                D3D11_SDK_VERSION, &dev, nullptr, &ctx),
              "D3D11CreateDevice");
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);  // AMF uses the same device
        initCursorPipeline();
        duplicate();
    }

    void duplicate() {
        dup_.Reset();
        DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        check(output_->DuplicateOutput1(dev.Get(), 0, 1, &fmt, &dup_), "DuplicateOutput1");
        DXGI_OUTDUPL_DESC d;
        dup_->GetDesc(&d);
        if (d.ModeDesc.Width != width || d.ModeDesc.Height != height) {
            width = d.ModeDesc.Width, height = d.ModeDesc.Height;
            D3D11_TEXTURE2D_DESC td{width, height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
            desk_.Reset(), out_.Reset(), rtv_.Reset();
            check(dev->CreateTexture2D(&td, nullptr, &desk_), "CreateTexture2D desk");
            check(dev->CreateTexture2D(&td, nullptr, &out_), "CreateTexture2D out");
            check(dev->CreateRenderTargetView(out_.Get(), nullptr, &rtv_), "CreateRenderTargetView");
            const float black[4] = {0, 0, 0, 1};
            ctx->ClearRenderTargetView(rtv_.Get(), black);
            ctx->CopyResource(desk_.Get(), out_.Get());
        }
    }

    // Waits up to timeoutMs for a desktop or pointer change. Returns true when `out()` holds a new image.
    bool next(DWORD timeoutMs) {
        DXGI_OUTDUPL_FRAME_INFO fi;
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            // mode change, secure desktop (UAC), display restart: re-create the duplication
            Sleep(50);
            duplicate();
            return false;
        }
        check(hr, "AcquireNextFrame");
        bool changed = false;
        if (fi.LastPresentTime.QuadPart) {
            ComPtr<ID3D11Texture2D> t;
            res.As(&t);
            ctx->CopyResource(desk_.Get(), t.Get());
            changed = true;
        }
        if (fi.LastMouseUpdateTime.QuadPart) {
            if (fi.PointerPosition.Visible != ptrVisible_ || fi.PointerPosition.Position.x != ptrPos_.x ||
                fi.PointerPosition.Position.y != ptrPos_.y)
                changed = true;
            ptrVisible_ = fi.PointerPosition.Visible;
            ptrPos_ = fi.PointerPosition.Position;
            if (fi.PointerShapeBufferSize) {
                updateCursorShape(fi.PointerShapeBufferSize);
                changed = true;
            }
        }
        dup_->ReleaseFrame();
        if (changed) compose();
        return changed;
    }

    ID3D11Texture2D* out() { return out_.Get(); }

private:
    void initCursorPipeline() {
        ComPtr<ID3DBlob> vsb, psb, err;
        check(D3DCompile(kCursorShader, strlen(kCursorShader), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err), "compile vs");
        check(D3DCompile(kCursorShader, strlen(kCursorShader), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err), "compile ps");
        check(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_), "CreateVertexShader");
        check(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_), "CreatePixelShader");
        D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        check(dev->CreateBuffer(&bd, nullptr, &cb_), "CreateBuffer");
        D3D11_BLEND_DESC bl{};
        auto& rt = bl.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA, rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE, rt.DestBlendAlpha = D3D11_BLEND_ZERO, rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(dev->CreateBlendState(&bl, &blend_), "CreateBlendState");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        check(dev->CreateSamplerState(&sd, &smp_), "CreateSamplerState");
    }

    // Converts the duplication pointer shape to a straight-alpha BGRA texture.
    void updateCursorShape(UINT size) {
        std::vector<uint8_t> buf(size);
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
        UINT got = 0;
        if (FAILED(dup_->GetFramePointerShape(size, buf.data(), &got, &si))) return;
        UINT w = si.Width, h = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? si.Height / 2 : si.Height;
        std::vector<uint32_t> px(w * h);
        for (UINT y = 0; y < h; y++) {
            for (UINT x = 0; x < w; x++) {
                uint32_t& o = px[y * w + x];
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                    bool andBit = buf[y * si.Pitch + x / 8] & (0x80 >> (x % 8));
                    bool xorBit = buf[(y + h) * si.Pitch + x / 8] & (0x80 >> (x % 8));
                    // and=1,xor=0 transparent; 0/0 black; 0/1 white; 1/1 "invert" (drawn white: dashboards are dark)
                    o = andBit && !xorBit ? 0x00000000 : (xorBit ? 0xFFFFFFFF : 0xFF000000);
                } else {
                    uint32_t c = *(const uint32_t*)&buf[y * si.Pitch + x * 4];
                    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR)
                        o = (c >> 24) ? ((c & 0xFFFFFF) ? (c | 0xFF000000) : 0) : (c | 0xFF000000);  // mask=XOR: approximate
                    else
                        o = c;
                }
            }
        }
        D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(dev->CreateTexture2D(&td, &init, &t))) return;
        cursorSrv_.Reset();
        dev->CreateShaderResourceView(t.Get(), nullptr, &cursorSrv_);
        cursorW_ = w, cursorH_ = h;
    }

    void compose() {
        ctx->CopyResource(out_.Get(), desk_.Get());
        if (!g_opt.cursor || !ptrVisible_ || !cursorSrv_) return;
        float x0 = 2.f * ptrPos_.x / width - 1, y0 = 1 - 2.f * ptrPos_.y / height;
        float x1 = 2.f * (ptrPos_.x + (int)cursorW_) / width - 1, y1 = 1 - 2.f * (ptrPos_.y + (int)cursorH_) / height;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        float r[4] = {x0, y0, x1, y1};
        memcpy(m.pData, r, sizeof r);
        ctx->Unmap(cb_.Get(), 0);
        D3D11_VIEWPORT vp{0, 0, (float)width, (float)height, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        ctx->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, cursorSrv_.GetAddressOf());
        ctx->PSSetSamplers(0, 1, smp_.GetAddressOf());
        ctx->Draw(4, 0);
        ID3D11RenderTargetView* none = nullptr;
        ctx->OMSetRenderTargets(1, &none, nullptr);
    }

    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput5> output_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> desk_, out_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11SamplerState> smp_;
    ComPtr<ID3D11ShaderResourceView> cursorSrv_;
    UINT cursorW_ = 0, cursorH_ = 0;
    BOOL ptrVisible_ = FALSE;
    POINT ptrPos_{};
};

// ---------------------------------------------------------------- encoder (AMD AMF)

class Encoder {
public:
    void init(ID3D11Device* dev, UINT w, UINT h) {
        static HMODULE lib = LoadLibraryW(AMF_DLL_NAME);
        if (!lib) throw Fail("amfrt64.dll not found (AMD driver missing?)");
        auto init = (AMFInit_Fn)GetProcAddress(lib, AMF_INIT_FUNCTION_NAME);
        auto query = (AMFQueryVersion_Fn)GetProcAddress(lib, AMF_QUERY_VERSION_FUNCTION_NAME);
        amf_uint64 rt = 0;
        if (query) query(&rt);
        checkAmf(init(std::min<amf_uint64>(rt ? rt : AMF_FULL_VERSION, AMF_FULL_VERSION), &factory_), "AMFInit");
        checkAmf(factory_->CreateContext(&ctx_), "CreateContext");
        checkAmf(ctx_->InitDX11(dev), "InitDX11");

        checkAmf(factory_->CreateComponent(ctx_, AMFVideoConverter, &conv_), "create converter");
        conv_->SetProperty(AMF_VIDEO_CONVERTER_MEMORY_TYPE, amf::AMF_MEMORY_DX11);
        conv_->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, amf::AMF_SURFACE_NV12);
        conv_->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_SIZE, AMFConstructSize((amf_int32)w, (amf_int32)h));
        conv_->SetProperty(AMF_VIDEO_CONVERTER_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
        checkAmf(conv_->Init(amf::AMF_SURFACE_BGRA, (amf_int32)w, (amf_int32)h), "converter Init");

        checkAmf(factory_->CreateComponent(ctx_, AMFVideoEncoderVCE_AVC, &enc_), "create H.264 encoder");
        enc_->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
        enc_->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_CONSTRAINED_BASELINE);
        enc_->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize((amf_int32)w, (amf_int32)h));
        enc_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate((amf_uint32)g_opt.fps, 1));
        enc_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
        enc_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)g_opt.bitrate);
        enc_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, (amf_int64)g_opt.bitrate * 2);
        enc_->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, (amf_int64)g_opt.bitrate / g_opt.fps * 2);  // ~2 frames
        enc_->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
        enc_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
        enc_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64)0);
        // 1 reference frame -> SPS max_dec_frame_buffering=1: the dongle's decoder shows each frame immediately
        enc_->SetProperty(AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, (amf_int64)1);
        enc_->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        enc_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64)g_opt.fps * 2);
        enc_->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, (amf_int64)g_opt.fps * 2);
        enc_->SetProperty(AMF_VIDEO_ENCODER_CABAC_ENABLE, AMF_VIDEO_ENCODER_CALV);
        enc_->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, (amf_int64)50);
        checkAmf(enc_->Init(amf::AMF_SURFACE_NV12, (amf_int32)w, (amf_int32)h), "encoder Init");
        forceIdr_ = true;
    }

    ~Encoder() {
        if (enc_) enc_->Terminate();
        if (conv_) conv_->Terminate();
        if (ctx_) ctx_->Terminate();
    }

    void requestIdr() { forceIdr_ = true; }

    // Encodes one BGRA texture synchronously; returns the access unit (Annex-B).
    const std::vector<uint8_t>& encode(ID3D11Texture2D* tex) {
        amf::AMFSurfacePtr in;
        checkAmf(ctx_->CreateSurfaceFromDX11Native(tex, &in, nullptr), "CreateSurfaceFromDX11Native");
        checkAmf(conv_->SubmitInput(in), "converter SubmitInput");
        amf::AMFDataPtr nv12;
        for (int i = 0; i < 100 && !nv12; i++) {
            AMF_RESULT r = conv_->QueryOutput(&nv12);
            if (r != AMF_OK && r != AMF_REPEAT) checkAmf(r, "converter QueryOutput");
            if (!nv12) Sleep(0);
        }
        if (!nv12) throw Fail("converter produced no output");
        if (forceIdr_) {
            nv12->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            nv12->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            nv12->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
            forceIdr_ = false;
        }
        AMF_RESULT r;
        while ((r = enc_->SubmitInput(nv12)) == AMF_INPUT_FULL) drain(false);
        checkAmf(r, "encoder SubmitInput");
        au_.clear();
        drain(true);
        return au_;
    }

private:
    void drain(bool waitOne) {
        for (int tries = 0; tries < 20; tries++) {
            amf::AMFDataPtr out;
            AMF_RESULT r = enc_->QueryOutput(&out);  // blocks up to QUERY_TIMEOUT
            if (out) {
                amf::AMFBufferPtr b(out);
                auto p = (const uint8_t*)b->GetNative();
                au_.assign(p, p + b->GetSize());
                return;
            }
            if (!waitOne) return;
            if (r != AMF_OK && r != AMF_REPEAT) checkAmf(r, "encoder QueryOutput");
        }
        throw Fail("encoder produced no output");
    }

    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContextPtr ctx_;
    amf::AMFComponentPtr conv_, enc_;
    std::vector<uint8_t> au_;
    bool forceIdr_ = true;
};

// ---------------------------------------------------------------- main loop

static void runSession() {
    Capture cap;
    cap.init();
    logf("capturing USB-Monitor %ux%u", cap.width, cap.height);

    Dongle usb;
    usb.open();
    Session s(usb);
    s.handshake();
    logf("connected; waiting for device info");
    // After a previous session the dongle answers the handshake but does not announce "monitor connected"
    // again, so ask for the info ourselves and stream anyway if it stays quiet.
    if (!s.waitReady(1500)) {
        usb.cmd(proto::CMD_GET_INFO, 1, 0);
        if (!s.waitReady(1500)) logf("no device info (dongle still in previous session?) - streaming anyway");
    }
    if (!s.alive()) throw Fail("usb connection lost during handshake");

    Encoder enc;
    enc.init(cap.dev.Get(), cap.width, cap.height);
    UINT encW = cap.width, encH = cap.height;
    usb.cmd(proto::CMD_CURSOR, 2, 28, std::vector<uint8_t>(28).data(), 28);  // hide the dongle's own cursor

    double lastSend = 0, statT = nowMs(), latSum = 0, latMax = 0, encSum = 0, usbSum = 0;
    uint64_t frames = 0, bytes = 0, fresh = 0;
    bool first = true, dirty = false;
    double tCap = 0, minInterval = 1000.0 / g_opt.maxFps;
    try {
        while (!stopRequested()) {
            if (!s.alive()) throw Fail("usb connection lost");
            // The dongle queues frames and shows them at its own 60 Hz: sending faster makes the queue
            // (and the delay) grow without bound, so cap the rate below that and always send the newest image.
            double since = nowMs() - lastSend;
            if (dirty) {
                // a change is pending: wait out the rest of the frame gap, then pick up the newest image
                // (AcquireNextFrame would block until the next present, overshooting the gap)
                double wait = minInterval - since;
                if (wait >= 1) Sleep((DWORD)wait);
                cap.next(0);
            } else if (cap.next((DWORD)std::max(1.0, g_opt.keepaliveMs - since))) {
                tCap = nowMs();
                dirty = true;
            }
            if (cap.width != encW || cap.height != encH) throw Fail("USB-Monitor resolution changed; restarting");
            since = nowMs() - lastSend;
            if (!first && !(dirty && since >= minInterval) && since < g_opt.keepaliveMs) continue;
            bool changed = dirty;
            dirty = false;
            lastSend = nowMs();  // pace on frame starts, so encode+usb time does not stretch the interval
            const auto& au = enc.encode(cap.out());
            if (au.empty()) continue;
            double tEnc = nowMs();
            usb.sendFrame(au.data(), au.size(), encW, encH);
            double tDone = nowMs();
            first = false;
            frames++, bytes += au.size(), encSum += tEnc - lastSend, usbSum += tDone - tEnc;
            if (changed) {
                double l = tDone - tCap;
                fresh++, latSum += l, latMax = std::max(latMax, l);
            }
            if (tDone - statT >= 5000) {
                double dt = (tDone - statT) / 1000;
                logf("%.1f fps (%.1f new), %.1f Mbit/s | change->sent avg %.1f ms, max %.1f ms | encode %.1f ms, usb %.1f ms",
                     frames / dt, fresh / dt, bytes * 8 / dt / 1e6, fresh ? latSum / fresh : 0.0, latMax,
                     encSum / frames, usbSum / frames);
                statT = tDone, frames = bytes = fresh = 0, latSum = latMax = encSum = usbSum = 0;
            }
        }
        logf("stop requested");
    } catch (...) {
        if (g_opt.sendStop) try { usb.cmd(proto::CMD_STOP, 1, 0); } catch (...) {}
        s.stopEvents();
        throw;
    }
    if (g_opt.sendStop) try { usb.cmd(proto::CMD_STOP, 1, 0); } catch (...) {}
    s.stopEvents();
}

static void usage() {
    printf(
        "drongcast - native sender for the Drong USB display dongle\n"
        "  --forever          reconnect automatically (unplug, hiccups, display changes)\n"
        "  --bitrate MBPS     H.264 bitrate in Mbit/s (default 15)\n"
        "  --fps N            nominal encoder frame rate (default 60)\n"
        "  --max-fps N        send at most N frames/s (default 50; >= 60 makes the dongle's queue grow)\n"
        "  --send-stop        send the stop command on exit\n"
        "  --no-cursor        do not draw the mouse cursor\n"
        "  --monitor ID       PnP id of the monitor to send (default MTT1337 = USB-Monitor)\n"
        "  -v                 verbose\n"
        "Create a file named 'stop' next to the exe to stop cleanly.\n");
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--forever") g_opt.forever = true;
        else if (a == L"--bitrate") g_opt.bitrate = (int)(_wtof(next().c_str()) * 1e6);
        else if (a == L"--fps") g_opt.fps = _wtoi(next().c_str());
        else if (a == L"--max-fps") g_opt.maxFps = _wtof(next().c_str());
        else if (a == L"--send-stop") g_opt.sendStop = true;
        else if (a == L"--no-cursor") g_opt.cursor = false;
        else if (a == L"--monitor") g_opt.monitorId = next();
        else if (a == L"-v") g_opt.verbose = true;
        else { usage(); return a == L"-h" || a == L"--help" ? 0 : 2; }
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_stopFile = exe;
    g_stopFile = g_stopFile.substr(0, g_stopFile.find_last_of(L'\\') + 1) + L"stop";
    DeleteFileW(g_stopFile.c_str());
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    int rc = 0;
    do {
        try {
            runSession();
            if (stopRequested()) break;
        } catch (const std::exception& e) {
            logf("%s", e.what());
            rc = 1;
        }
        if (!g_opt.forever || stopRequested()) break;
        for (int i = 0; i < 30 && !stopRequested(); i++) Sleep(100);
    } while (!stopRequested());
    DeleteFileW(g_stopFile.c_str());
    return stopRequested() ? 0 : rc;
}
