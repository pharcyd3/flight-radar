#!/usr/bin/env python3
"""Capture a screenshot from the device over serial and save it as a round PNG.

Sends any pose commands given, then `SHOT`, reads the RGB565 framebuffer streamed
between SHOT_BEGIN/SHOT_END, and writes an RGBA PNG with the corners outside the
round display masked transparent.

Usage: screenshot.py OUT.png [POSE_CMD ...]
  e.g. screenshot.py docs/images/lofi.png "MAP:1"
       screenshot.py docs/images/follow.png "SEL:0" "FOLLOW"
"""
import os
import struct
import sys
import time
import zlib

PORT = os.environ.get("DIAL_PORT", "/dev/cu.usbmodem101")
BAUD = 115200


def rgb565_to_rgba(buf, w, h):
    out = bytearray(w * h * 4)
    cx, cy, rr = (w - 1) / 2.0, (h - 1) / 2.0, (w / 2.0) ** 2
    for i in range(w * h):
        v = (buf[2 * i] << 8) | buf[2 * i + 1]   # sprite stores RGB565 big-endian
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        y, x = divmod(i, w)
        inside = (x - cx) ** 2 + (y - cy) ** 2 <= rr
        o = 4 * i
        out[o]     = (r * 255 + 15) // 31
        out[o + 1] = (g * 255 + 31) // 63
        out[o + 2] = (b * 255 + 15) // 31
        out[o + 3] = 255 if inside else 0
    return bytes(out)


def write_png(path, rgba, w, h):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter type 0
        raw.extend(rgba[y * w * 4:(y + 1) * w * 4])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    out = sys.argv[1]
    poses = sys.argv[2:]

    import serial  # pyserial
    ser = serial.Serial(PORT, BAUD, timeout=2)
    time.sleep(0.3)

    for cmd in poses:
        ser.reset_input_buffer()
        ser.write((cmd + "\n").encode())
        ser.flush()
        time.sleep(2.5)                     # let a redraw / fetch settle

    ser.reset_input_buffer()
    ser.write(b"SHOT\n")
    ser.flush()

    # Find the SHOT_BEGIN header line amongst normal log output.
    w = h = None
    deadline = time.time() + 15
    while time.time() < deadline:
        ln = ser.readline()
        if ln.startswith(b"SHOT_BEGIN"):
            _, sw, sh = ln.split()
            w, h = int(sw), int(sh)
            break
    if w is None:
        print("ERROR: no SHOT_BEGIN (is the debug firmware flashed?)"); sys.exit(2)

    need = w * h * 2
    buf = bytearray()
    while len(buf) < need and time.time() < deadline + 20:
        buf.extend(ser.read(need - len(buf)))
    if len(buf) < need:
        print(f"ERROR: short frame {len(buf)}/{need}"); sys.exit(3)

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    write_png(out, rgb565_to_rgba(buf, w, h), w, h)
    print(f"saved {out} ({w}x{h})")
    ser.close()


if __name__ == "__main__":
    main()
