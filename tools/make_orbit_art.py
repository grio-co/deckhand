#!/usr/bin/env python3
"""Generates the art of the Orbit skin (resources/skins/orbit.dhskin/img/*.png).

Orbit is a free-form skin: a main window made of two large discs (the decks) joined by a waist that holds the mixer,
with see-through holes in the waist, a separate capsule-shaped library window and a round sampler pod. The PNGs are
the windows' bodies; their alpha channel is also the window mask (clicks on transparent pixels fall through).

Everything is computed from signed distance functions (discs, rounded rectangles, rings, ticks, gradients); no outside
artwork is used. Pure standard library. Usage: python3 tools/make_orbit_art.py [output_dir]

The geometry below must match resources/skins/orbit.dhskin (window sizes, platter positions, hole positions). Art is
drawn at 2x the skin's logical size so it stays crisp on Retina displays.

Files (RGBA, straight alpha, edges anti-aliased analytically):
  main.png     2000 x 1120   body of the main window (skin size 1000 x 560)
  browser.png  1520 x  600   capsule library window (760 x 300)
  pod.png       600 x  600   sampler pod (300 x 300)
"""
import math
import os
import struct
import sys
import zlib

S = 2.0  # art pixels per skin point

CYAN = (53, 214, 255)
MAGENTA = (255, 79, 216)
AMBER = (255, 179, 71)
INK = (5, 7, 13)


def clamp(x, a=0.0, b=1.0):
    return a if x < a else b if x > b else x


def mix(a, b, t):
    t = clamp(t)
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def over(dst, src, a):
    """src (rgb) with coverage a over dst (rgb)."""
    return mix(dst, src, a)


def cov(d):
    """Coverage of a shape from its signed distance in art pixels (negative inside)."""
    return clamp(0.5 - d)


def circle(x, y, cx, cy, r):
    return math.hypot(x - cx, y - cy) - r


def ellipse(x, y, cx, cy, rx, ry):
    # first-order distance estimate, good enough for anti-aliasing
    nx, ny = (x - cx) / rx, (y - cy) / ry
    k = math.hypot(nx, ny)
    if k < 1e-6:
        return -min(rx, ry)
    g = math.hypot(nx / rx, ny / ry) / k
    return (k - 1.0) / g if g > 0 else 0.0


def rrect(x, y, cx, cy, hw, hh, r):
    qx, qy = abs(x - cx) - hw + r, abs(y - cy) - hh + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def png(path, w, h, rows):
    raw = bytearray()
    for row in rows:
        raw.append(0)
        raw += row

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def render(path, w, h, shade):
    rows = []
    for py in range(h):
        row = bytearray(w * 4)
        y = py + 0.5
        for px in range(w):
            r, g, b, a = shade(px + 0.5, y)
            o = px * 4
            row[o] = int(clamp(r / 255.0) * 255 + 0.5)
            row[o + 1] = int(clamp(g / 255.0) * 255 + 0.5)
            row[o + 2] = int(clamp(b / 255.0) * 255 + 0.5)
            row[o + 3] = int(clamp(a) * 255 + 0.5)
        rows.append(row)
    png(path, w, h, rows)


# ---- main window ---------------------------------------------------------------------------------------------------
DECK_A = (255.0, 280.0)   # skin points
DECK_B = (745.0, 280.0)
DECK_R = 240.0
PLATTER = (255.0, 265.0)  # jog widgets sit here (radius 125) - see orbit.dhskin
PLATTER_B = (745.0, 265.0)
BRIDGE = (500.0, 280.0, 135.0, 150.0, 70.0)  # cx cy half-w half-h radius
HOLES = [(500.0, 165.0, 62.0, 17.0), (500.0, 395.0, 62.0, 17.0)]
GRIP = (880.0, 455.0, 24.0)


def body_distance(x, y):
    """Signed distance (art px) of the main window shape: two discs and a bridge, minus two portholes."""
    px, py = x / S, y / S
    d = min(circle(px, py, *DECK_A, DECK_R), circle(px, py, *DECK_B, DECK_R), rrect(px, py, BRIDGE[0], BRIDGE[1], BRIDGE[2], BRIDGE[3], BRIDGE[4]))
    for hx, hy, rx, ry in HOLES:
        d = max(d, -ellipse(px, py, hx, hy, rx, ry))
    return d * S


