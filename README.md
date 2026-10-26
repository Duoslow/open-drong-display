# drong-display

An open-source Windows display driver for the **"Drong Display" USB screen dongle** (USB `1CBE:0003`) — a
replacement for the vendor software, which phones home to a server in China, installs a self-signed root
certificate and downloads adverts (see [docs/VENDOR-SOFTWARE.md](docs/VENDOR-SOFTWARE.md)).

Plug the dongle in and Windows gets an extra monitor, **USB-Monitor (1920×1080 @ 50 Hz)**, that behaves like any
other screen: extend the desktop onto it, drag windows to it, put SimHub dashboards on it. Unplug it and the monitor
goes away. No background program, no network access, no vendor code.

> Not affiliated with or endorsed by the maker of the dongle. The USB protocol was reverse-engineered for
> interoperability; see [docs/PROTOCOL.md](docs/PROTOCOL.md).

## How it works

```
Windows desktop compositor
   │  frames for "USB-Monitor"            (IddCx swap-chain, on the GPU)
   ▼
DrongIdd.dll  (UMDF 2 / IddCx display driver, bound to the dongle's USB interface 0)
   │  GPU copy → AMD AMF: BGRA→NV12 → H.264 (constrained baseline, ultra-low-latency, 1 ref frame)
   ▼
USB bulk OUT 0x01  — reverse-engineered protocol, ≤ 50 frames/s
   ▼
dongle (embedded Linux) decodes H.264 → HDMI
```

Measured on a Radeon RX 9060 XT: ~2.7 ms encode + ~1–2 ms USB per frame.

## Requirements

- Windows 10 2004 or newer / Windows 11, x64
- **An AMD Radeon GPU with a hardware video encoder** (the driver encodes with AMD AMF; NVIDIA/Intel are not
  supported yet)
- The dongle: USB ID `1CBE:0003`, shows up as *windows mirroring interface*, *Drong USB Audio Device* and a small
  USB drive with `Drong Display.exe` on it
- To build: Visual Studio 2022 or newer with the C++ desktop workload, git, and the
  **Windows Driver Kit 10.0.26100** (`winget install Microsoft.WindowsWDK.10.0.26100`)

## Install

There are no prebuilt binaries: Windows only loads drivers signed by a certificate it trusts, so the install script
signs the driver on your machine (see [Signing](#signing)).

1. **Do not** run the vendor installer from the dongle's USB drive. If you already did, uninstall *Drong Display*
   and remove the `DrongCert` certificate (Trusted Root and Trusted Publishers).
2. Plug in the dongle.
3. Build (from a normal command prompt):
   ```
   driver\build.bat
   ```
4. Install (from an **administrator** PowerShell):
   ```
   powershell -ExecutionPolicy Bypass -File driver\install.ps1
   ```
   The log is written to `driver\out\install.log`.

USB-Monitor appears a few seconds later. Arrange it in *Settings → System → Display*; Windows remembers its position
across replugs.

To update after changing the code, run both steps again — every install gets a new driver version so Windows
replaces the old one.

### Uninstall

```
powershell -ExecutionPolicy Bypass -File driver\install.ps1 -Uninstall
```

removes the driver package and the signing certificates. The dongle's display interface is then without a driver
until you install one again.

### Optional: hide the dongle's USB drive and audio device

The dongle also exposes a small USB drive (holding the vendor installer) and an audio device. If you don't want them,
disable *USB Mass Storage Device* and *Drong USB Audio Device* under the dongle in Device Manager, or:

```powershell
Get-PnpDevice -PresentOnly | ? InstanceId -match 'VID_1CBE&PID_0003&MI_0[13]' | Disable-PnpDevice -Confirm:$false
```

(Disabled devices are remembered per USB port.)

## Signing

`install.ps1` creates a code-signing certificate (`CN=drong-display local driver signing`), signs the driver and
its catalog, adds the certificate to the machine's *Trusted Root* and *Trusted Publishers* stores and then
**deletes the private key**. Nothing else can ever be signed with that certificate — unlike a vendor certificate
whose key lives somewhere else. Each install replaces the previous certificate; `-Uninstall` removes it.

No test-signing mode is needed: this is a user-mode (UMDF) driver, which Windows checks against the trusted
certificate stores rather than Microsoft's kernel-mode signing requirements.

## Troubleshooting

The driver logs to `C:\ProgramData\drong-display\driver.log` (handshake, device info, stream statistics every 30 s).
It also saves the first ~3 seconds of every stream as `C:\ProgramData\drong-display\stream-start.h264` — play it
with `ffplay` to see exactly what was sent.

| Symptom | Cause / fix |
|---|---|
| Black screen on the dongle's monitor | Check the log for `streaming` and the fps line. If the dongle stopped answering (`usb frame write failed`, `not answering`), unplug it, wait 5 s and plug it back in. |
| Delay grows over time | The dongle drains frames at only ~50/s; the driver's 50 Hz mode keeps below that. Don't raise the refresh rate. |
| USB-Monitor missing | Device Manager → *Display adapters* → *Drong USB Display (USB-Monitor)* should be present without a warning sign. Re-run `install.ps1`. |
| `install.ps1`: *UpdateDriverForPlugAndPlayDevices failed* | The dongle wasn't plugged in. |

## Project layout

| Path | |
|---|---|
| `driver/` | **DrongIdd**, the display driver (IddCx), its INF, `build.bat` and `install.ps1` |
| `docs/PROTOCOL.md` | The dongle's USB protocol and what we learned on real hardware |
| `docs/VENDOR-SOFTWARE.md` | What the vendor installer and driver do |
| `tools/` | Earlier user-mode approach: a sender program + a virtual monitor, and the Python prototype used to work out the protocol — see [tools/README.md](tools/README.md) |
| `third_party/` | AMD AMF SDK headers, fetched by `tools/fetch-amf.cmd` on first build (not committed) |

## License

MIT (see [LICENSE](LICENSE)), except `driver/`, which is derived from Microsoft's
[IndirectDisplay sample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay) and is
distributed under the Microsoft Public License ([driver/LICENSE-IddSample.txt](driver/LICENSE-IddSample.txt)).
AMD AMF headers are fetched from [GPUOpen](https://github.com/GPUOpen-LibrariesAndSDKs/AMF) at build time (MIT).
