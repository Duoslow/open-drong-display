"""Open-source sender for the "Drong Display" USB screen dongle (USB 1CBE:0003).

Protocol reverse-engineered (statically) from the vendor's IddSampleDriver.dll.
No vendor code, no network access. Needs WinUSB bound to interface 0
("windows mirroring interface", USB\\VID_1CBE&PID_0003&MI_00).

All command packets are 512 bytes on bulk OUT 0x01; the first 24 bytes are six
big-endian u32: cmd, arg1, 0, arg2 (usually payload length), 0, 0.
Replies arrive the same way on bulk IN 0x81.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import threading
import time

import libusb_package
import usb.backend.libusb1
import usb.core
import usb.util

VID, PID = 0x1CBE, 0x0003
EP_OUT, EP_IN = 0x01, 0x81
PKT = 512
CHUNK = 0xA0000  # vendor splits each encoded frame into chunks of at most this size
# creating this file makes a running sender stop cleanly (killing it mid-stream hangs the dongle)
STOP_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stop")

# PC -> device
CMD_INIT = 0x02
CMD_STOP = 0x03
CMD_CURSOR = 0x63
CMD_H264 = 0x67
CMD_REPORT_UNAUTHORIZED = 0x54
CMD_REPORT_AUTHORIZED = 0x55
CMD_NONCE_REPLY = 0x58
CMD_CHECK = 0x74
CMD_GET_INFO = 0x77
# device -> PC
EV_NONCE_REQ = 0x57
EV_CHECK_REPLY = 0x75
EV_INFO = 0x77
EV_CONNECTED = 0xA0
EV_DISCONNECTED = 0xA1


class DongleError(Exception):
    pass


def pad(b):
    return b + b"\0" * (-len(b) % PKT)


def find_output(monitor_id="MTT1337"):
    """Return the ddagrab output_idx of the monitor whose PnP id contains monitor_id
    (MTT1337 = the Virtual Display Driver, shown as "USB-Monitor"), or None."""
    import ctypes
    from ctypes import wintypes

    class DISPLAY_DEVICEW(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("DeviceName", wintypes.WCHAR * 32), ("DeviceString", wintypes.WCHAR * 128),
                    ("StateFlags", wintypes.DWORD), ("DeviceID", wintypes.WCHAR * 128), ("DeviceKey", wintypes.WCHAR * 128)]

    user32 = ctypes.windll.user32
    target = None
    i = 0
    while target is None:
        ad = DISPLAY_DEVICEW(cb=ctypes.sizeof(DISPLAY_DEVICEW))
        if not user32.EnumDisplayDevicesW(None, i, ctypes.byref(ad), 0):
            break
        j = 0
        while True:
            mon = DISPLAY_DEVICEW(cb=ctypes.sizeof(DISPLAY_DEVICEW))
            if not user32.EnumDisplayDevicesW(ad.DeviceName, j, ctypes.byref(mon), 0):
                break
            if monitor_id.lower() in mon.DeviceID.lower():
                target = ad.DeviceName
                break
            j += 1
        i += 1
    if target is None:
        return None

    # ddagrab enumerates the outputs of DXGI adapter 0; match by GDI device name
    class GUID(ctypes.Structure):
        _fields_ = [("a", ctypes.c_uint32), ("b", ctypes.c_uint16), ("c", ctypes.c_uint16), ("d", ctypes.c_ubyte * 8)]

    class DXGI_OUTPUT_DESC(ctypes.Structure):
        _fields_ = [("DeviceName", wintypes.WCHAR * 32), ("Rect", wintypes.RECT), ("Attached", wintypes.BOOL),
                    ("Rotation", ctypes.c_int), ("Monitor", wintypes.HMONITOR)]

    def vcall(obj, idx, restype, *args):
        # args: ctypes scalars by value, or ctypes objects (structs/pointers) passed by address
        args = [a if isinstance(a, ctypes.c_uint) else ctypes.c_void_p(ctypes.addressof(a)) for a in args]
        vtbl = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        fn = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *[type(x) for x in args])(vtbl[idx])
        return fn(obj, *args)

    iid = GUID(0x770AAE78, 0xF26F, 0x4DBA, (ctypes.c_ubyte * 8)(0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87))  # IDXGIFactory1
    factory = ctypes.c_void_p()
    if ctypes.windll.dxgi.CreateDXGIFactory1(ctypes.byref(iid), ctypes.byref(factory)):
        return None
    adapter = ctypes.c_void_p()
    if vcall(factory, 7, ctypes.c_long, ctypes.c_uint(0), adapter):  # EnumAdapters
        return None
    k = 0
    while True:
        out = ctypes.c_void_p()
        if vcall(adapter, 7, ctypes.c_long, ctypes.c_uint(k), out):  # EnumOutputs
            return None
        desc = DXGI_OUTPUT_DESC()
        vcall(out, 7, ctypes.c_long, desc)  # GetDesc
        vcall(out, 2, ctypes.c_ulong)  # Release
        if desc.DeviceName == target:
            return k
        k += 1


class Dongle:
    def __init__(self, verbose=False):
        be = usb.backend.libusb1.get_backend(find_library=libusb_package.find_library)
        self.dev = usb.core.find(idVendor=VID, idProduct=PID, backend=be)
        if self.dev is None:
            raise DongleError("dongle not found (1CBE:0003)")
        try:
            usb.util.claim_interface(self.dev, 0)
        except usb.core.USBError as e:
            raise DongleError(f"cannot open interface 0 ({e}). Bind WinUSB to 'windows mirroring interface' first (see tools/README.md).")
        self.verbose = verbose
        self.lock = threading.Lock()
        self.info = {}
        self.connected = threading.Event()
        self.ready = threading.Event()
        self.stop = threading.Event()

    def log(self, *a):
        if self.verbose:
            print("[usb]", *a, flush=True)

    def write(self, data):
        with self.lock:
            self.dev.write(EP_OUT, pad(data), timeout=5000)

    def cmd(self, cmd, a1=0, a2=0, payload=None):
        self.log(f"-> cmd {cmd:#04x} ({a1}, {a2})")
        with self.lock:
            self.dev.write(EP_OUT, pad(struct.pack(">6I", cmd, a1, 0, a2, 0, 0)), timeout=5000)
            if payload:
                self.dev.write(EP_OUT, pad(payload), timeout=5000)

    def read_hdr(self, timeout=100):
        try:
            b = bytes(self.dev.read(EP_IN, PKT, timeout=timeout))
        except usb.core.USBTimeoutError:
            return None
        self.log(f"   raw[{len(b)}] {b[:64].hex()}")
        if len(b) < 24:
            return None
        h = struct.unpack(">6I", b[:24])
        return h if h[0] else None

    def read_data(self, n, timeout=2000):
        out = b""
        while len(out) < n:
            try:
                b = bytes(self.dev.read(EP_IN, PKT * (-(-(n - len(out)) // PKT)), timeout=timeout))
            except usb.core.USBTimeoutError:
                self.log(f"   data timeout after {len(out)}/{n} bytes")
                break
            self.log(f"   data[{len(b)}] {b[:64].hex()}")
            out += b
        return out[:n]

    def handshake(self):
        # "SendMsg9toDefaultEndpoint": class/interface IN request 9, 1 byte
        try:
            r = self.dev.ctrl_transfer(0xA1, 9, 0, 0, 1, timeout=5000)
            self.log("msg9 ->", bytes(r).hex())
        except usb.core.USBError as e:
            self.log("msg9 failed:", e)
        time.sleep(0.005)
        # "CheckCheck": vendor uses this to verify the device; we only need to complete it
        self.cmd(CMD_CHECK, 1, 16, os.urandom(16))
        t = time.time() + 3
        while time.time() < t:
            h = self.read_hdr()
            if h and h[0] == EV_CHECK_REPLY:
                self.log("check reply", self.read_data(h[3]).hex())
                break
        else:
            self.log("no check reply (continuing)")
        self.cmd(CMD_INIT, 1, 0)

    def event_loop(self):
        while not self.stop.is_set():
            try:
                h = self.read_hdr()
            except usb.core.USBError as e:
                print("usb read error:", e)
                self.stop.set()
                break
            if not h:
                continue
            c = h[0]
            self.log(f"<- ev {c:#04x} {h[1:]}")
            if c == EV_CONNECTED:
                self.connected.set()
                self.cmd(CMD_GET_INFO, 1, 0)
            elif c == EV_INFO:
                raw = self.read_data(h[3]) if h[3] else b""
                info = parse_info(raw)
                if info != self.info:  # the device repeats this as a keep-alive
                    print("device info:", info or raw[:200], flush=True)
                self.info = info
                self.ready.set()
            elif c == EV_NONCE_REQ:
                self.cmd(CMD_NONCE_REPLY, 1, 16, os.urandom(16))
            elif c == EV_DISCONNECTED:
                print("monitor disconnected")
                self.ready.clear()
                self.connected.clear()
            elif h[3] and h[3] < 1 << 20:
                self.log("unknown event payload", self.read_data(h[3])[:64].hex())

    def send_frame(self, au, w, h):
        total = len(au)
        n = max(1, -(-total // CHUNK))
        for i in range(n):
            chunk = au[i * CHUNK:(i + 1) * CHUNK]
            info = struct.pack(">6I", CMD_H264, 2, 0, len(chunk), 0, 0) + \
                struct.pack(">10I", total, len(chunk), n, i + 1, 1, w, h, 0, 0, 0)
            with self.lock:
                self.dev.write(EP_OUT, pad(info), timeout=5000)
                self.dev.write(EP_OUT, pad(chunk), timeout=5000)


def parse_info(raw):
    txt = raw.split(b"\0")[0].decode("utf-8", "replace")
    try:
        return json.loads(txt)
    except ValueError:
        pass
    out = {}
    for part in txt.replace("&", "\n").replace(";", "\n").splitlines():
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
        elif ":" in part:
            k, v = part.split(":", 1)
            out[k.strip(' "{}')] = v.strip(' ",{}')
    return out


def ffmpeg_cmd(src, w, h, fps, enc, bitrate, label=None):
    if src == "test":
        inp = ["-re", "-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate={fps}"]
        vf = []
        if label:
            vf = ["-vf", r"drawtext=fontfile='C\:/Windows/Fonts/arialbd.ttf':text='" + label +
                  "':fontsize=360:fontcolor=white:box=1:boxcolor=black:boxborderw=40:x=(w-tw)/2:y=(h-th)/2"]
    else:
        idx = src.removeprefix("desktop") or "0"
        inp = ["-f", "lavfi", "-i", f"ddagrab=output_idx={idx}:framerate={fps}:draw_mouse=1"]
        vf = ["-vf", f"hwdownload,format=bgra,scale={w}:{h}:flags=bicubic,format=yuv420p"]
    if enc == "amf":
        ev = ["-c:v", "h264_amf", "-usage", "ultralowlatency", "-profile:v", "constrained_baseline", "-quality", "speed", "-max_b_frames", "0"]
    elif enc == "nvenc":
        ev = ["-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ull", "-profile:v", "baseline"]
    elif enc == "x264hq":
        ev = ["-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-profile:v", "high", "-coder", "cavlc"]
    else:
        ev = ["-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-profile:v", "baseline", "-coder", "cavlc"]
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", *inp, *vf, *ev,
            # -refs 1 makes the SPS advertise max_dec_frame_buffering=1; AMF defaults to 4, which lets
            # the dongle's decoder hold ~4 frames before showing one
            "-pix_fmt", "yuv420p", "-bf", "0", "-refs", "1", "-g", str(fps * 2), "-b:v", bitrate,
            "-bsf:v", "h264_metadata=aud=insert", "-flush_packets", "1", "-f", "h264", "-"]


def nal_units(stream):
    """Yield each NAL unit with its start code (how the vendor's advert player sends a stream)."""
    for au in access_units(stream):
        starts = []
        i = 0
        while (i := au.find(b"\0\0\1", i)) >= 0:
            starts.append(i - 1 if i and au[i - 1] == 0 else i)
            i += 3
        for k, st in enumerate(starts):
            yield au[st:starts[k + 1] if k + 1 < len(starts) else len(au)]


def pipe_empty(fd):
    """True when nothing more is waiting in the pipe (Windows)."""
    import ctypes
    import msvcrt
    avail = ctypes.c_ulong(0)
    ok = ctypes.windll.kernel32.PeekNamedPipe(msvcrt.get_osfhandle(fd), None, 0, None, ctypes.byref(avail), None)
    return bool(ok) and avail.value == 0


def access_units(stream):
    """Split an Annex-B stream into access units on AUD NALs (dropping the AUDs).

    ffmpeg runs with -flush_packets 1, so each encoded frame arrives in one write: when the
    pipe runs dry the buffered data is a complete frame and is sent right away instead of
    waiting for the next frame's AUD (saves a frame of latency)."""
    try:
        fd = stream.fileno()
    except (AttributeError, OSError, ValueError):
        fd = None
    buf = b""
    cur = b""
    while True:
        # os.read returns whatever is available; a buffered read(n) would wait for n bytes
        data = os.read(fd, 1 << 20) if fd is not None else stream.read(65536)
        if not data:
            break
        buf += data
        if fd is not None and pipe_empty(fd):
            au = cur + buf
            sc = 4 if au.startswith(b"\0\0\0\1") else 3
            if len(au) > sc and au[sc] & 0x1F == 9:  # drop the leading AUD
                k = au.find(b"\0\0\1", sc)
                au = au[k - 1 if au[k - 1] == 0 else k:] if k > 0 else b""
            if au:
                yield au
            cur = buf = b""
            continue
        while True:
            i = buf.find(b"\0\0\1", 3)
            if i < 0:
                break
            j = i - 1 if buf[i - 1] == 0 else i
            nal, buf = buf[:j], buf[j:]
            if not nal:
                continue
            sc = 4 if nal.startswith(b"\0\0\0\1") else 3
            if len(nal) > sc and nal[sc] & 0x1F == 9:  # AUD = frame boundary
                if cur:
                    yield cur
                cur = b""
            else:
                cur += nal
    if cur or buf:
        yield cur + buf


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", default="usb-monitor",
                    help="usb-monitor (the virtual screen) | test | desktop | desktopN (capture output index)")
    ap.add_argument("--forever", action="store_true", help="reconnect automatically (unplug, hiccups, display changes)")
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--encoder", default="amf", choices=["amf", "nvenc", "x264", "x264hq"])
    ap.add_argument("--bitrate", default="8M")
    ap.add_argument("--authorize", action="store_true", help="also send CMD_PC_REPORT_AUTHORIZED (0x55)")
    ap.add_argument("--split", default="au", choices=["au", "nal"], help="one packet per frame or per NAL unit")
    ap.add_argument("--no-cursor", dest="cursor", action="store_false", help="skip the empty cursor packet (0x63) sent before streaming")
    ap.add_argument("--label", help="draw this text on the test pattern")
    ap.add_argument("--duration", type=float, help="stop after N seconds (per segment with --variants)")
    ap.add_argument("--variants", help="comma list of label:encoder:fps:split[:auth] segments in one session")
    ap.add_argument("--no-wait", action="store_true", help="stream without waiting for the device info event")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if not a.forever:
        try:
            sys.exit(session(a))
        except DongleError as e:
            sys.exit(str(e))
    # keep the screen alive: reconnect after unplug, dongle hiccups or display changes
    if os.path.exists(STOP_FILE):
        os.remove(STOP_FILE)
    while True:
        try:
            session(a)
            if os.path.exists(STOP_FILE):
                os.remove(STOP_FILE)
                print("stopped (stop file)", flush=True)
                break
        except DongleError as e:
            print(e, flush=True)
        except KeyboardInterrupt:
            break
        except Exception as e:
            print("session error:", repr(e), flush=True)
        try:
            time.sleep(3)
        except KeyboardInterrupt:
            break


