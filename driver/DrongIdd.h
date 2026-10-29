#pragma once

// Based on Microsoft's IndirectDisplay sample (Microsoft Public License, see LICENSE-IddSample.txt).

#define NOMINMAX
#define WIN32_NO_STATUS // the NTSTATUS codes come from ntstatus.h via wudfwdm.h
#include <windows.h>
#undef WIN32_NO_STATUS
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <timeapi.h>
#include <powrprof.h>
#include <wrl.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using std::max;
using std::min;

void DrongLog(const char* fmt, ...);

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace Microsoft
{
    namespace IndirectDisp
    {
        /// <summary>
        /// The dongle's vendor interface: bulk OUT for commands/frames, bulk IN for events.
        /// </summary>
        class DongleUsb
        {
        public:
            NTSTATUS Open(WDFDEVICE Device);
            bool Cmd(ULONG Cmd, ULONG A1, ULONG A2, const BYTE* Payload = nullptr, size_t PayloadLen = 0);
            bool SendFrame(const BYTE* Au, size_t Total, ULONG W, ULONG H);
            int ReadEvent(ULONG Ev[6], ULONG TimeoutMs);
            std::vector<BYTE> ReadData(size_t n);
            bool ControlMsg9();
            void Close();
            void Resume();

        private:
            long Xfer(WDFUSBPIPE Pipe, BYTE* Buf, ULONG Len, ULONG TimeoutMs);
            bool WriteLocked(const BYTE* Data, size_t Len, ULONG TimeoutMs);

            WDFUSBDEVICE m_UsbDevice = nullptr;
            WDFUSBPIPE m_In = nullptr, m_Out = nullptr;
            std::mutex m_WriteLock;
            bool m_Closed = false;
            std::vector<BYTE> m_Scratch;
        };

        /// <summary>
        /// Handshake with the dongle and handling of its events, on a background thread.
        /// </summary>
        class DongleSession
        {
        public:
            ~DongleSession() { Stop(); }
            void Start(DongleUsb* Usb);
            void Stop();
            bool Ready() const { return m_Ready; }
            bool TakeIdrRequest() { return m_NeedIdr.exchange(false); }

            // While USB-Monitor shows nothing (switched off, released), the session keeps the dongle fed with
            // these black IDR frames: after a gap in the stream the dongle ignores new frames.
            void StartIdle(std::vector<BYTE> A, std::vector<BYTE> B, ULONG W, ULONG H);
            // Stops the black stream; when it returns no black frame is being or will be sent.
            void StopIdle();

        private:
            bool Handshake();
            void Run();
            void RunLoop();
            void FeedIdle();

            DongleUsb* m_Usb = nullptr;
            std::thread m_Thread;
            std::atomic<bool> m_Quit{false}, m_Ready{false}, m_NeedIdr{false};
            std::mutex m_IdleLock;
            bool m_IdleActive = false;
            std::vector<BYTE> m_Idle[2];
            ULONG m_IdleW = 0, m_IdleH = 0;
            unsigned m_IdleCount = 0;
            double m_IdleLast = 0;
        };

        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            Direct3DDevice();
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object, encodes them and sends
        /// them to the dongle.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent,
                               DongleUsb* Usb, DongleSession* Session);
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            DongleUsb* m_Usb;
            DongleSession* m_Session;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
        };

        /// <summary>
        /// One plugged-in dongle: its IddCx adapter, USB pipes and protocol session.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit();

            DongleUsb Usb;
            DongleSession Session;

        protected:
            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
        };

        class IndirectMonitorContext
        {
        public:
            IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, IndirectDeviceContext* Device);
            virtual ~IndirectMonitorContext();

            void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain();

        private:
            IDDCX_MONITOR m_Monitor;
            IndirectDeviceContext* m_DeviceContext;
            std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
        } ;
    }
}
