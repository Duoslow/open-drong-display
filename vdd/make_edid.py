"""Writes user_edid.bin: a 128-byte EDID 1.4 for a 1920x1080@60 monitor called "USB-Monitor".

The Virtual Display Driver presents it to Windows when <CustomEdid> is true
(it takes resolutions from vdd_settings.xml, the EDID only supplies identity).
"""
import sys

NAME = "USB-Monitor"


def pnp_id(s):
    a, b, c = (ord(ch) - 64 for ch in s)
    return ((a << 10) | (b << 5) | c).to_bytes(2, "big")


def descriptor(tag, text):
    body = text.encode("ascii")[:13]
    if len(body) < 13:
        body += b"\n" + b" " * (12 - len(body))
    return bytes([0, 0, 0, tag, 0]) + body


e = bytearray()
e += bytes([0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])
e += pnp_id("USB")                   # manufacturer
e += (0x0001).to_bytes(2, "little")  # product code
e += (1).to_bytes(4, "little")       # serial
e += bytes([1, 2026 - 1990])         # week, year
e += bytes([1, 4])                   # EDID 1.4
e += bytes([0x80, 60, 34, 120, 0x02])  # digital input, 60x34 cm, gamma 2.2, RGB + preferred timing is native
e += bytes.fromhex("EE91A3544C99260F5054")  # sRGB chromaticity
e += bytes(3)                        # no established timings
e += bytes([0xD1, 0xC0]) + bytes([0x01, 0x01]) * 7  # standard timing 1920x1080@60
# detailed timing: CEA 1080p60, 148.5 MHz, 2200x1125 total, +h +v sync
e += bytes.fromhex("023A801871382D40582C4500585421000 01E".replace(" ", ""))
e += descriptor(0xFC, NAME)          # monitor name
e += bytes([0, 0, 0, 0xFD, 0, 56, 76, 30, 83, 15, 0, 0x0A]) + b" " * 6  # range limits
e += bytes([0, 0, 0, 0x10, 0]) + bytes(13)  # dummy
e += bytes([0])                      # no extension blocks
e += bytes([(-sum(e)) & 0xFF])
assert len(e) == 128, len(e)
open(sys.argv[1] if len(sys.argv) > 1 else "user_edid.bin", "wb").write(e)
print("wrote", len(e), "bytes, name", NAME)
