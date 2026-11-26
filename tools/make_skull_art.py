#!/usr/bin/env python3
"""Generates the art of the Skull skin (resources/skins/skull.dhskin/img/*.png).

Skull is a free-form, multi-window skin: the main window is an X-ray skull with huge eye sockets (the decks), a dropped jaw with an
open mouth (the mixer), teeth (pads and buttons) and a cervical spine (effects and app controls). Everything that is not bone is
transparent, and the PNG alpha is also the window mask. Floating modules: a ribcage (library), a femur (sampler) and bone trays
(grid tools, library tools / settings panels).

The art is original and procedural: polygons rasterised with exact horizontal and 4x vertical anti-aliasing, bone shading from a
blurred coverage field. No outside artwork, no text (all labels are skin widgets). Pure standard library.

Usage: python3 tools/make_skull_art.py [output_dir]      env SKULL_ART_SCALE=1 for a quick low-resolution draft (default 2)

The geometry here must match resources/skins/skull.dhskin (window sizes, socket centres, tooth pitch, plate and vertebra rows).

Files (RGBA, straight alpha):
  main.png     2200 x 2880   skull window (skin size 1100 x 1440)
  browser.png  1600 x  940   ribcage library (800 x 470)
  sampler.png  1000 x  380   femur sampler (500 x 190)
  grid.png     1400 x  600   grid / cue tools tray (700 x 300)
  tools.png    1400 x 1040   library tools tray (700 x 520), hosts the application's Tools panel
  settings.png 1440 x 1120   settings tray (720 x 560), hosts the application's Settings panel
"""
import math
import os
import struct
import sys
import zlib
from array import array

S = int(os.environ.get("SKULL_ART_SCALE", "2"))  # art pixels per skin point
SUB = 4  # vertical subsamples per pixel

CYAN = (63, 230, 255)
AMBER = (255, 194, 69)
INK = (8, 21, 34)
BONE_HI = (246, 251, 255)
BONE_MID = (196, 222, 244)
BONE_LO = (128, 166, 208)
RIM = (232, 246, 255)
DEEP = (6, 14, 24)


def clamp(x, a=0.0, b=1.0):
    return a if x < a else b if x > b else x


def mix(a, b, t):
    t = clamp(t)
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def smooth(e0, e1, x):
    t = clamp((x - e0) / (e1 - e0))
    return t * t * (3 - 2 * t)


# ---- geometry helpers (points in skin units; scaled by S at raster time) --------------------------------------------------
def catmull(pts, n=10, closed=False):
    out = []
    m = len(pts)
    rng = range(m) if closed else range(m - 1)
    for i in rng:
        p0 = pts[(i - 1) % m] if (closed or i > 0) else pts[i]
        p1, p2 = pts[i], pts[(i + 1) % m]
        p3 = pts[(i + 2) % m] if (closed or i + 2 < m) else pts[(i + 1) % m]
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * ((2 * p1[j]) + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2 + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) for j in (0, 1)))
    if not closed:
        out.append(pts[-1])
    return out


def mirror_x(pts, w):
    return [(w - x, y) for x, y in reversed(pts)]


def circle_poly(cx, cy, r, n=None):
    n = n or max(24, int(r * S * 0.9))
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def ellipse_poly(cx, cy, rx, ry, n=None):
    n = n or max(24, int(max(rx, ry) * S * 0.9))
    return [(cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n)) for i in range(n)]


def rrect_poly(x, y, w, h, r, n=8):
    r = min(r, w / 2, h / 2)
    pts = []
    for cx, cy, a0 in ((x + w - r, y + r, -90), (x + w - r, y + h - r, 0), (x + r, y + h - r, 90), (x + r, y + r, 180)):
        for k in range(n + 1):
            a = math.radians(a0 + 90 * k / n)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def arc_pts(cx, cy, r, a0, a1, n=None):
    n = n or max(8, int(abs(a1 - a0) / 360 * r * S))
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def bezier(p0, p1, p2, n=24):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0], (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in [i / n for i in range(n + 1)]]


