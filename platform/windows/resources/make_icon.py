#!/usr/bin/env python3
"""Generates deckhand.ico: an original placeholder icon (a record on a dark tile). Pure standard library.
Run: python3 make_icon.py deckhand.ico"""
import math, struct, sys, zlib

def png(size):
    px = bytearray()
    c = (size - 1) / 2.0
    for y in range(size):
        px.append(0)
        for x in range(size):
            dx, dy = x - c, y - c
            r = math.hypot(dx, dy) / (size / 2.0)
            # rounded square tile
            qx, qy = abs(dx) / (size / 2.0), abs(dy) / (size / 2.0)
            k = 0.28
            ex, ey = max(qx - (1 - k), 0), max(qy - (1 - k), 0)
            inside = math.hypot(ex, ey) <= k
            col, a = (22, 24, 32), 255 if inside else 0
            if inside:
                if r < 0.86:   # record
                    col = (40, 42, 54)
                    if 0.60 < r < 0.62 or 0.46 < r < 0.48: col = (58, 60, 74)
                if r < 0.28: col = (255, 122, 26)   # label
                if r < 0.05: col = (22, 24, 32)     # spindle hole
                ang = math.atan2(dy, dx)
                if 0.62 < r < 0.80 and -2.2 < ang < -1.5: col = (255, 122, 26)  # a highlight arc
            px += bytes((*col, a))
    raw = bytes(px)
    def chunk(t, d):
        b = t + d
        return struct.pack(">I", len(d)) + b + struct.pack(">I", zlib.crc32(b) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

sizes = [16, 32, 48, 256]
imgs = [png(s) for s in sizes]
out = struct.pack("<HHH", 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for s, d in zip(sizes, imgs):
    out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(d), off)
    off += len(d)
open(sys.argv[1], "wb").write(out + b"".join(imgs))
