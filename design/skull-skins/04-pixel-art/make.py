import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1560
D = Doc(W, H, '#3a2470'); D.rootx = ' shape-rendering="crispEdges"'
PX = '"Menlo","Courier New",monospace'
DARK, BONE, SHADE, HI = '#0f0820', '#f4efe0', '#cfc6a8', '#fffdf4'
CY, PK, YL, LM = '#3be3ff', '#ff4fa3', '#ffd23b', '#6bff6b'

def pdisc(cx, cy, r, p, fill, stroke=None):
    o = ''
    for gy in range(-int(r // p) - 1, int(r // p) + 2):
        for gx in range(-int(r // p) - 1, int(r // p) + 2):
            d = math.hypot(gx * p, gy * p)
            if d <= r:
                edge = d > r - p * 1.2
                o += '<rect x="%s" y="%s" width="%s" height="%s" fill="%s"/>' % (n(cx + gx * p - p / 2), n(cy + gy * p - p / 2), p, p, (stroke or fill) if edge else fill)
    return o

class Pix(Theme):
    font = PX; ink = DARK; dim = '#4a3a6c'; btn_fill = DARK; btn_stroke = BONE; btn_text = BONE
    accent = YL; on_text = DARK; knob_fill = DARK; knob_stroke = BONE; track = DARK; meter_off = '#6a5a8a'
    pad_off = BONE; wave_bg = '#0c0818'; colA = '#00b8e6'; colB = '#ff3d95'; btn_r = 0; pad_r = 0
    def text(s, x, y, t, size=12, fill=None, anchor='middle', weight='normal', mono=False, halo=None, extra=''):
        return Theme.text(s, x, y, t, size, fill, anchor, '700', True, halo, extra)
    def shape(s, x, y, w, h, fill, stroke, sw=None, r=None, extra=''):
        return '<rect x="%s" y="%s" width="%s" height="%s" fill="#0006"/><rect x="%s" y="%s" width="%s" height="%s" fill="%s" stroke="%s" stroke-width="4"/>' % (n(x + 4), n(y + 4), n(w), n(h), n(x), n(y), n(w), n(h), fill, stroke)
    def pad(s, x, y, w, h, i, lit, col=None, label=None):
        col = col or HC[i % 8]
        o = s.shape(x, y, w, h, col if lit else BONE, DARK)
        if lit:
            o += '<rect x="%s" y="%s" width="%s" height="8" fill="#fff" fill-opacity=".45"/>' % (n(x + 4), n(y + 4), n(w - 8))
        else:
            o += '<rect x="%s" y="%s" width="12" height="12" fill="%s"/>' % (n(x + 6), n(y + 6), col)
        o += s.text(x + w / 2, y + h / 2 + 7, label or str(i + 1), 18, DARK)
        return o
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        col = col or s.accent
        ang = 135 + 270 * val
        o = pdisc(cx + 3, cy + 3, r, 4, '#0006') + pdisc(cx, cy, r, 4, DARK, BONE)
        for k in range(0, 270, 30):
            if k <= 270 * val:
                a = math.radians(135 + k)
                o += '<rect x="%s" y="%s" width="4" height="4" fill="%s"/>' % (n(cx + (r + 4) * math.cos(a) - 2), n(cy + (r + 4) * math.sin(a) - 2), col)
        for t in range(2, int(r - 4), 4):
            o += '<rect x="%s" y="%s" width="4" height="4" fill="%s"/>' % (n(cx + t * math.cos(math.radians(ang)) - 2), n(cy + t * math.sin(math.radians(ang)) - 2), BONE)
        if label:
            o += s.text(cx, cy + r + 15, label, 11, s.dim)
        return o
    def fader_v(s, x, y, w, h, val, col=None, label=None):
        col = col or s.accent; cx = x + w / 2
        o = '<rect x="%s" y="%s" width="8" height="%s" fill="%s" stroke="%s" stroke-width="2"/>' % (n(cx - 4), n(y), n(h), DARK, BONE)
        cy = y + h * (1 - val)
        o += s.shape(x, cy - 12, w, 24, col, DARK) + '<rect x="%s" y="%s" width="%s" height="4" fill="%s"/>' % (n(x + 4), n(cy - 2), n(w - 8), DARK)
        return o
    def fader_h(s, x, y, w, h, val, col=None, label=None):
        col = col or s.accent; cy = y + h / 2
        o = '<rect x="%s" y="%s" width="%s" height="8" fill="%s" stroke="%s" stroke-width="2"/>' % (n(x), n(cy - 4), n(w), DARK, BONE)
        cx = x + (w - 18) * val
        o += s.shape(cx, y + 3, 18, h - 6, col, DARK)
        return o
    def meter(s, x, y, w, h, val, horiz=False, segs=14):
        return Theme.meter(s, x, y, w, h, val, horiz, segs).replace('rx="1"', 'rx="0"')
    def jog(s, cx, cy, r, ang=40, col=None):
        col = col or s.accent
        o = pdisc(cx + 4, cy + 4, r, 8, '#0006') + pdisc(cx, cy, r, 8, DARK, col) + pdisc(cx, cy, r * .6, 8, '#3a2a60', '#5a4a80')
        for a in range(0, 360, 30):
            o += '<rect x="%s" y="%s" width="4" height="4" fill="%s"/>' % (n(cx + (r - 6) * math.cos(math.radians(a)) - 2), n(cy + (r - 6) * math.sin(math.radians(a)) - 2), BONE)
        o += '<rect x="%s" y="%s" width="12" height="12" fill="%s" stroke="#fff" stroke-width="2"/>' % (n(cx + (r - 14) * math.cos(math.radians(ang)) - 6), n(cy + (r - 14) * math.sin(math.radians(ang)) - 6), col)
        return o
T = Pix(); DK = Pix(ink=BONE, dim='#b9aee0', btn_fill='#201533')
# starfield
import random
left = [(800, 20), (560, 30), (380, 80), (280, 180), (240, 300), (240, 420), (260, 500), (220, 560), (240, 640), (300, 700), (320, 780), (320, 880), (340, 940), (420, 1000), (520, 1060), (540, 1200), (560, 1360), (600, 1450), (700, 1505), (800, 1515)]
pts = sym(left, W); poly = sample(pts, steps=4)
P = 20
cols, rows = W // P, H // P
NOSE = [(740,640),(860,640),(860,740),(740,740)]
G = [[inside(poly, c * P + P / 2, r * P + P / 2) and not inside(NOSE, c * P + P / 2, r * P + P / 2) for c in range(cols)] for r in range(rows)]
def g(c, r): return G[r][c] if 0 <= r < rows and 0 <= c < cols else False
runs = {}
for r in range(rows):
    for c in range(cols):
        if not G[r][c]:
            continue
        edge = not (g(c - 1, r) and g(c + 1, r) and g(c, r - 1) and g(c, r + 1))
        if edge: col = DARK
        elif not (g(c - 1, r - 1) and g(c - 2, r) and g(c, r - 2)): col = HI
        elif not (g(c + 1, r + 1) and g(c + 2, r) and g(c, r + 2)): col = SHADE
        else: col = BONE
        runs.setdefault(col, []).append((c, r))
for col, cells in runs.items():
    d = ''
    for c, r in cells:
        d += 'M%d,%dh%dv%dh-%dz' % (c * P, r * P, P, P, P)
    D.add('<path d="%s" fill="%s"/>' % (d, col))
# ears (pixel blocks) drawn as rect stacks behind? draw on top edges
for sx in (1, -1):
    def X(x, w=0): return x if sx == 1 else W - x - w
    D.add('<rect x="%d" y="300" width="200" height="380" fill="%s" stroke="%s" stroke-width="8"/><rect x="%d" y="308" width="184" height="12" fill="%s"/>' % (X(40, 200), BONE, DARK, X(48, 184), HI))
    D.add('<rect x="%d" y="610" width="56" height="52" fill="%s"/>' % (X(112, 56), DARK))
# eyes
for cx, d, col in ((440, 'A', T.colA), (1160, 'B', T.colB)):
    x0, y0, w, h = cx - 160, 280, 320, 300
    D.add('<path d="M%d,%d h%d v20 h20 v%d h-20 v20 h-%d v-20 h-20 v-%d h20 v-20z" fill="%s" stroke="%s" stroke-width="8"/>' % (x0 + 20, y0, w - 40, h - 40, w - 40, h - 40, DARK, DARK) if False else
          '<rect x="%d" y="%d" width="%d" height="%d" fill="#0c0818" stroke="%s" stroke-width="8"/>' % (x0, y0, w, h, DARK))
    for (qx, qy) in ((x0, y0), (x0 + w - 24, y0), (x0, y0 + h - 24), (x0 + w - 24, y0 + h - 24)):
        D.add('<rect x="%d" y="%d" width="24" height="24" fill="%s"/>' % (qx - 4, qy - 4, BONE))
        D.add('<rect x="%d" y="%d" width="24" height="24" fill="%s"/>' % (qx + (-4 if qx == x0 else 4), qy + (-4 if qy == y0 else 4), DARK) if False else '')
    D.add('<clipPath id="e%s"><rect x="%d" y="%d" width="%d" height="%d"/></clipPath>' % (d, x0 + 4, y0 + 4, w - 8, h - 8))
    D.add('<g clip-path="url(#e%s)"><rect x="%d" y="%d" width="%d" height="96" fill="#05030c"/>' % (d, x0, y0 + 40, w) + wave(x0 + 8, y0 + 44, w - 16, 88, DECK[d]['seed'], col, '#fff', head=(w - 16) / 2, cues=DECK[d]['cues'][:3], pitch=8, grid=False) + '</g>')
    D.add(T.text(x0 + 24, y0 + 28, 'DECK ' + d, 16, col, 'start') + T.text(cx, y0 + h - 12, 'JOG', 11, '#9a8aba'))
    D.add(T.jog(cx, y0 + 214, 62, 300 if d == 'A' else 230, col))
    D.put('jog ' + d, ('', 320, 300), x0, y0)
D.put('wave', ('', 304, 88), 288, 324)
# nose
D.add('<path d="M740,640 h120 v100 h-120z" fill="none" stroke="%s" stroke-width="8"/>' % DARK)
# teeth: upper = hot cue pads, lower = sampler
tw, pitch, tx0 = 54, 58.75, 330
for i in range(16):
    d, j = ('A', i) if i < 8 else ('B', i - 8)
    x = tx0 + i * pitch
    D.add('<g>' + T.pad(x, 790, tw, 78, j, j in DECK[d]['lit'], HC[j]) + '</g>')
    D.add(T.pad(x, 880, tw, 54, i % 8, i in (1, 4, 6, 9, 12), HC[i % 8], 'S%d' % (i + 1)))
D.names['pads A'] = (tx0, 790, 8 * pitch, 78); D.names['pads B'] = (tx0 + 8 * pitch, 790, 8 * pitch, 78); D.names['sampler'] = (tx0, 880, 940, 54)
both(D, T, 'info', b_info, 400, 80, w=320)
both(D, T, 'overview', b_overview, 405, 178)
both(D, T, 'toggles', b_toggles, 330, 228, orient='row')
both(D, T, 'transport', b_transport, 262, 594)
both(D, T, 'keyshift', b_keyshift, 484, 602)
both(D, T, 'loop', b_loop, 262, 652)
both(D, T, 'padmodes', b_padmodes, 466, 652)
both(D, T, 'fx', b_fx, 40, 322) if False else None
for d, col in (('A', T.colA), ('B', T.colB)):
    pass
both(D, T, 'fx', b_fx, 52, 322)
both(D, T, 'tempo', b_tempo, 120, 440, h=90)
D.put('master', b_master(T), 678, 470)
D.put('mic', b_mic(T), 710, 330)
# vertical strips in the long jaw
D.put('ch1', b_strip(T, 1, T.colA, 90), 698, 996)
D.put('ch2', b_strip(T, 2, T.colB, 90), 806, 996)
D.put('xfader', b_xfader(T, w=420), 590, 1420) if False else None
D.put('xfader', b_xfader(T, w=300), 650, 1428)
# bottom-left browser, bottom-right misc
D.add('<path d="M28,1000 H572 V1016 H588 V1420 H572 V1436 H28 V1420 H12 V1016 H28Z" fill="#0f0820" stroke="#f4efe0" stroke-width="4"/>')
D.put('browser', b_browser(DK, YL, 560, 410, 'SAVE FILE  ·  TRACK BROWSER', nrows=8), 20, 1010)
D.put('rec', b_rec(T), 1200, 1060)
D.put('appbtns', b_appbtns(T), 1200, 1130)
D.add(T.text(1200, 1230, '★ DECKHAND  PIXEL EDITION ★', 16, YL, 'start') + T.text(1200, 1256, 'PRESS START TO MIX', 14, CY, 'start'))
D.put('status', b_status(DK), 20, 1500)
D.save(os.path.dirname(__file__))