def stroke(pts, width, closed=False, caps=True):
    """Polygons whose nonzero union is the stroked polyline."""
    polys = []
    hw = width / 2
    segs = list(zip(pts, pts[1:] + ([pts[0]] if closed else [])))
    for (x0, y0), (x1, y1) in segs:
        dx, dy = x1 - x0, y1 - y0
        ln = math.hypot(dx, dy)
        if ln < 1e-9:
            continue
        nx, ny = -dy / ln * hw, dx / ln * hw
        q = [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)]
        # orient consistently (positive area)
        area = sum(q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1] for i in range(4))
        polys.append(q if area > 0 else q[::-1])
    if caps:
        joints = pts if closed else pts
        step = max(1, len(joints) // 80) if width > 3 else 1
        for i in range(0, len(joints), step):
            polys.append(circle_poly(joints[i][0], joints[i][1], hw, max(10, int(hw * S * 1.2))))
        if not closed and (len(joints) - 1) % step:
            polys.append(circle_poly(joints[-1][0], joints[-1][1], hw, max(10, int(hw * S * 1.2))))
    return polys


def dashed(pts, on, off):
    out, cur, acc, state = [], [pts[0]], 0.0, True
    for a, b in zip(pts, pts[1:]):
        ln = math.hypot(b[0] - a[0], b[1] - a[1])
        pos = 0.0
        while pos < ln:
            lim = on if state else off
            take = min(lim - acc, ln - pos)
            pos += take
            acc += take
            t = pos / ln if ln else 1
            p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
            if state:
                cur.append(p)
            if acc >= lim - 1e-9:
                if state:
                    out.append(cur)
                state = not state
                acc = 0.0
                cur = [p]
    if state and len(cur) > 1:
        out.append(cur)
    return out


# ---- canvas ---------------------------------------------------------------------------------------------------------------
class Canvas:
    def __init__(self, w, h):
        self.w, self.h = int(w * S), int(h * S)
        self.px = array("f", bytes(4 * 4 * self.w * self.h))  # straight rgb 0..255, alpha 0..1

    def blend_row(self, y, x0, cov, shader):
        px, w = self.px, self.w
        base = (y * w + x0) * 4
        for i, c in enumerate(cov):
            if c <= 0.003:
                continue
            r, g, b, a = shader(x0 + i + 0.5, y + 0.5)
            a *= c
            if a <= 0.001:
                continue
            o = base + i * 4
            da = px[o + 3]
            oa = a + da * (1 - a)
            k = a / oa
            px[o] += (r - px[o]) * k
            px[o + 1] += (g - px[o + 1]) * k
            px[o + 2] += (b - px[o + 2]) * k
            px[o + 3] = oa

    def fill(self, polys, shader, rule="evenodd"):
        for y, x0, cov in coverage(polys, self.w, self.h, rule):
            self.blend_row(y, x0, cov, shader)

    def save(self, path):
        w, h, px = self.w, self.h, self.px
        raw = bytearray()
        for y in range(h):
            raw.append(0)
            row = bytearray(w * 4)
            o = y * w * 4
            for i in range(w * 4):
                v = px[o + i]
                row[i] = int(clamp(v / 255.0 if i % 4 != 3 else v) * 255 + 0.5)
            raw += row

        def chunk(tag, data):
            c = struct.pack(">I", len(data)) + tag + data
            return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
            f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
            f.write(chunk(b"IEND", b""))


def coverage(polys, W, H, rule="evenodd"):
    """Yields (y, x0, [coverage...]) rows for the polygon set (points in skin units, scaled here)."""
    edges = []
    minx, maxx, miny, maxy = 1e9, -1e9, 1e9, -1e9
    for poly in polys:
        n = len(poly)
        for i in range(n):
            (ax, ay), (bx, by) = poly[i], poly[(i + 1) % n]
            ax, ay, bx, by = ax * S, ay * S, bx * S, by * S
            minx, maxx, miny, maxy = min(minx, ax, bx), max(maxx, ax, bx), min(miny, ay, by), max(maxy, ay, by)
            if ay != by:
                edges.append((ax, ay, bx, by))
    if not edges:
        return
    x_lo, x_hi = max(0, int(math.floor(minx))), min(W, int(math.ceil(maxx)) + 1)
    y_lo, y_hi = max(0, int(math.floor(miny))), min(H, int(math.ceil(maxy)) + 1)
    if x_hi <= x_lo or y_hi <= y_lo:
        return
    buckets = {}
    for ax, ay, bx, by in edges:
        d = 1 if by > ay else -1
        if d < 0:
            ax, ay, bx, by = bx, by, ax, ay
        s0 = max(int(math.ceil(ay * SUB - 0.5)), y_lo * SUB)
        s1 = min(int(math.ceil(by * SUB - 0.5)), y_hi * SUB)
        slope = (bx - ax) / (by - ay)
        for s in range(s0, s1):
            sy = (s + 0.5) / SUB
            buckets.setdefault(s, []).append((ax + (sy - ay) * slope, d))
    wd = x_hi - x_lo
    wgt = 1.0 / SUB
    row_y = None
    acc = None
    for s in sorted(buckets):
        y = s // SUB
        if y != row_y:
            if acc is not None:
                yield from _emit(row_y, x_lo, acc, wd)
            row_y, acc = y, [0.0] * (wd + 2)
        xs = sorted(buckets[s])
        if rule == "evenodd":
            spans = [(xs[i][0], xs[i + 1][0]) for i in range(0, len(xs) - 1, 2)]
        else:
            spans, wind, start = [], 0, 0.0
            for x, d in xs:
                if wind == 0:
                    start = x
                wind += d
                if wind == 0:
                    spans.append((start, x))
        for xa, xb in spans:
            xa, xb = max(xa, x_lo) - x_lo, min(xb, x_hi) - x_lo
            if xb <= xa:
                continue
            ia, ib = int(xa), int(xb)
            if ia == ib:
                acc[ia] += (xb - xa) * wgt
            else:
                acc[ia] += (ia + 1 - xa) * wgt
                acc[ia + 1] += 0  # keep length stable
                # full cells via difference array stored in the tail slot trick: use a second list
                _add_run(acc, ia + 1, ib, wgt, wd)
                acc[ib] += (xb - ib) * wgt
    if acc is not None:
        yield from _emit(row_y, x_lo, acc, wd)


_runs = {}


def _add_run(acc, a, b, w, wd):
    # accumulate full-coverage runs in a side difference array keyed by the row list id
    d = _runs.setdefault(id(acc), [0.0] * (wd + 3))
    d[a] += w
    d[b] -= w


def _emit(y, x_lo, acc, wd):
    d = _runs.pop(id(acc), None)
    if d is not None:
        run = 0.0
        for i in range(wd):
            run += d[i]
            acc[i] += run
    yield y, x_lo, [min(1.0, v) for v in acc[:wd]]


# ---- soft field (blurred coverage, used for bone volume) -----------------------------------------------------------------
class Field:
    """Blurred coverage of a polygon set sampled bilinearly at art-pixel positions; ~0 at the edge, ~1 deep inside."""

    def __init__(self, polys, w, h, radius_pts, ds=4):
        self.ds = ds
        gw, gh = int(w * S / ds) + 2, int(h * S / ds) + 2
        g = [[0.0] * gw for _ in range(gh)]
        global S_
        for y, x0, cov in coverage_small(polys, gw, gh, ds):
            row = g[y]
            for i, c in enumerate(cov):
                row[x0 + i] = c
        r = max(1, int(radius_pts * S / ds))
        for _ in range(2):
            g = [box_blur_row(row, r) for row in g]
            cols = [box_blur_row([g[y][x] for y in range(gh)], r) for x in range(gw)]
            g = [[cols[x][y] for x in range(gw)] for y in range(gh)]
        self.g, self.gw, self.gh = g, gw, gh

    def at(self, x, y):
        ds = self.ds
        fx, fy = x / ds - 0.5, y / ds - 0.5
        x0, y0 = int(fx), int(fy)
        if x0 < 0 or y0 < 0 or x0 + 1 >= self.gw or y0 + 1 >= self.gh:
            return 0.0
        tx, ty = fx - x0, fy - y0
        g = self.g
        a = g[y0][x0] * (1 - tx) + g[y0][x0 + 1] * tx
        b = g[y0 + 1][x0] * (1 - tx) + g[y0 + 1][x0 + 1] * tx
        return a * (1 - ty) + b * ty


def box_blur_row(row, r):
    n = len(row)
    pre = [0.0]
    for v in row:
        pre.append(pre[-1] + v)
    out = []
    for i in range(n):
        a, b = max(0, i - r), min(n, i + r + 1)
        out.append((pre[b] - pre[a]) / (2 * r + 1))
    return out


def coverage_small(polys, gw, gh, ds):
    global S
    saved = S
    S = saved / ds
    try:
        for item in coverage(polys, gw, gh):
            yield item
    finally:
        S = saved


# ---- shaders ------------------------------------------------------------------------------------------------------------------
def solid(c, a=1.0):
    t = (c[0], c[1], c[2], a)
    return lambda x, y: t


def bone_shader(field, top, bottom, tint=0.0):
    """Cool white bone: bright and slightly warm in the middle, shaded blue towards the edges."""
    def f(x, y):
        v = field.at(x, y)
        t = smooth(0.15, 0.85, v)
        vy = clamp((y / S - top) / max(1.0, bottom - top))
        base = mix(BONE_LO, BONE_MID, t)
        base = mix(base, BONE_HI, smooth(0.55, 1.0, v) * (1.0 - 0.35 * vy))
        return (base[0], base[1], base[2], 1.0)
    return f


def vgrad(c0, c1, y0, y1, a=1.0):
    def f(x, y):
        c = mix(c0, c1, (y / S - y0) / (y1 - y0))
        return (c[0], c[1], c[2], a)
    return f


def radial(cx, cy, r, c0, c1, a=1.0, fx=0.0, fy=0.0):
    def f(x, y):
        c = mix(c0, c1, math.hypot(x / S - cx - fx * r, y / S - cy - fy * r) / r)
        return (c[0], c[1], c[2], a)
    return f


def paint(cv, polys, shader, rule="evenodd"):
    cv.fill(polys, shader, rule)


def outline(cv, pts, width, color, a=1.0, closed=True):
    cv.fill(stroke(pts, width, closed), solid(color, a), "nonzero")


def glow_outline(cv, pts, color, closed=True):
    outline(cv, pts, 9, color, 0.10, closed)
    outline(cv, pts, 5, color, 0.22, closed)
    outline(cv, pts, 2.6, RIM, 0.95, closed)


# ---- the skull window ----------------------------------------------------------------------------------------------------------
MW, MH = 1100, 1440
EYE_A, EYE_B, EYE_Y = 312, 788, 286
EYE_R = 150          # dark socket disc
EYE_RING = 163       # outer edge of the bone ring around it
TOOTH_X0, TOOTH_PITCH, TOOTH_W = 230, 40, 36
UP_Y0, UP_Y1 = 570, 642
LO_Y0, LO_Y1 = 798, 868
SPINE_Y0, SPINE_PITCH, SPINE_H = 1062, 52, 46

LEFT_CRANIUM = [(550, 10), (430, 12), (320, 32), (225, 76), (150, 140), (100, 215), (78, 290), (82, 370), (105, 435), (140, 495), (180, 540), (206, 575)]
NASAL = [(550, 466), (508, 548), (530, 566), (550, 552), (570, 566), (592, 548)]
LEFT_JAW = [(214, 798), (222, 880), (250, 952), (312, 1008), (410, 1040), (550, 1052)]
RAMUS_L = [(112, 492), (176, 470), (282, 812), (214, 834)]
PLATE = (330, 654, 440, 136)


def cranium_outline():
    left = catmull(LEFT_CRANIUM, 12)
    left += [(212, 600), (212, 636)]
    top = left
    full = top + [(550, 642)] + [(1100 - x, y) for x, y in reversed(top)][:-0]
    # mirror: left goes top->bottom; right must go bottom->top
    right = [(MW - x, y) for x, y in reversed(top)]
    return top + right


def jaw_outline():
    left = catmull(LEFT_JAW, 12)
    return left + [(MW - x, y) for x, y in reversed(left)]


def vertebra_rows():
    return [SPINE_Y0 + i * SPINE_PITCH for i in range(7)]


def draw_main():
    cv = Canvas(MW, MH)
    crani = cranium_outline()
    jaw = jaw_outline()
    ram_l = RAMUS_L
    ram_r = [(MW - x, y) for x, y in ram_l]
    # --- spine column (behind the vertebra wings, visible between them)
    col = [(522, SPINE_Y0 - 14), (578, SPINE_Y0 - 14), (578, MH - 8), (522, MH - 8)]
    f_col = Field([col], MW, MH, 14)
    paint(cv, [col], lambda x, y: tuple(mix((88, 128, 176), (170, 206, 238), f_col.at(x, y) * 1.4)) + (1.0,))
    outline(cv, col, 2, RIM, 0.7)
    # --- vertebrae
    for i, y0 in enumerate(vertebra_rows()):
        wing = rrect_poly(278, y0, 544, SPINE_H, 22, 10)
        f = Field([wing], MW, MH, 14)
        paint(cv, [wing], bone_shader(f, y0, y0 + SPINE_H))
        glow_outline(cv, wing, (160, 210, 255))
        body = rrect_poly(486, y0 + 4, 128, SPINE_H - 8, 14, 8)
        f2 = Field([body], MW, MH, 10)
        paint(cv, [body], bone_shader(f2, y0, y0 + SPINE_H))
        outline(cv, body, 2.2, (70, 112, 156), 0.95)
        # a small spinous notch
        outline(cv, [(550 - 10, y0 + SPINE_H / 2), (550 + 10, y0 + SPINE_H / 2)], 3, (70, 112, 156), 0.55, False)
    # --- the field for the main bone (cranium + maxilla + jaw + rami share one soft field)
    bones = [crani, jaw, ram_l, ram_r]
    field = Field([crani, jaw] + [ram_l, ram_r], MW, MH, 30)
    # rami (behind the cranium, in front of the spine)
    for r in (ram_l, ram_r):
        paint(cv, [r], bone_shader(Field([r], MW, MH, 14), 470, 834))
        glow_outline(cv, r, (160, 210, 255))
    # --- jaw
    paint(cv, [jaw], bone_shader(Field([jaw], MW, MH, 34), 798, 1052))
    glow_outline(cv, jaw, (160, 210, 255))
    # chin shading line
    outline(cv, catmull([(300, 960), (410, 1004), (550, 1016), (690, 1004), (800, 960)], 8), 2, (110, 150, 196), 0.5, False)
    # --- cranium + maxilla with the nasal cavity cut out
    paint(cv, [crani, NASAL], bone_shader(Field([crani, NASAL], MW, MH, 34), 10, 642))
    glow_outline(cv, crani, (160, 210, 255))
    glow_outline(cv, NASAL, (150, 200, 245))
    # sutures: a dashed coronal line over the brow, cheek and temple creases
    for seg in dashed(catmull([(190, 126), (330, 78), (550, 62), (770, 78), (910, 126)], 14), 9, 6):
        outline(cv, seg, 1.6, (96, 138, 186), 0.85, False)
    for sx in (1, -1):
        def X(x):
            return x if sx == 1 else MW - x
        outline(cv, [(X(x), y) for x, y in catmull([(118, 470), (150, 520), (205, 566)], 8)], 1.6, (110, 150, 196), 0.6, False)
        outline(cv, [(X(x), y) for x, y in catmull([(480, 470), (492, 520), (506, 552)], 8)], 1.4, (110, 150, 196), 0.5, False)
    # --- eye sockets
    for cx in (EYE_A, EYE_B):
        paint(cv, [circle_poly(cx, EYE_Y, EYE_RING + 6)], solid((86, 124, 170)))
        paint(cv, [circle_poly(cx, EYE_Y, EYE_RING)], radial(cx, EYE_Y, EYE_RING, (214, 236, 252), (120, 160, 205), 1.0, -0.2, -0.3))
        outline(cv, circle_poly(cx, EYE_Y, EYE_RING + 6), 2.4, RIM, 0.95)
        paint(cv, [circle_poly(cx, EYE_Y, EYE_R + 2)], radial(cx, EYE_Y, EYE_R + 2, (14, 34, 58), (3, 8, 14)))
        outline(cv, circle_poly(cx, EYE_Y, EYE_R + 2), 2, (30, 62, 96), 1.0)
        ticks = []
        for k in range(60):
            a = math.radians(k * 6)
            r0, r1 = EYE_R + 5, EYE_R + (12 if k % 5 == 0 else 9)
            ticks.append([(cx + r0 * math.cos(a), EYE_Y + r0 * math.sin(a)), (cx + r1 * math.cos(a), EYE_Y + r1 * math.sin(a))])
        for t in ticks:
            outline(cv, t, 1.6, (80, 120, 168), 0.9, False)
    # --- upper teeth (the hot cue pads sit on them)
    for i in range(16):
        x = TOOTH_X0 + i * TOOTH_PITCH
        tooth = rrect_poly(x - 2, UP_Y0 - 6, TOOTH_W + 4, UP_Y1 - UP_Y0 + 6, 10, 6)
        paint(cv, [tooth], vgrad((250, 253, 255), (170, 200, 230), UP_Y0, UP_Y1))
        outline(cv, tooth, 1.8, (84, 126, 176), 0.95)
    # --- lower teeth (buttons sit on them)
    for i in range(16):
        x = TOOTH_X0 + i * TOOTH_PITCH
        tooth = rrect_poly(x - 2, LO_Y0, TOOTH_W + 4, LO_Y1 - LO_Y0 + 4, 10, 6)
        paint(cv, [tooth], vgrad((170, 200, 230), (250, 253, 255), LO_Y0, LO_Y1))
        outline(cv, tooth, 1.8, (84, 126, 176), 0.95)
    # --- the dark plate in the open mouth (mixer)
    px_, py_, pw_, ph_ = PLATE
    plate = rrect_poly(px_, py_, pw_, ph_, 22, 10)
    paint(cv, [plate], vgrad((10, 30, 52), (4, 12, 22), py_, py_ + ph_, 0.96))
    outline(cv, plate, 2.4, (150, 210, 255), 0.85)
    outline(cv, rrect_poly(px_ + 5, py_ + 5, pw_ - 10, ph_ - 10, 18, 8), 1, (60, 110, 160), 0.6)
    # --- status pills in the bottom corners
    for x0 in (14, MW - 14 - 250):
        pill = rrect_poly(x0, 1402, 250, 28, 14, 10)
        paint(cv, [pill], solid((6, 14, 24), 0.94))
        outline(cv, pill, 2, (110, 170, 220), 0.8)
    # --- resize grip (lower right corner of the cranium)
    gx, gy = 968, 462
    paint(cv, [circle_poly(gx, gy, 20)], solid((60, 96, 140)))
    outline(cv, circle_poly(gx, gy, 20), 2, RIM, 0.9)
    for k in (-1, 0, 1):
        outline(cv, [(gx - 8 + k * 6, gy + 8 + k * 6 * 0), (gx + 8 + k * 6, gy - 8 + k * 6 * 0)], 2, RIM, 0.7, False)
    return cv


# ---- the trays --------------------------------------------------------------------------------------------------------------------
def tray(w, h, accent=(150, 210, 255), inset=(18, 16, 18, 16)):
    cv = Canvas(w, h)
    body = rrect_poly(2, 2, w - 4, h - 4, 30, 12)
    # joint notches at the corners (transparent)
    notches = [circle_poly(cx, cy, 12) for cx, cy in ((10, 10), (w - 10, 10), (10, h - 10), (w - 10, h - 10))]
    f = Field([body], w, h, 22)
    paint(cv, [body] + notches, bone_shader(f, 0, h))
    glow_outline(cv, body, accent)
    for n in notches:
        outline(cv, n, 2, RIM, 0.8)
    l, t, r, b = inset
    panel = rrect_poly(l, t, w - l - r, h - t - b, 16, 8)
    paint(cv, [panel], vgrad((10, 26, 44), (6, 14, 24), t, h - b, 0.97))
    outline(cv, panel, 2, (110, 170, 220), 0.85)
    return cv


def draw_grid():
    return tray(700, 300, inset=(16, 36, 16, 14))


def draw_tools():
    return tray(700, 520, inset=(16, 14, 16, 14))


def draw_settings():
    return tray(720, 560, inset=(16, 14, 16, 14))


def draw_browser():
    w, h = 800, 470
    cv = Canvas(w, h)
    panel = rrect_poly(90, 40, 620, 390, 22, 10)
    frame = rrect_poly(78, 28, 644, 414, 30, 10)
    # ribs: six bent bars per side hanging off the frame, transparent between them
    ribs = []
    for i in range(6):
        y = 74 + i * 62
        left = bezier((92, y), (4, y - 6), (24, y + 46), 28)
        ribs.append(left)
        ribs.append([(w - x, yy) for x, yy in left])
    # clavicle across the top and a notch-ended sternum bar at the bottom
    clav = rrect_poly(56, 6, 688, 24, 12, 8)
    for rb in ribs:
        polys = stroke(rb, 22)
        f = Field(polys, w, h, 8)
        paint(cv, polys, bone_shader(f, 0, h), "nonzero")
        outline(cv, rb, 22, (160, 210, 255), 0.0, False)
        for poly_pts in (rb,):
            pass
    for rb in ribs:
        # rim lines of the ribs (two thin strokes along the bar edges)
        for off in (-11, 11):
            edge = []
            for k, (x, y) in enumerate(rb):
                x2, y2 = rb[min(k + 1, len(rb) - 1)]
                x1, y1 = rb[max(k - 1, 0)]
                dx, dy = x2 - x1, y2 - y1
                ln = math.hypot(dx, dy) or 1
                edge.append((x - dy / ln * off, y + dx / ln * off))
            outline(cv, edge, 1.6, (150, 200, 245), 0.8, False)
    paint(cv, [clav], bone_shader(Field([clav], w, h, 8), 0, h))
    glow_outline(cv, clav, (160, 210, 255))
    f = Field([frame], w, h, 20)
    paint(cv, [frame], bone_shader(f, 0, h))
    glow_outline(cv, frame, (160, 210, 255))
    paint(cv, [panel], vgrad((10, 26, 44), (6, 14, 24), 40, 430, 0.97))
    outline(cv, panel, 2, (110, 170, 220), 0.85)
    return cv


def draw_sampler():
    w, h = 500, 190
    cv = Canvas(w, h)
    shaft = rrect_poly(48, 26, 404, 138, 24, 10)
    ends = [circle_poly(cx, cy, 36) for cx, cy in ((40, 50), (40, 140), (460, 50), (460, 140))]
    allp = [shaft] + ends
    f = Field(allp, w, h, 22)
    paint(cv, allp, bone_shader(f, 0, h), "nonzero")
    for p in ends + [shaft]:
        pass
    # outline of the union: stroke each piece, then paint the shaft interior over the inner ring strokes
    for p in ends:
        outline(cv, p, 5, (160, 210, 255), 0.22)
        outline(cv, p, 2.6, RIM, 0.95)
    outline(cv, shaft, 5, (160, 210, 255), 0.22)
    paint(cv, [shaft], bone_shader(Field([shaft] + [circle_poly(cx, cy, 30) for cx, cy in ((40, 50), (40, 140), (460, 50), (460, 140))], w, h, 22), 0, h))
    paint(cv, [circle_poly(cx, cy, 33) for cx, cy in ((40, 50), (40, 140), (460, 50), (460, 140))], bone_shader(f, 0, h), "nonzero")
    outline(cv, shaft, 2.6, RIM, 0.0)
    inner = rrect_poly(60, 34, 380, 122, 16, 8)
    paint(cv, [inner], vgrad((10, 26, 44), (6, 14, 24), 34, 156, 0.97))
    outline(cv, inner, 2, (110, 170, 220), 0.85)
    return cv


def draw_all(outdir):
    os.makedirs(outdir, exist_ok=True)
    jobs = [("main.png", draw_main), ("browser.png", draw_browser), ("sampler.png", draw_sampler), ("grid.png", draw_grid), ("tools.png", draw_tools),
            ("settings.png", draw_settings)]
    only = os.environ.get("SKULL_ART_ONLY")
    for name, fn in jobs:
        if only and name not in only.split(","):
            continue
        fn().save(os.path.join(outdir, name))
        print("wrote", name)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "skins", "skull.dhskin", "img")
    draw_all(out)
