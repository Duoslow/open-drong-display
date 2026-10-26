# drong-display

Open replacement for the vendor software of the "Drong Display" USB screen dongle
(USB `1CBE:0003`, rev 4.07). No vendor code, no phone-home.

## Why

The vendor installer (`E:\Drong Display.exe`, SHA-256 `35C9BA52…7296`, unsigned Advanced Installer) installs:

- `IddSampleDriver.dll` – Microsoft's IddCx sample + FFmpeg 6.1/x264 (GPL, no source offered), signed with a
  WDK *test* certificate; catalog signed by self-signed `DrongCert` (thumbprint `FBC274FB…8E80`) which the MSI
  adds to the trusted stores.
- The driver itself talks to `ds.dronghootin.com.cn`: `authProduct`, `isCompatibility`,
  `saveOrRefreshProductOnline`, `uploadErrorLog`, `getAdvertising`, `upgradeFirmware`, `productPreview/upload`.
  It sends device serial/model/usage time plus PC manufacturer/model/GPU (WMI `Win32_ComputerSystem`).
- `DrongUsbService.exe` (service, named pipe `\\.\pipe\Drongpipe`) → `a.bat` → `CheckPackageRun.exe` elevated,
  which downloads an advert video (`Advert.h264`) and firmware.

## Protocol (from static analysis of IddSampleDriver.dll)

Interface 0 (vendor class), bulk OUT `0x01`, bulk IN `0x81`. Every command is a 512-byte packet whose
first 24 bytes are six big-endian u32: `cmd, arg1, 0, arg2/len, 0, 0`. Data payloads follow as separate
transfers, zero-padded to a multiple of 512.

| dir | cmd | args | meaning |
|---|---|---|---|
| ctrl | `A1 09 0000 0000 0001` | | "SendMsg9" (class IN request) at start |
| → | `0x74` | 1, 16 + 16 bytes | challenge; device answers `0x75` + 16 bytes (PC-side check, ignorable) |
| → | `0x02` | 1, 0 | init / wait for monitor |
| ← | `0xA0` | | monitor connected → PC sends `0x77` (1, 0) |
| ← | `0x71` | len | URL the device wants fetched (`http://8.134.59.18/drongscreen/…/product_sabbat_dual.abs`) — ignored |
| ← | `0x77` | len | device info, JSON (prodtype, version, uuid, monitor, resolution `1080P60`, …) |
| ← | `0x57` | | nonce request → PC sends `0x58` (1, 16) + 16 random bytes |
| → | `0x54` / `0x55` | 2, 0 | report unauthorized / authorized (vendor decides via its server) |
| → | `0x67` | 2, chunkLen | frame chunk header; bytes 24..63 = `total, chunkLen, nChunks, idx(1-based), 1, width, height, 0, 0, 0`; then the H.264 chunk |
| → | `0x63` | 2, 28 + 28 bytes | hardware cursor update |
| ← | `0xA1` | | monitor disconnected |
| → | `0x6a`/`0x6b`/`0x6c` | | firmware-update path — **not implemented on purpose** |

Frames: H.264 Constrained Baseline (CAVLC, no B-frames), Annex-B, one access unit per frame, chunks ≤ 0xA0000 bytes.
The vendor uses 1920×1080 for its advert stream.

### Behaviour learned on hardware

- Send an empty cursor packet (`0x63`, 28 zero bytes) before the first frame.
- The SPS must allow showing frames immediately: 1 reference frame → `max_dec_frame_buffering = 1`
  (AMF defaults to 4, which lets the dongle hold ~4 frames).
- **Never send more than ~50 frames/s.** The dongle queues frames and drains them slower than its 60 Hz output;
  at 53+ fps the queue (and the delay) grows to seconds.
- Do **not** send `0x03` (stop) on exit: afterwards the dongle ignores everything until it is replugged.
  Without it, a new session works right away — the dongle answers the handshake but does not repeat `0xA0`,
  so ask for `0x77` yourself.
- The device repeats `0x77` (info) as a keep-alive.

## Layout

| path | what |
|---|---|
| `native/` | **drongcast.exe** – the sender (C++): Desktop Duplication → GPU cursor overlay → AMF BGRA→NV12 → AMF H.264 (ultra-low-latency) → WinUSB. ~5 ms encode+USB, 50 fps cap. |
| `vdd/` | the open-source [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (signed by SignPath), configured as one 1920×1080@60 monitor named **USB-Monitor** (`make_edid.py`) |
| `drongcast.py` | first prototype (Python + ffmpeg); kept for protocol experiments (`--variants`, `--source test`) |

## Setup

1. Bind Microsoft's WinUSB driver to the display interface (only interface 0):
   Device Manager → *Other devices* → **windows mirroring interface** → Update driver → Browse my computer →
   Let me pick → *Universal Serial Bus devices* → **WinUsb Device** → Next → Yes.
   (Undo: uninstall that device in Device Manager.)
2. Install the virtual monitor (elevated): `powershell -ExecutionPolicy Bypass -File vdd\install-vdd.ps1`
   (after changing `vdd_settings.xml` / the EDID: add `-Apply`).
3. Build and run: `native\build.bat`, then `native\drongcast.exe --forever`.
   Stop cleanly by creating a file named `stop` next to the exe.

`drongcast.exe` options: `--max-fps N` (default 50), `--bitrate MBPS` (default 15), `--no-cursor`, `--monitor ID`, `-v`.

Optional: the dongle's mass-storage (`MI_03`, shows as a drive with the vendor installer) and audio (`MI_01`)
interfaces can be disabled in Device Manager.