def session(a):
    w, h = map(int, a.size.lower().split("x"))
    source = a.source
    if source == "usb-monitor":
        idx = find_output()
        if idx is None:
            raise DongleError("USB-Monitor (Virtual Display Driver) not found / not active")
        source = f"desktop{idx}"

    d = Dongle(a.verbose)
    try:
        d.handshake()
    except usb.core.USBError as e:
        usb.util.dispose_resources(d.dev)
        raise DongleError(f"dongle not answering ({e}); retrying")
    threading.Thread(target=d.event_loop, daemon=True).start()
    if a.authorize:
        d.cmd(CMD_REPORT_AUTHORIZED, 2, 0)
    if not a.no_wait:
        print(f"connected; waiting for device info ({time.strftime('%T')})", flush=True)
        if not d.ready.wait(15):
            d.stop.set()
            usb.util.release_interface(d.dev, 0)
            usb.util.dispose_resources(d.dev)
            raise DongleError("no device info within 15 s - dongle is not responding")

    if a.cursor:
        d.cmd(CMD_CURSOR, 2, 28, bytes(28))
    # --variants "B:x264:60:au,C:x264:60:nal:auth" runs several labelled segments in one USB session
    if a.variants:
        segs = []
        for v in a.variants.split(","):
            label, enc, fps, split, *flags = v.split(":")
            segs.append((label, enc, int(fps), split, "auth" in flags))
    else:
        segs = [(a.label, a.encoder, a.fps, a.split, False)]
    ff = None
    try:
        for label, enc, fps, split, auth in segs:
            if auth:
                d.cmd(CMD_REPORT_AUTHORIZED, 2, 0)
            print(f"=== segment {label}: {enc} {fps}fps split={split}{' +auth' if auth else ''} ({time.strftime('%T')})", flush=True)
            ff = subprocess.Popen(ffmpeg_cmd(source, w, h, fps, enc, a.bitrate, label), stdout=subprocess.PIPE, bufsize=0)
            n, nbytes, t0 = 0, 0, time.time()
            units = nal_units if split == "nal" else access_units
            for au in units(ff.stdout):
                if d.stop.is_set() or (a.duration and time.time() - t0 > a.duration) or os.path.exists(STOP_FILE):
                    break
                d.send_frame(au, w, h)
                n += 1
                nbytes += len(au)
                if n % (fps * 5) == 0:
                    print(f"{n} frames, {n / (time.time() - t0):.1f} fps", flush=True)
            dt = time.time() - t0
            cpu = ""
            try:
                import psutil
                ct = psutil.Process(ff.pid).cpu_times()
                cpu = f", ffmpeg CPU {(ct.user + ct.system) / dt / psutil.cpu_count() * 100:.1f}% of all cores"
            except Exception:
                pass
            print(f"--- segment {label} result: {n / dt:.1f} fps, {nbytes * 8 / dt / 1e6:.1f} Mbit/s{cpu}", flush=True)
            ff.kill()
            if d.stop.is_set():
                break
    except KeyboardInterrupt:
        pass
    except usb.core.USBError as e:
        print("usb error while streaming:", e, flush=True)
    finally:
        if ff:
            ff.kill()
        d.stop.set()
        try:
            d.cmd(CMD_STOP, 1, 0)
        except usb.core.USBError:
            pass
        try:
            usb.util.release_interface(d.dev, 0)
        except usb.core.USBError:
            pass
        usb.util.dispose_resources(d.dev)
    return 0


if __name__ == "__main__":
    main()
