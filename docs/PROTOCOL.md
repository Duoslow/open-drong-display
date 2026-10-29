# Drong Display USB protocol

Reverse-engineered by static analysis of the vendor's `IddSampleDriver.dll` (no vendor code was run) and verified
against the real device. Device: USB `1CBE:0003`, `bcdDevice` 4.07, product `DH340C`, firmware
`H15DH340CDR015CP1-V001`. Internally the dongle runs Linux (its storage shows up as *Linux File-Stor Gadget*).

## USB layout

Configuration 1 (the default):

| Interface | Class | Endpoints | Use |
|---|---|---|---|
| 0 | vendor (0xFF) | bulk IN `0x81`, bulk OUT `0x01` (512 B) | display — everything below |
| 1, 2 | audio | isochronous OUT `0x02` | HDMI audio |
| 3 | mass storage | bulk `0x82` / `0x03` | read-only drive with the vendor installer |

There is also a configuration 2 with a CDC-NCM network interface (not used). The device sometimes appears as
`1CBE:0005` too (alternate mode, not used).

## Packets

Every command is a **512-byte bulk transfer** on `0x01`. The first 24 bytes are six **big-endian** `u32`, the rest is
zero:

```
cmd, arg1, 0, arg2, 0, 0          arg2 is usually the length of a following payload
```

A payload (if any) follows as a separate transfer, **zero-padded to a multiple of 512 bytes**.

Device → PC events arrive the same way on `0x81` (reads return 64-byte packets; payloads follow in further reads).
The vendor driver polls `0x81` with a 5 ms timeout.

## Session start

| Step | Direction | Packet | Notes |
|---|---|---|---|
| 1 | control IN | `bmRequestType=0xA1, bRequest=9, wValue=0, wIndex=0, wLength=1` | device answers 1 byte (`0xBE`) |
| 2 | → | `0x74 (1, 16)` + 16 random bytes | challenge |
| 3 | ← | `0x75 (…, 16)` + 16 bytes | answer — the vendor PC software uses it to verify the device; can be ignored |
| 4 | → | `0x02 (1, 0)` | display start (the vendor also sends it whenever Windows hands its monitor a new swap-chain) |
| 5 | ← | `0xA0` | monitor connected → reply `0x77 (1, 0)` |
| 6 | ← | `0x71 (…, len)` + text | URL the device wants fetched (`http://8.134.59.18/drongscreen/…/product_sabbat_dual.abs`) — ignore |
| 7 | ← | `0x77 (…, len)` + JSON | device info, see below |

After a previous session the device does not send `0xA0` again; send `0x77 (1, 0)` yourself and it answers with the
info.

Device info example:

```json
{"prodtype":"DH340C","version":"H15DH340CDR015CP1-V001","uuid":"…","webversion":"H15DH340CDR015CP1","no": 13,
 "advert": 0,"manufacturer":"ACI","monitor":"MX279","resolution":"1080P60","memory":"DDR2-128M-800MHz",
 "flash":"XMC-128MBit","wifi":"none","bt":"none"}
```

`manufacturer`/`monitor` describe the HDMI monitor connected to the dongle; `no` counts power-ups. The device
repeats this packet periodically as a keep-alive.

## During a session

| Direction | Packet | Meaning |
|---|---|---|
| ← | `0x57` | nonce request → reply `0x58 (1, 16)` + 16 random bytes |
| → | `0x63 (2, 28)` + 28 bytes | hardware cursor; 28 zero bytes hide it. Send once before the first frame. |
| → | `0x67 (2, chunkLen)` + frame chunk | video, see below |
| → | `0x55 (2, 0)` / `0x54 (2, 0)` | "PC authorized / unauthorized". The vendor driver asks its server and sends `0x54` if the answer is no; never needed. |
| ← | `0xA1` | HDMI monitor disconnected |
| → | `0x6A`, `0x6B`, `0x6C` | firmware update — not implemented on purpose |
| → | `0x03 (1, 0)` | display stop — the vendor sends it when Windows takes the monitor's swap-chain away. The device then shows its built-in promo picture ("install the driver"); HDMI stays on. `0x02` resumes in the same session. **Don't** start a new session after it (control request + handshake): the device then ignores everything until it is replugged. |

## Video

- H.264, Annex-B, **constrained baseline**, CAVLC, no B-frames, 1920×1080 (the vendor uses 1920×1080 for its own
  stream; NV12/BT.709). The vendor driver's NVENC/QSV paths use the High profile, so the decoder accepts more.
- One access unit (one frame) per `0x67` message. Frames larger than `0xA0000` bytes are split into chunks.
- Each chunk is sent as a 512-byte header packet followed by the chunk itself (zero-padded):

```
bytes  0..23  0x67, 2, 0, chunkLen, 0, 0                      (big-endian u32)
bytes 24..63  totalLen, chunkLen, nChunks, chunkIndex (1-based), 1, width, height, 0, 0, 0
```

### Behaviour learned on hardware

- **Rate:** the device queues frames and drains them at roughly **50 frames/s**, even though its HDMI output runs at
  60 Hz. At ≥ 53 fps the queue — and the delay — grows to seconds; 49–50 fps stays live.
- **Continuity:** the device shows **black** if it only gets frames when the screen changes (~10 fps on a still
  desktop). Repeat the last frame to keep a steady stream.
- **Decoder buffering:** use 1 reference frame so the SPS says `max_dec_frame_buffering = 1`. AMD AMF defaults to 4,
  which lets the device hold several frames before showing one.
- Start streaming only after the device info arrived, begin with an IDR frame (SPS/PPS included) and send an IDR at
  least every 2 s so a lost frame cannot leave the screen broken for long.
- A stream that the device could not decode (e.g. sent before it was ready) shows as black with glitched lines and
  can leave it unresponsive until replugged.
- **No filler data.** Filler NAL units (type 12, which CBR encoders such as Media Foundation's add as padding)
  freeze the picture and can crash the device. Use VBR/low-delay rate control or strip them before sending.
- **Never cut a frame off.** If a transfer is cancelled in the middle of a frame (e.g. the sender stops while a frame
  is being written), the device's firmware may crash and drop off USB until it is power-cycled. Finish the frame in
  flight before stopping.
- Multiple slices per frame and SEI NAL units are fine; access unit delimiters (type 9) are fine.
- **Never pause the stream.** If no frames arrive for a few seconds, the device keeps showing black (or the last
  picture) and ignores frames that come later, until it is power-cycled. When there is nothing to show, keep sending
  black frames (the vendor driver plays an advert video instead).
- **Keep the event read pending.** Cancelling and re-issuing the bulk-IN read very often (every 20 ms) loses device
  events, and the device then blanks its output; a 100 ms read timeout works.
- **Frame sizes:** 1920×1080, 1600×900 and 1280×720 streams all display (the device scales to its HDMI output);
  the width/height in the `0x67` header follow the stream. Larger sizes were not tested.
