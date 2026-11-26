#!/usr/bin/env python3
"""Fails (exit 1) unless a PNG exists, has the expected size and is not essentially blank.

    check-screenshot.py FILE.png [--size WxH] [--min-colours N] [--min-content FRACTION]

Standard library only. Handles non-interlaced 8-bit grey, RGB, RGBA and palette PNGs (what the shells write).
"Content" is the share of pixels that differ from the most common colour (the background).
"""
import argparse
import collections
import struct
import sys
import zlib


def fail(msg):
    print("screenshot check FAILED: " + msg)
    sys.exit(1)


def decode(path):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        fail("cannot read %s (%s)" % (path, e))
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail("%s is not a PNG" % path)
    pos, idat, plte = 8, [], b""
    w = h = depth = ctype = interlace = None
    while pos + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if w is None or not idat:
        fail("PNG has no header or no image data")
    if depth != 8 or interlace != 0 or ctype not in (0, 2, 3, 6):
        fail("unsupported PNG layout (depth %s, colour type %s, interlace %s)" % (depth, ctype, interlace))
    bpp = {0: 1, 2: 3, 3: 1, 6: 4}[ctype]
    try:
        raw = zlib.decompress(b"".join(idat))
    except zlib.error as e:
        fail("corrupt image data (%s)" % e)
    stride = w * bpp
    if len(raw) < (stride + 1) * h:
        fail("image data is truncated")
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        elif f != 0:
            fail("bad PNG filter type %d" % f)
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--size", help="expected WxH")
    ap.add_argument("--min-colours", type=int, default=24)
    ap.add_argument("--min-content", type=float, default=0.02)
    a = ap.parse_args()
    w, h, bpp, rows = decode(a.png)
    if a.size:
        ew, eh = (int(v) for v in a.size.lower().split("x"))
        if (w, h) != (ew, eh):
            fail("size is %dx%d, expected %dx%d" % (w, h, ew, eh))
    counts = collections.Counter()
    for line in rows:
        counts.update(line[i:i + bpp].hex() for i in range(0, w * bpp, bpp))
    colours = len(counts)
    background, bg_n = counts.most_common(1)[0]
    content = 1.0 - bg_n / float(w * h)
    print("%s: %dx%d, %d distinct colours, %.1f%% of pixels differ from the background colour %s"
          % (a.png, w, h, colours, content * 100, background))
    if colours < a.min_colours:
        fail("only %d distinct colours (need %d): the image is essentially blank" % (colours, a.min_colours))
    if content < a.min_content:
        fail("only %.2f%% non-background pixels (need %.2f%%)" % (content * 100, a.min_content * 100))


main()
