# tools

Before the driver existed, the dongle was driven from user mode. These tools still work and are handy for protocol
experiments, but for everyday use install the driver from `driver/` instead — the two cannot be used at the same
time (both need the dongle's interface 0).

## Shared setup: WinUSB on the display interface

The user-mode tools talk to the dongle through Microsoft's generic WinUSB driver:

Device Manager → *Other devices* → **windows mirroring interface** → *Update driver* → *Browse my computer* →
*Let me pick* → *Universal Serial Bus devices* → **WinUsb Device** → *Next* → *Yes*.

(If DrongIdd is installed, run `driver\install.ps1 -Uninstall` first.)

## `sender/` — drongcast.exe

Native user-mode sender (C++): Desktop Duplication of a monitor → GPU cursor overlay → AMD AMF → WinUSB.

```
tools\sender\build.bat
tools\sender\out\drongcast.exe --forever
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

## `virtual-display/` — a monitor for the sender

drongcast.exe sends an existing monitor. To get an *extra* screen, `install-vdd.ps1` (run elevated) installs the
open-source [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (MIT, signed via
SignPath; the script downloads a pinned release and checks its SHA-256) and configures it as one 1920×1080 monitor
named **USB-Monitor** (`vdd_settings.xml`, EDID from `make_edid.py`). After editing those files, apply them with
`install-vdd.ps1 -Apply`.

## `prototype/` — drongcast.py

The Python prototype used to work out the protocol (pyusb + ffmpeg):

```
pip install pyusb libusb-package
python tools\prototype\drongcast.py --source test -v                  # test pattern
python tools\prototype\drongcast.py --source test --duration 12 ^
    --variants "B:x264:60:au,D:amf:60:au"                             # labelled variants in one session
```

`--source desktopN` captures monitor N with ffmpeg's `ddagrab`; `--encoder amf|nvenc|x264|x264hq`;
`--split au|nal`. Needs `ffmpeg` in `PATH`.
