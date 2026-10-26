# open-drong-display

An open-source Windows display driver for the **"Drong Display" USB screen dongle** (USB `1CBE:0003`) — a
replacement for the vendor software, which sends device and PC information to the vendor's server
(`ds.dronghootin.com.cn`), installs a self-signed root certificate and downloads adverts
(see [docs/VENDOR-SOFTWARE.md](docs/VENDOR-SOFTWARE.md)).

Plug the dongle in and Windows gets an extra monitor, **USB-Monitor (1920×1080 @ 50 Hz)**, that behaves like any
other screen. Unplug it and the monitor goes away. No background program, no network access, no vendor code.

> Not affiliated with or endorsed by the maker of the dongle. The USB protocol was reverse-engineered for
> interoperability; see [docs/PROTOCOL.md](docs/PROTOCOL.md).

## How it works

```
Windows desktop compositor
   │  frames for "USB-Monitor"            (IddCx swap-chain, on the GPU)
   ▼
DrongIdd.dll  (UMDF 2 / IddCx display driver, bound to the dongle's USB interface 0)
   │  GPU copy → H.264 (constrained baseline, low latency, 1 ref frame) with
   │    AMD GPUs:     AMD AMF
   │    other GPUs:   D3D11 video processor (BGRA→NV12) → the GPU's Media Foundation encoder
   │    no hardware:  Microsoft's software H.264 encoder
   ▼
USB bulk OUT 0x01  — reverse-engineered protocol, ≤ 50 frames/s
   ▼
dongle (embedded Linux) decodes H.264 → HDMI
```

Measured on a Radeon RX 9060 XT with AMF: ~2.7 ms encode + ~1–2 ms USB per frame.

## Requirements

- Windows 10 2004 or newer / Windows 11, x64
- A GPU with a hardware H.264 encoder — AMD (tested), NVIDIA or Intel (should work, untested — please report).
  Without one, the driver falls back to software encoding on the CPU. See [Video encoders](#video-encoders).
- The dongle: USB ID `1CBE:0003`, shows up as *windows mirroring interface*, *Drong USB Audio Device* and a small
  USB drive with `Drong Display.exe` on it
- To build: Visual Studio 2022 or newer with the C++ desktop workload, git, and the
  **Windows Driver Kit 10.0.26100** (`winget install Microsoft.WindowsWDK.10.0.26100`)

## Install

Windows only loads drivers signed by a certificate it trusts, so the install script signs the driver on your machine
(see [Signing](#signing)); it uses the signing tools from the Windows Driver Kit either way.

1. **Do not** run the vendor installer from the dongle's USB drive. If you already did, uninstall *Drong Display*
   and remove the `DrongCert` certificate (Trusted Root and Trusted Publishers).
2. Plug in the dongle.
3. Get the driver:
   - **prebuilt:** download the zip from the [latest release](https://github.com/Duoslow/open-drong-display/releases/latest)
     and unzip it, or
   - **from source** (needs Visual Studio), from a normal command prompt:
     ```
     driver\build.bat
     ```
4. Install (from an **administrator** PowerShell, in the unzipped/cloned folder):
   ```
   powershell -ExecutionPolicy Bypass -File driver\install.ps1
   ```
   The log is written to `driver\out\install.log`.

USB-Monitor appears a few seconds later. Arrange it in *Settings → System → Display*; Windows remembers its position
across replugs.

To update, run steps 3 and 4 again — every install gets a new driver version so Windows
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

## Video encoders

The driver encodes on the GPU that renders USB-Monitor and picks, in order:

| | Used on | Status |
|---|---|---|
| **AMD AMF** | AMD Radeon | tested (RX 9060 XT) |
| **Media Foundation, hardware** — the GPU vendor's H.264 encoder (NVENC, Quick Sync, VCN) through Windows' standard encoder interface | NVIDIA, Intel, or AMD if AMF fails | tested on AMD; NVIDIA/Intel untested |
| **Media Foundation, software** — Microsoft's H.264 encoder, on the CPU | any PC | tested; uses noticeable CPU |

The log shows the choice (`encoder: …`). To force one, set a string value and replug the dongle (or restart the
device in Device Manager):

```
reg add "HKLM\SOFTWARE\open-drong-display" /v Encoder /t REG_SZ /d mf          (amf | mf | mf-software | auto)
reg delete "HKLM\SOFTWARE\open-drong-display" /v Encoder                        (back to automatic)
```

If the forced encoder isn't available the driver falls back to the next one.

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
| Picture freezes with a non-AMD encoder | Please open an issue with `driver.log` and `stream-start.h264` attached. As a workaround, force `mf-software` (see [Video encoders](#video-encoders)). |

## Alternative usage

Can't or don't want to install a driver? [`alternative/`](alternative/README.md) has a user-mode sender program
(with an optional virtual monitor) that drives the dongle through Microsoft's generic WinUSB driver, and the Python
prototype used to work out the protocol.

## Project layout

| Path | |
|---|---|
| `driver/` | **DrongIdd**, the display driver (IddCx), its INF, `build.bat` and `install.ps1` |
| `docs/PROTOCOL.md` | The dongle's USB protocol and what we learned on real hardware |
| `docs/VENDOR-SOFTWARE.md` | What the vendor installer and driver do |
| `alternative/` | Using the dongle without the driver: user-mode sender, virtual display setup, Python prototype |
| `scripts/fetch-amf.cmd` | Fetches the AMD AMF SDK headers into `third_party/` on first build (not committed) |

## License

MIT (see [LICENSE](LICENSE)), except `driver/`, which is derived from Microsoft's
[IndirectDisplay sample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay) and is
distributed under the Microsoft Public License ([driver/LICENSE-IddSample.txt](driver/LICENSE-IddSample.txt)).
AMD AMF headers are fetched from [GPUOpen](https://github.com/GPUOpen-LibrariesAndSDKs/AMF) at build time (MIT).