def main_shade(x, y):
    d = body_distance(x, y)
    a = cov(d)
    if a <= 0:
        return (0, 0, 0, 0)
    px, py = x / S, y / S
    # which deck is nearest decides the accent colour; the bridge blends the two
    da = math.hypot(px - DECK_A[0], py - DECK_A[1])
    db = math.hypot(px - DECK_B[0], py - DECK_B[1])
    side = clamp((da - db) / 120.0 * 0.5 + 0.5)
    accent = mix(CYAN, MAGENTA, side)
    # base: deep blue-black with a soft vertical sheen
    base = mix((10, 14, 26), (22, 30, 52), 0.5 - 0.5 * math.cos(py / 560.0 * math.pi * 2) * 0.6)
    col = base
    # concentric grooves on the discs
    for c, (cx, cy) in ((CYAN, DECK_A), (MAGENTA, DECK_B)):
        r = math.hypot(px - cx, py - cy)
        if r < DECK_R + 1:
            for gr, w, k in ((205.0, 0.8, 0.30), (170.0, 0.6, 0.18), (150.0, 1.2, 0.0)):
                if k > 0:
                    col = over(col, c, clamp(1.0 - abs(r - gr) / w) * k)
            # platter well: slightly darker disc behind the jog widget with a thin accent ring at its edge
            pcx, pcy = (PLATTER if cx < 500 else PLATTER_B)
            pr = math.hypot(px - pcx, py - pcy)
            if pr < 133:
                col = mix(col, (6, 8, 16), clamp((133 - pr) / 3.0) * 0.85)
            col = over(col, c, clamp(1.0 - abs(pr - 133.0) / 1.0) * 0.55)
            # tick marks around the rim
            ang = math.atan2(py - cy, px - cx)
            if 214 < r < 224:
                tick = abs(((ang / (2 * math.pi)) * 120.0) % 1.0 - 0.5)
                col = over(col, c, clamp((0.08 - tick) * 12) * 0.35)
    # bridge detail: a brushed inner plate behind the mixer
    plate = rrect(px, py, 500, 280, 118, 96, 26)
    col = over(col, (7, 10, 20), cov(plate * S) * 0.9)
    col = over(col, mix(CYAN, MAGENTA, 0.5), clamp(1.0 - abs(plate * S) / 1.4) * 0.35)
    # ring around each porthole
    for hx, hy, rx, ry in HOLES:
        e = ellipse(px, py, hx, hy, rx, ry) * S
        col = over(col, accent, clamp(1.0 - abs(e - 5.0) / 2.0) * 0.55)
    # resize grip on deck B's rim
    g = circle(px, py, GRIP[0], GRIP[1], GRIP[2]) * S
    col = over(col, (26, 34, 58), cov(g))
    col = over(col, MAGENTA, clamp(1.0 - abs(g + 3.0 * S) / 1.2) * 0.7)
    for k in (-1, 0, 1):
        gl = abs((px - GRIP[0]) * 0.7071 + (py - GRIP[1]) * -0.7071 - k * 6.0) * S
        if math.hypot(px - GRIP[0], py - GRIP[1]) < 15:
            col = over(col, (210, 220, 255), clamp(1.0 - gl / 1.3) * 0.5)
    # outer rim light, 2.5 points wide, brighter at the top-left
    rim = clamp((-d) / (2.5 * S))
    light = 0.55 + 0.45 * clamp(0.5 - ((px - 500) * 0.35 + (py - 280)) / 700.0)
    col = over(col, accent, (1.0 - rim) * light * 0.95)
    # inner shadow just inside the rim
    col = mix(col, (0, 0, 0), clamp(1.0 - abs(-d - 3.0 * S) / (3.0 * S)) * 0.25)
    return (col[0], col[1], col[2], a)


# ---- capsule library window -----------------------------------------------------------------------------------------
BW, BH = 760.0, 300.0


def browser_shade(x, y):
    px, py = x / S, y / S
    d = rrect(px, py, BW / 2, BH / 2, BW / 2, BH / 2, BH / 2) * S
    a = cov(d)
    if a <= 0:
        return (0, 0, 0, 0)
    side = clamp(px / BW)
    accent = mix(CYAN, MAGENTA, side)
    col = mix((9, 13, 24), (18, 25, 44), clamp(py / BH))
    inner = rrect(px, py, BW / 2, BH / 2, BW / 2 - 14, BH / 2 - 14, BH / 2 - 14) * S
    col = over(col, (6, 9, 18), cov(inner) * 0.9)
    col = over(col, accent, clamp(1.0 - abs(inner) / 1.2) * 0.35)
    rim = clamp((-d) / (2.5 * S))
    light = 0.6 + 0.4 * clamp(1.0 - py / BH)
    col = over(col, accent, (1.0 - rim) * light * 0.95)
    return (col[0], col[1], col[2], a)


# ---- sampler pod ---------------------------------------------------------------------------------------------------------
PS = 300.0


def pod_shade(x, y):
    px, py = x / S, y / S
    d = circle(px, py, PS / 2, PS / 2, PS / 2) * S
    a = cov(d)
    if a <= 0:
        return (0, 0, 0, 0)
    r = math.hypot(px - PS / 2, py - PS / 2)
    col = mix((14, 18, 32), (8, 11, 20), clamp(r / 150.0))
    col = over(col, AMBER, clamp(1.0 - abs(r - 118.0) / 0.9) * 0.45)
    ang = math.atan2(py - PS / 2, px - PS / 2)
    if 124 < r < 134:
        tick = abs(((ang / (2 * math.pi)) * 48.0) % 1.0 - 0.5)
        col = over(col, AMBER, clamp((0.1 - tick) * 12) * 0.4)
    rim = clamp((-d) / (2.5 * S))
    light = 0.6 + 0.4 * clamp(0.5 - ((px - 150) * 0.4 + (py - 150)) / 300.0)
    col = over(col, AMBER, (1.0 - rim) * light * 0.95)
    return (col[0], col[1], col[2], a)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "skins", "orbit.dhskin", "img")
    os.makedirs(out, exist_ok=True)
    render(os.path.join(out, "main.png"), int(1000 * S), int(560 * S), main_shade)
    render(os.path.join(out, "browser.png"), int(BW * S), int(BH * S), browser_shade)
    render(os.path.join(out, "pod.png"), int(PS * S), int(PS * S), pod_shade)


if __name__ == "__main__":
    main()
