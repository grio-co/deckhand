#!/usr/bin/env python3
"""Generates the voidwave skin's image atlas (resources/skins/voidwave.dhskin/img/atlas.png).

Everything is computed here from simple geometry (rings, ticks, gradients); no outside artwork is used.
Pure standard library. Usage: python3 tools/make_voidwave_atlas.py [output.png]

Atlas layout (256x256, RGBA), matching the `atlas art` block in skin.dhs:
  ring   (0,0,128,128)      thin orbit ring with ticks and a comet arc; rotates in the skin
  marker (128,0,128,128)    platter marker: a bright dot near the rim and a faint spoke; spins while playing
  glow   (0,128,128,128)    soft radial haze
  frame  (128,128,64,64)    glass panel border, nine-slice with 20 px corners
"""
import math
import struct
import sys
import zlib

W = H = 256
VIOLET = (138, 92, 255)
CYAN = (41, 242, 255)
MAGENTA = (255, 61, 240)
WHITE = (255, 255, 255)
SS = 4  # samples per axis


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def smooth(x):
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


class Layer:
    """Straight-alpha 'over' compositing of a few shapes into one sample."""

    def __init__(self):
        self.c = (0.0, 0.0, 0.0)
        self.a = 0.0

    def over(self, col, a):
        if a <= 0:
            return
        na = a + self.a * (1 - a)
        self.c = tuple((col[i] * a + self.c[i] * self.a * (1 - a)) / na for i in range(3))
        self.a = na

    def result(self):
        return (self.c[0], self.c[1], self.c[2], self.a)


def ring_px(x, y):  # 128x128 local coordinates, y down
    dx, dy = x - 64.0, y - 64.0
    r = math.hypot(dx, dy)
    ang = math.degrees(math.atan2(dy, dx)) % 360.0
    L = Layer()
    L.over(VIOLET, 0.80 * smooth(1 - abs(r - 60.0) / 1.2))                                       # outer ring
    L.over(CYAN, 0.35 * smooth(1 - abs(r - 52.0) / 0.9) * (1 if int(ang / 7.5) % 2 == 0 else 0))  # dashed inner ring
    if 55.5 <= r <= 58.5:
        L.over(VIOLET, 0.55 * smooth(1 - abs(((ang + 3.75) % 7.5) - 3.75) / 0.9))                # 48 ticks
    if 62.0 <= r <= 63.6 and (ang % 30.0) < 1.4:                                                 # twelve pips
        L.over(MAGENTA, 0.7)
    if 57.5 <= r <= 62.5:                                                                        # comet arc
        t = ang / 80.0
        if 0 <= t <= 1:
            L.over(mix(MAGENTA, CYAN, t), (1 - t) ** 1.5 * smooth(1 - abs(r - 60.0) / 2.5))
    return L.result()


def marker_px(x, y):
    dx, dy = x - 64.0, y - 64.0
    L = Layer()
    d = math.hypot(dx, dy + 50.0)  # the dot sits at 12 o'clock, 50 px from the centre
    L.over(CYAN, 0.25 * smooth(1 - d / 15.0))
    L.over(CYAN, 0.9 * smooth(1 - d / 7.0))
    L.over(WHITE, 0.95 * smooth(1 - d / 3.2))
    if -44 < dy < -8:                                                                            # faint spoke
        L.over(CYAN, 0.35 * smooth(1 - abs(dx)) * smooth((-dy - 8) / 10))
    L.over(CYAN, 0.35 * smooth(1 - abs(math.hypot(dx, dy) - 3.0)))                               # spindle ring
    return L.result()


def glow_px(x, y):
    d = math.hypot(x - 64.0, y - 64.0) / 64.0
    return (VIOLET[0], VIOLET[1], VIOLET[2], max(0.0, 1 - d) ** 2.2 * 0.55)


def frame_px(x, y):  # 64x64: rounded-rect border with a soft inner glow and brighter corners
    r = 16.0
    qx = abs(x - 32.0) - (32.0 - r - 1.5)
    qy = abs(y - 32.0) - (32.0 - r - 1.5)
    dist = math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r  # signed distance to the border line
    line = smooth(1 - abs(dist) / 1.3) * 0.65
    inner = smooth(1 - abs(dist + 4.0) / 5.0) * 0.18 if dist < 0 else 0.0
    corner = 0.35 * smooth(1 - abs(dist) / 2.4) if (x < 22 or x > 42) and (y < 22 or y > 42) else 0.0
    col = mix(VIOLET, CYAN, min(1.0, corner * 2.5))
    return (col[0], col[1], col[2], min(1.0, max(line, inner) + corner))


REGIONS = [(0, 0, 128, 128, ring_px), (128, 0, 128, 128, marker_px), (0, 128, 128, 128, glow_px), (128, 128, 64, 64, frame_px)]


def render():
    px = [[(0, 0, 0, 0)] * W for _ in range(H)]
    n = SS * SS
    for (rx, ry, rw, rh, fn) in REGIONS:
        for y in range(rh):
            for x in range(rw):
                sr = sg = sb = sa = 0.0
                for sy in range(SS):
                    for sx in range(SS):
                        r, g, b, a = fn(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS)
                        sr += r * a
                        sg += g * a
                        sb += b * a
                        sa += a
                if sa > 0:
                    px[ry + y][rx + x] = (round(sr / sa), round(sg / sa), round(sb / sa), round(255 * sa / n))
    return px


def png_bytes(px):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = bytearray()
    for row in px:
        raw.append(0)  # filter: none
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "resources/skins/voidwave.dhskin/img/atlas.png"
    with open(out, "wb") as f:
        f.write(png_bytes(render()))
    print("wrote", out)
