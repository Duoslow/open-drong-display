# Alternative usage (without the driver)

The recommended way to use the dongle is the display driver in [`../driver`](../driver). The tools here drive the
dongle from a normal program instead — useful if you can't or don't want to install a driver, or for experimenting
with the protocol.

They talk to the dongle through Microsoft's generic WinUSB driver, so **they can't run while the display driver is
installed** (both need the dongle's interface 0). Remove it first with `driver\install.ps1 -Uninstall`.

| Folder | What | Use it for |
|---|---|---|
| [`user-mode-sender/`](#user-mode-sender) | `drongcast.exe`, a C++ program that streams a monitor to the dongle | everyday use without the driver |
| [`virtual-display/`](#virtual-display) | setup for a virtual monitor that the sender can stream | an *extra* screen instead of a copy of an existing one |
| [`python-prototype/`](#python-prototype) | `drongcast.py`, the prototype used to work out the protocol | protocol experiments, test patterns, comparing encoder settings |

Compared with the driver, the user-mode route needs a program running in the background, a separate virtual
monitor, and has to skip frames itself to stay under the dongle's ~50 frames/s limit.

## Setup: WinUSB on the display interface

Device Manager → *Other devices* → **windows mirroring interface** → *Update driver* → *Browse my computer* →
*Let me pick* → *Universal Serial Bus devices* → **WinUsb Device** → *Next* → *Yes*.

## user-mode-sender

Desktop Duplication of a monitor → GPU cursor overlay → AMD AMF (H.264) → WinUSB. Needs an AMD Radeon GPU.

```
alternative\user-mode-sender\build.bat
alternative\user-mode-sender\out\drongcast.exe --forever
```

| Option | |
|---|---|
| `--forever` | reconnect automatically after unplugging, dongle hiccups or display changes |
| `--max-fps N` | send at most N frames/s (default 50; ≥ 53 makes the dongle's queue grow) |
| `--bitrate MBPS` | H.264 bitrate (default 15) |
| `--monitor ID` | PnP id of the monitor to capture (default `MTT1337` = the virtual display below) |
| `--no-cursor` | don't draw the mouse pointer |
| `--send-stop` | send the stop command on exit (leaves the dongle unresponsive until replugged) |
| `-v` | log device events |

Stop it cleanly by creating an empty file named `stop` next to the exe. It prints frame rate, bitrate and the
encode/USB time every 5 seconds.

## virtual-display

`drongcast.exe` streams an existing monitor. To get an *extra* screen, run `install-vdd.ps1` (elevated): it installs
the open-source [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (MIT, signed via
SignPath — the script downloads a pinned release and checks its SHA-256) and configures it as one 1920×1080 monitor
named **USB-Monitor** (`vdd_settings.xml`, EDID from `make_edid.py`). After editing those files, apply them with
`install-vdd.ps1 -Apply`.

## python-prototype

The Python prototype used to work out the protocol (pyusb + ffmpeg):

```
pip install pyusb libusb-package
python alternative\python-prototype\drongcast.py --source test -v                  # test pattern
python alternative\python-prototype\drongcast.py --source test --duration 12 ^
    --variants "B:x264:60:au,D:amf:60:au"                                         # labelled variants in one session
```

`--source desktopN` captures monitor N with ffmpeg's `ddagrab`; `--encoder amf|nvenc|x264|x264hq`;
`--split au|nal`. Needs `ffmpeg` in `PATH`.
