#!/usr/bin/env python3
"""ppm2png.py FILE.ppm... -- the frame dumps (LONGROAD2_FRAME_DUMP_DIR) as PNGs, stdlib only."""
import sys, zlib, struct
def conv(src, dst, scale=1):
    d = open(src, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split()); px = parts[3]
    rows = b"".join(b"\x00" + px[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)
for a in sys.argv[1:]: conv(a, a[:-4] + ".png")
