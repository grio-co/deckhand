"""Shared helpers for the DeckHand skull skin mockups (standard library only).

Each concept's make.py builds an SVG with a Doc, a Theme (control look) and
block functions (control families).  Blocks render at local (0,0) and return
(svg, w, h); Doc.put places them, mirrors them for deck B, records names for the
inventory check and warns about overlaps / blocks falling outside a silhouette.
"""
import math, random, subprocess, os

FONT = '"Helvetica Neue",Helvetica,Arial,sans-serif'
MONO = 'Menlo,Monaco,"Courier New",monospace'
HC = ['#ff3b5c', '#ff9f1a', '#ffe23b', '#4cd964', '#2fd8d0', '#3b8cff', '#a46bff', '#ff5fd0']
CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'

DECK = {
    'A': dict(title='Midnight Ossuary', artist='The Marrow Kings', bpm='128.0', key='8A', t='02:41.3', rem='-03:12.4',
              shift='+2', seed=11, head=0.42, cues=[0.08, 0.2, 0.31, 0.62, 0.8], lit=[0, 1, 3, 5], loop='4', fxn=['ECHO', 'REVERB', 'FLANGER']),
    'B': dict(title='Bone China Heart', artist='Calcium Sisters', bpm='126.0', key='9A', t='01:17.8', rem='-04:36.0',
              shift='-1', seed=29, head=0.35, cues=[0.1, 0.27, 0.5, 0.7], lit=[0, 2, 4, 6, 7], loop='8', fxn=['FILTER', 'PHASER', 'CRUSH']),
}


def n(v):
    s = '%.1f' % v
    return s[:-2] if s.endswith('.0') else s


def esc(t):
    return str(t).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def smooth(pts, closed=True, k=0.5):
    """Catmull-Rom through pts -> cubic bezier path string."""
    N = len(pts)
    if N < 3:
        return 'M' + ' L'.join('%s,%s' % (n(x), n(y)) for x, y in pts)
    d = 'M%s,%s' % (n(pts[0][0]), n(pts[0][1]))
    rng = range(N if closed else N - 1)
    for i in rng:
        p0 = pts[(i - 1) % N] if (closed or i > 0) else pts[i]
        p1 = pts[i]
        p2 = pts[(i + 1) % N]
        p3 = pts[(i + 2) % N] if (closed or i + 2 < N) else p2
        c1 = (p1[0] + (p2[0] - p0[0]) * k / 3 * 2 / 2, p1[1] + (p2[1] - p0[1]) * k / 3 * 2 / 2)
        c2 = (p2[0] - (p3[0] - p1[0]) * k / 3 * 2 / 2, p2[1] - (p3[1] - p1[1]) * k / 3 * 2 / 2)
        d += ' C%s,%s %s,%s %s,%s' % (n(c1[0]), n(c1[1]), n(c2[0]), n(c2[1]), n(p2[0]), n(p2[1]))
    return d + (' Z' if closed else '')


def sym(left, W):
    """left: points from top-centre going down the left side to bottom-centre."""
    right = [(W - x, y) for x, y in reversed(left[1:-1])]
    return left + right


def sample(pts, closed=True, steps=8, k=0.5):
    """Polygon approximating the smooth curve through pts (for inside tests)."""
    N = len(pts)
    out = []
    for i in range(N):
        p0, p1, p2, p3 = pts[(i - 1) % N], pts[i], pts[(i + 1) % N], pts[(i + 2) % N]
        c1 = (p1[0] + (p2[0] - p0[0]) * k / 3, p1[1] + (p2[1] - p0[1]) * k / 3)
        c2 = (p2[0] - (p3[0] - p1[0]) * k / 3, p2[1] - (p3[1] - p1[1]) * k / 3)
        for s in range(steps):
            t = s / steps
            u = 1 - t
            out.append((u ** 3 * p1[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t ** 3 * p2[0],
                        u ** 3 * p1[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t ** 3 * p2[1]))
    return out


def inside(poly, x, y):
    c = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi + 1e-9) + xi:
            c = not c
        j = i
    return c


def arc(cx, cy, r, a0, a1):
    """SVG arc path, angles in degrees clockwise from east."""
    x0, y0 = cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0))
    x1, y1 = cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1))
    large = 1 if abs(a1 - a0) > 180 else 0
    return 'M%s,%s A%s,%s 0 %d 1 %s,%s' % (n(x0), n(y0), n(r), n(r), large, n(x1), n(y1))


class Doc:
    def __init__(s, W, H, bg='#15161a', name='mockup'):
        s.W, s.H, s.cx = W, H, W / 2
        s.defs, s.body, s.boxes, s.names, s.bg = [], [], [], {}, bg
        s.warn = []
        s.rootx = ''

    def d(s, x):
        s.defs.append(x)

    def add(s, x):
        s.body.append(x)

    def put(s, name, block, x, y, mirror=False, poly=None, w_out=None):
        svg, w, h = block
        if mirror:
            x = 2 * s.cx - x - w
        s.add('<g transform="translate(%s,%s)">%s</g>' % (n(x), n(y), svg))
        s.names[name] = (x, y, w, h)
        for nm, (bx, by, bw, bh) in s.names.items():
            if nm != name and x < bx + bw - 2 and bx < x + w - 2 and y < by + bh - 2 and by < y + h - 2:
                s.warn.append('overlap %s / %s' % (name, nm))
        if poly:
            for px, py in ((x, y), (x + w, y), (x, y + h), (x + w, y + h)):
                if not inside(poly, px, py):
                    s.warn.append('outside silhouette: %s corner (%d,%d)' % (name, px, py))
                    break
        if x < 0 or y < 0 or x + w > s.W or y + h > s.H:
            s.warn.append('off canvas: ' + name)
        return (x, y, w, h)

    def svg(s, wall=False):
        w = ''
        if wall:
            w = ('<linearGradient id="wpg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8c97ab"/><stop offset="1" stop-color="#454f63"/></linearGradient>'
                 '<filter id="wpb" x="-50%%" y="-50%%" width="200%%" height="200%%"><feGaussianBlur stdDeviation="90"/></filter>')
            wb = ('<rect width="%d" height="%d" fill="url(#wpg)"/><g filter="url(#wpb)" opacity=".55"><ellipse cx="%d" cy="%d" rx="380" ry="260" fill="#c9d3e6"/>'
                  '<ellipse cx="%d" cy="%d" rx="420" ry="300" fill="#2a3346"/><ellipse cx="%d" cy="%d" rx="300" ry="240" fill="#7f6aa8"/></g>' %
                  (s.W, s.H, s.W * .25, s.H * .25, s.W * .8, s.H * .85, s.W * .85, s.H * .2))
        else:
            wb = ''
        return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" font-family=\'%s\'%s>'
                '<defs>%s%s</defs>%s%s</svg>' %
                (s.W, s.H, s.W, s.H, FONT, s.rootx, w, ''.join(s.defs), wb, ''.join(s.body)))

    def save(s, folder, required=None):
        os.makedirs(folder, exist_ok=True)
        p = os.path.join(folder, 'mockup.svg')
        open(p, 'w').write(s.svg())
        open(os.path.join(folder, '_wall.svg'), 'w').write(s.svg(True))
        miss = [r for r in (required or REQUIRED) if r not in s.names]
        for m in miss:
            s.warn.append('MISSING ' + m)
        for w in s.warn:
            print('  WARN', w)
        render(os.path.join(folder, '_wall.svg'), os.path.join(folder, 'mockup.png'), s.W, s.H)
        render(p, os.path.join(folder, 'mockup-transparent.png'), s.W, s.H, True)
        os.remove(os.path.join(folder, '_wall.svg'))


def render(svg, png, W, H, transparent=False):
    subprocess.run([CHROME, '--headless=new'] + (['--default-background-color=00000000'] if transparent else []) + [ '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                    '--screenshot=' + png, '--window-size=%d,%d' % (W, H), 'file://' + os.path.abspath(svg)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


REQUIRED = ['jog A', 'jog B', 'wave', 'transport A', 'transport B', 'toggles A', 'toggles B', 'keyshift A', 'keyshift B',
            'loop A', 'loop B', 'fx A', 'fx B', 'tempo A', 'tempo B', 'info A', 'info B', 'overview A', 'overview B',
            'pads A', 'pads B', 'padmodes A', 'padmodes B', 'ch1', 'ch2', 'xfader', 'master', 'mic', 'sampler', 'rec',
            'appbtns', 'status', 'browser']


# ---------------------------------------------------------------- waveform
def envelope(seed, N):
    r = random.Random(seed)
    out = []
    for i in range(N):
        beat = (i % 8) / 8.0
        kick = max(0.0, 1 - beat * 3.2)
        sec = 0.55 + 0.35 * math.sin(i / 37.0 + seed) * math.sin(i / 11.0)
        a = (0.22 + 0.55 * kick + 0.15 * r.random()) * (0.6 + 0.4 * sec)
        out.append(max(0.06, min(1.0, a)))
    return out


def wave(x, y, w, h, seed, col, core='#fff', head=None, cues=(), pitch=4, past_op=0.5, grid=True, offset=0, headcol='#fff',
         labels=True, mono_dim=None):
    """Scrolling waveform; head is playhead x in local coords (default centre)."""
    N = int(w / pitch)
    env = envelope(seed, N + offset + 2)[offset:]
    cy = y + h / 2
    head = w / 2 if head is None else head
    past, fut, cpast, cfut = [], [], [], []
    for i in range(N):
        px = x + i * pitch + pitch / 2
        a = env[i] * h / 2
        seg = 'M%s,%s V%s' % (n(px), n(cy - a), n(cy + a))
        c = 'M%s,%s V%s' % (n(px), n(cy - a * 0.5), n(cy + a * 0.5))
        (past if i * pitch < head else fut).append(seg)
        (cpast if i * pitch < head else cfut).append(c)
    sw = pitch - 1.2
    o = ['<g>']
    if grid:
        gd = ''.join('M%s,%s V%s' % (n(x + i * pitch * 8 + 2), n(y), n(y + h)) for i in range(int(N / 8) + 1))
        o.append('<path d="%s" stroke="%s" stroke-opacity=".25" stroke-width="1" fill="none"/>' % (gd, core))
    o.append('<path d="%s" stroke="%s" stroke-width="%s" stroke-opacity="%s"/>' % (''.join(past), col, n(sw), past_op))
    o.append('<path d="%s" stroke="%s" stroke-width="%s"/>' % (''.join(fut), col, n(sw)))
    o.append('<path d="%s" stroke="%s" stroke-width="%s" stroke-opacity="%s"/>' % (''.join(cpast), core, n(sw), past_op * .8))
    o.append('<path d="%s" stroke="%s" stroke-width="%s" stroke-opacity=".85"/>' % (''.join(cfut), core, n(sw)))
    for k, c in enumerate(cues):
        cxp = x + c * w
        o.append('<path d="M%s,%s V%s" stroke="%s" stroke-width="2"/>' % (n(cxp), n(y), n(y + h), HC[k % 8]))
        if labels:
            o.append('<path d="M%s,%s h14 v14 h-14z" fill="%s"/><text x="%s" y="%s" font-size="11" font-weight="700" fill="#000" text-anchor="middle">%s</text>' %
                     (n(cxp), n(y), HC[k % 8], n(cxp + 7), n(y + 11), k + 1))
    o.append('<path d="M%s,%s V%s" stroke="%s" stroke-width="2.5"/>' % (n(x + head), n(y - 4), n(y + h + 4), headcol))
    o.append('</g>')
    return ''.join(o)


# ---------------------------------------------------------------- theme
class Theme:
    font = FONT
    mono = MONO
    ink = '#e8e8ea'
    dim = '#9a9aa6'
    halo = None            # text outline colour for legibility over art
    btn_fill = '#2a2b31'
    btn_stroke = '#5a5b66'
    btn_text = '#e6e6ea'
    btn_r = 6
    btn_sw = 1.5
    accent = '#4cc9f0'
    on_text = '#05090c'
    colA = '#38d0ff'
    colB = '#ff5fa8'
    knob_fill = '#33343b'
    knob_stroke = '#6a6b78'
    track = '#0c0d10'
    pad_off = '#202127'
    pad_r = 8
    meter_off = '#23242a'
    core = '#ffffff'
    wave_bg = '#0a0b0e'

    def __init__(s, **kw):
        s.__dict__.update(kw)

    # text -------------------------------------------------------------
    def text(s, x, y, t, size=12, fill=None, anchor='middle', weight='normal', mono=False, halo=None, extra=''):
        halo = halo if halo is not None else s.halo
        h = ' stroke="%s" stroke-width="%s" stroke-linejoin="round" paint-order="stroke"' % (halo, n(max(2.5, size / 5))) if halo else ''
        return '<text x="%s" y="%s" font-size="%s" fill="%s" text-anchor="%s" font-weight="%s"%s%s%s>%s</text>' % (
            n(x), n(y), n(size), fill or s.ink, anchor, weight, ' font-family=\'%s\'' % s.mono if mono else '', h, extra, esc(t))

    # primitives -------------------------------------------------------
    def shape(s, x, y, w, h, fill, stroke, sw=None, r=None, extra=''):
        r = s.btn_r if r is None else r
        return '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="%s" stroke="%s" stroke-width="%s" %s/>' % (
            n(x), n(y), n(w), n(h), n(r), fill, stroke, n(sw or s.btn_sw), extra)

    def btn(s, x, y, w, h, txt='', on=False, col=None, fs=12, icon=None):
        col = col or s.accent
        o = s.shape(x, y, w, h, col if on else s.btn_fill, col if on else s.btn_stroke)
        tc = s.on_text if on else s.btn_text
        cx, cy = x + w / 2, y + h / 2
        if icon == 'play':
            o += '<path d="M%s,%s l0,%s l%s,-%s z" fill="%s"/>' % (n(cx - 6), n(cy - 9), n(18), n(16), n(9), tc)
        elif icon == 'pause':
            o += '<path d="M%s,%s v18 M%s,%s v18" stroke="%s" stroke-width="5"/>' % (n(cx - 5), n(cy - 9), n(cx + 5), n(cy - 9), tc)
        if txt:
            fs = max(11, fs)
            if len(txt) * fs * 0.62 > w - 4 and fs > 11:
                fs = 11
            o += s.text(cx, cy + fs * 0.36, txt, fs, tc, weight='600', halo='')
        return o

    def pad(s, x, y, w, h, i, lit, col=None, label=None):
        col = col or HC[i % 8]
        o = s.shape(x, y, w, h, col if lit else s.pad_off, col, 2, s.pad_r)
        if lit:
            o += '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="#fff" fill-opacity=".22"/>' % (n(x + 3), n(y + 3), n(w - 6), n(h * .38), n(s.pad_r - 2))
        o += s.text(x + w / 2, y + h / 2 + 6, label or str(i + 1), 17, s.on_text if lit else col, weight='700', halo='')
        return o

    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        col = col or s.accent
        a0, a1 = 135, 405
        ang = a0 + (a1 - a0) * val
        o = '<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="1.5"/>' % (n(cx), n(cy), n(r), s.knob_fill, s.knob_stroke)
        o += '<path d="%s" stroke="%s" stroke-width="3" fill="none" stroke-linecap="round" stroke-opacity=".35"/>' % (arc(cx, cy, r - 4, a0, a1), s.knob_stroke)
        if bipolar:
            o += '<path d="%s" stroke="%s" stroke-width="3" fill="none" stroke-linecap="round"/>' % (arc(cx, cy, r - 4, min(270, ang), max(270, ang)), col)
        else:
            o += '<path d="%s" stroke="%s" stroke-width="3" fill="none" stroke-linecap="round"/>' % (arc(cx, cy, r - 4, a0, ang), col)
        px, py = cx + (r - 8) * math.cos(math.radians(ang)), cy + (r - 8) * math.sin(math.radians(ang))
        o += '<path d="M%s,%s L%s,%s" stroke="%s" stroke-width="3" stroke-linecap="round"/>' % (n(cx), n(cy), n(px), n(py), s.ink)
        if label:
            o += s.text(cx, cy + r + 13, label, 11, s.dim, weight='600')
        return o

    def fader_v(s, x, y, w, h, val, col=None, label=None):
        col = col or s.accent
        cx = x + w / 2
        o = '<rect x="%s" y="%s" width="6" height="%s" rx="3" fill="%s" stroke="%s"/>' % (n(cx - 3), n(y), n(h), s.track, s.knob_stroke)
        for k in range(11):
            o += '<path d="M%s,%s h%s" stroke="%s" stroke-opacity=".6"/>' % (n(x), n(y + k * h / 10), n(w * .3), s.dim)
            o += '<path d="M%s,%s h%s" stroke="%s" stroke-opacity=".6"/>' % (n(x + w * .7), n(y + k * h / 10), n(w * .3), s.dim)
        cy = y + h * (1 - val)
        ch = 26
        o += s.shape(x + 1, cy - ch / 2, w - 2, ch, s.knob_fill, col, 2, 4)
        o += '<path d="M%s,%s h%s" stroke="%s" stroke-width="3"/>' % (n(x + 5), n(cy), n(w - 10), col)
        if label:
            o += s.text(cx, y + h + 15, label, 11, s.dim, weight='600')
        return o

    def fader_h(s, x, y, w, h, val, col=None, label=None):
        col = col or s.accent
        cy = y + h / 2
        o = '<rect x="%s" y="%s" width="%s" height="6" rx="3" fill="%s" stroke="%s"/>' % (n(x), n(cy - 3), n(w), s.track, s.knob_stroke)
        o += '<rect x="%s" y="%s" width="%s" height="6" rx="3" fill="%s"/>' % (n(x), n(cy - 3), n(w * val), col)
        cx = x + w * val
        o += s.shape(cx - 9, y + 1, 18, h - 2, s.knob_fill, col, 2, 4)
        o += '<path d="M%s,%s v%s" stroke="%s" stroke-width="3"/>' % (n(cx), n(y + 5), n(h - 10), col)
        return o

    def meter(s, x, y, w, h, val, horiz=False, segs=14):
        o = ''
        for k in range(segs):
            f = k / (segs - 1)
            c = '#35d36b' if f < .6 else ('#f2d13a' if f < .85 else '#ff4545')
            lit = f <= val
            if horiz:
                sw = w / segs
                o += '<rect x="%s" y="%s" width="%s" height="%s" rx="1" fill="%s"/>' % (n(x + k * sw + 1), n(y), n(sw - 2), n(h), c if lit else s.meter_off)
            else:
                sh = h / segs
                o += '<rect x="%s" y="%s" width="%s" height="%s" rx="1" fill="%s"/>' % (n(x), n(y + h - (k + 1) * sh + 1), n(w), n(sh - 2), c if lit else s.meter_off)
        return o

    def jog(s, cx, cy, r, ang=40, col=None):
        col = col or s.accent
        o = '<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="3"/>' % (n(cx), n(cy), n(r), s.knob_fill, col)
        o += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="2" stroke-opacity=".5"/>' % (n(cx), n(cy), n(r * .78), s.knob_stroke)
        t = ''.join('M%s,%s L%s,%s' % (n(cx + r * .86 * math.cos(math.radians(a))), n(cy + r * .86 * math.sin(math.radians(a))),
                                       n(cx + r * .95 * math.cos(math.radians(a))), n(cy + r * .95 * math.sin(math.radians(a)))) for a in range(0, 360, 10))
        o += '<path d="%s" stroke="%s" stroke-opacity=".7"/>' % (t, s.dim)
        mx, my = cx + r * .87 * math.cos(math.radians(ang)), cy + r * .87 * math.sin(math.radians(ang))
        o += '<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (n(mx), n(my), n(max(6, r * .06)), col)
        return o


# ---------------------------------------------------------------- blocks (return svg, w, h)
def b_transport(T, d, col, orient='h'):
    D = DECK[d]
    if orient == 'h':
        o = T.btn(0, 0, 70, 44, on=(d == 'A'), col=col, icon='play') + T.btn(76, 0, 62, 44, 'CUE', col=col, fs=13) + \
            T.btn(144, 0, 62, 44, 'SYNC', on=(d == 'B'), col=col, fs=13)
        return o, 206, 44
    o = T.btn(0, 0, 66, 40, on=(d == 'A'), col=col, icon='play') + T.btn(0, 46, 66, 40, 'CUE', col=col, fs=13) + \
        T.btn(0, 92, 66, 40, 'SYNC', on=(d == 'B'), col=col, fs=13)
    return o, 66, 132


def b_toggles(T, d, col, orient='row'):
    names = ['KEYLOCK', 'QUANT', 'SLIP', 'REV']
    on = [d == 'A', True, False, d == 'B']
    bw, bh = 68, 30
    o = ''
    for i, nm in enumerate(names):
        if orient == 'row':
            x, y = i * (bw + 6), 0
        elif orient == 'grid':
            x, y = (i % 2) * (bw + 6), (i // 2) * (bh + 6)
        else:
            x, y = 0, i * (bh + 6)
        o += T.btn(x, y, bw, bh, nm, on[i], col, 11)
    W = {'row': 4 * bw + 18, 'grid': 2 * bw + 6, 'col': bw}[orient]
    H = {'row': bh, 'grid': 2 * bh + 6, 'col': 4 * bh + 18}[orient]
    return o, W, H


def b_keyshift(T, d, col):
    D = DECK[d]
    o = T.btn(0, 0, 32, 30, '−', col=col, fs=16) + T.text(66, 20, 'KEY ' + D['shift'], 13, T.ink, weight='700') + T.btn(100, 0, 32, 30, '+', col=col, fs=16)
    return o, 132, 30


def b_loop(T, d, col, orient='row'):
    D = DECK[d]
    if orient == 'row':
        o = T.btn(0, 0, 38, 30, 'IN', col=col) + T.btn(42, 0, 44, 30, 'OUT', on=True, col=col) + T.btn(92, 0, 30, 30, '◂', col=col) + \
            T.btn(126, 0, 34, 30, D['loop'], col=col, fs=14) + T.btn(164, 0, 30, 30, '▸', col=col)
        return o, 194, 30
    o = T.btn(0, 0, 62, 30, 'LOOP IN', col=col) + T.btn(66, 0, 66, 30, 'LOOP OUT', on=True, col=col) + T.btn(0, 36, 30, 30, '◂', col=col) + \
        T.btn(34, 36, 64, 30, D['loop'] + ' BEATS', col=col) + T.btn(102, 36, 30, 30, '▸', col=col)
    return o, 132, 66


def b_fx(T, d, col, orient='rows'):
    D = DECK[d]
    o = ''
    if orient == 'rows':
        for i in range(3):
            y = i * 34
            o += T.btn(0, y, 76, 28, D['fxn'][i], False, col, 11) + T.btn(80, y, 34, 28, 'ON', i == 0 or (d == 'B' and i == 2), col, 11)
            o += T.fader_h(120, y, 76, 28, [.7, .4, .55][i], col)
        return o, 196, 96
    for i in range(3):
        x = i * 82
        o += T.btn(x, 0, 76, 28, D['fxn'][i], False, col, 11) + T.btn(x, 34, 76, 28, 'ON', i == 0, col, 11)
        o += T.fader_h(x, 68, 76, 28, [.7, .4, .55][i], col)
    return o, 240, 96


def b_tempo(T, d, col, h=170):
    o = T.btn(0, 0, 56, 28, '±8%', col=col, fs=12) + T.fader_v(8, 36, 40, h, 0.5 + (0.04 if d == 'A' else -0.03), col)
    o += T.text(28, h + 52, '+0.0%' if d == 'A' else '-1.5%', 12, T.ink, weight='700', mono=True)
    return o, 56, h + 60


def b_info(T, d, col, w=320):
    D = DECK[d]
    o = T.text(0, 18, D['title'], 20, T.ink, 'start', '700') + T.text(0, 38, D['artist'], 14, T.dim, 'start')
    o += T.text(0, 74, D['bpm'], 30, col, 'start', '700', mono=True) + T.text(104, 52, 'BPM', 11, T.dim, 'start', '600')
    o += T.text(104, 74, 'KEY ' + D['key'], 16, T.ink, 'start', '700') + T.text(w, 56, D['t'], 17, T.ink, 'end', '700', mono=True)
    o += T.text(w, 76, D['rem'], 15, T.dim, 'end', '600', mono=True)
    return o, w, 84


def b_overview(T, d, col, w=300, h=34):
    D = DECK[d]
    o = '<rect width="%s" height="%s" rx="4" fill="%s" stroke="%s"/>' % (n(w), n(h), T.wave_bg, T.knob_stroke)
    o += '<clipPath id="ov%s%d"><rect width="%s" height="%s" rx="4"/></clipPath><g clip-path="url(#ov%s%d)">' % (d, int(w), n(w), n(h), d, int(w))
    o += wave(0, 3, w, h - 6, D['seed'] + 5, col, T.core, head=w * D['head'], cues=D['cues'], pitch=3, grid=False, labels=False) + '</g>'
    return o, w, h


def b_padmodes(T, d, col, orient='row'):
    names, o = ['CUE', 'ROLL', 'SLICE', 'SMPL'], ''
    for i, nm in enumerate(names):
        x, y = (i * 50, 0) if orient == 'row' else ((i % 2) * 50, (i // 2) * 32)
        o += T.btn(x, y, 46, 28, nm, i == 0, col, 11)
    return (o, 194, 28) if orient == 'row' else (o, 96, 60)


def b_pads(T, d, col, s=48, g=6, cols=4):
    D = DECK[d]
    rows = 8 // cols
    o = ''
    for i in range(8):
        o += T.pad((i % cols) * (s + g), (i // cols) * (s + g), s, s, i, i in D['lit'])
    return o, cols * (s + g) - g, rows * (s + g) - g


def b_xfader(T, col='#fff', w=420, h=40):
    o = '<rect x="0" y="%s" width="%s" height="8" rx="4" fill="%s" stroke="%s"/>' % (n(h / 2 - 4), n(w), T.track, T.knob_stroke)
    o += T.text(0, h + 14, 'A', 13, T.colA, 'start', '700') + T.text(w, h + 14, 'B', 13, T.colB, 'end', '700') + T.text(w / 2, h + 14, 'CROSSFADER', 11, T.dim, weight='600')
    cx = w * 0.46
    o += T.shape(cx - 16, 2, 32, h - 4, T.knob_fill, T.ink, 2, 5) + '<path d="M%s,6 v%s" stroke="%s" stroke-width="3"/>' % (n(cx), n(h - 12), T.ink)
    return o, w, h + 20


def b_master(T, w=330):
    o = T.knob(28, 30, 22, .72, 'MASTER', T.accent) + T.knob(96, 30, 18, .5, 'HP LEVEL', T.accent) + T.knob(158, 30, 18, .35, 'HP MIX', T.accent)
    o += T.meter(208, 0, 14, 76, .78) + T.meter(226, 0, 14, 76, .72) + T.text(224, 92, 'L  R', 11, T.dim, weight='600')
    return o, 244, 100


def b_mic(T):
    o = T.btn(0, 0, 54, 30, 'MIC', True, T.accent) + T.btn(60, 0, 66, 30, 'TALK', False, T.accent) + T.knob(158, 15, 15, .55, 'LEVEL', T.accent)
    return o, 180, 48


def b_sampler(T, cols=4, s=46, g=6, rows=2, n_pads=8):
    o = ''
    for i in range(n_pads):
        o += T.pad((i % cols) * (s + g), (i // cols) * (s + g), s, s, i, i in (1, 4, 6), label='S%d' % (i + 1))
    w = cols * (s + g) - g
    h = rows * (s + g) - g
    return o, w, h


def b_rec(T, w=78):
    o = T.btn(0, 0, w, 32, 'REC', False, '#ff3b3b', 13)
    o += '<circle cx="14" cy="16" r="5" fill="#ff3b3b"/>'
    return o, w, 32


def b_appbtns(T, orient='row'):
    names = ['⚙ SET', 'SKIN', 'EN']
    o = ''
    for i, nm in enumerate(names):
        x, y = (i * 62, 0) if orient == 'row' else (0, i * 36)
        o += T.btn(x, y, 56, 30, nm, False, T.accent, 11)
    return (o, 180, 30) if orient == 'row' else (o, 56, 102)


def b_status(T, w=610):
    o = '<rect x="-14" y="-6" width="%d" height="30" rx="15" fill="#0b0d12" fill-opacity=".82" stroke="#ffffff" stroke-opacity=".25"/>' % (w + 28)
    o += '<text x="0" y="14" font-size="12" fill="#c8ccd6" font-family=\'%s\'>READY  \u00b7  2 decks  \u00b7  audio: built-in out  \u00b7  controller: FLX-class (HID)  \u00b7  CPU 7%%</text>' % MONO
    return o, w, 24


def grip(x, y, col='#ffffff', op=.8):
    return ''.join('<circle cx="%s" cy="%s" r="3" fill="%s" fill-opacity="%s"/>' % (n(x + k * 10), n(y), col, op) for k in (-1, 0, 1)) + \
        '<rect x="%s" y="%s" width="44" height="14" rx="7" fill="none" stroke="%s" stroke-opacity="%s"/>' % (n(x - 22), n(y - 7), col, op * .6)


def b_strip(T, ch, col, h_fader=130, sp=54):
    """vertical channel strip: trim, 3 eq, filter, fader+meter, cue.  returns w=96"""
    o = ''
    lab = ['TRIM', 'HIGH', 'MID', 'LOW', 'FILTER']
    vals = [.5, .62, .45, .6, .5]
    for i in range(5):
        o += T.knob(48, 18 + i * sp, 18, vals[i], lab[i], col, bipolar=i > 0)
    y = 5 * sp + 4
    o += T.fader_v(10, y, 36, h_fader, .8 if ch == 1 else .7, col)
    o += T.meter(58, y, 12, h_fader, .75 if ch == 1 else .62) + T.meter(74, y, 12, h_fader, .7 if ch == 1 else .58)
    o += T.btn(24, y + h_fader + 22, 48, 30, 'CUE', ch == 2, col, 12)
    return o, 96, y + h_fader + 56


def b_browser(T, col, w=420, h=420, title='BROWSER', nrows=8):
    rows = [('Midnight Ossuary', 'The Marrow Kings', '128.0', '8A'), ('Bone China Heart', 'Calcium Sisters', '126.0', '9A'),
            ('Tibia Tides', 'Femur Fatale', '124.0', '8A'), ('Marrow Lane', 'Skeleton Crew', '130.0', '7A'),
            ('Last Rib Standing', 'Coccyx', '127.0', '9A'), ('Ossicle', 'Stirrup & Anvil', '122.0', '10A'),
            ('Dust to Dust Floor', 'Catacomb Club', '125.0', '8B'), ('Patella Rave', 'Hinge Joint', '132.0', '6A')]
    o = '<rect width="%s" height="%s" rx="10" fill="%s" stroke="%s" stroke-width="2"/>' % (n(w), n(h), T.wave_bg, T.knob_stroke)
    o += T.text(14, 24, title, 13, T.dim, 'start', '700') + T.btn(w - 190, 8, 176, 28, '⌕ search tracks…', False, T.accent, 11) + grip(w - 224, 22, T.dim)
    tw = 120
    o += '<path d="M%s,44 V%s" stroke="%s" stroke-opacity=".6"/>' % (n(tw + 6), n(h - 38), T.knob_stroke)
    for i, t in enumerate(['▾ Collection', '   Recent', '▾ Playlists', '   Warm-up', '   Peak hour', '   After dark', '   Crates'][:max(4, nrows + 1)]):
        o += T.text(14, 66 + i * 26, t, 12, T.ink if i in (1, 4) else T.dim, 'start', '600' if i in (1, 4) else 'normal')
    ry = 52
    o += T.text(tw + 16, ry + 8, 'TITLE', 11, T.dim, 'start', '700') + T.text(w - 112, ry + 8, 'BPM', 11, T.dim, 'start', '700') + T.text(w - 52, ry + 8, 'KEY', 11, T.dim, 'start', '700')
    for i, r in enumerate(rows[:nrows]):
        y = ry + 20 + i * 30
        if i == 1:
            o += '<rect x="%s" y="%s" width="%s" height="28" rx="4" fill="%s" fill-opacity=".25"/>' % (n(tw + 12), n(y), n(w - tw - 22), col)
        o += T.text(tw + 16, y + 19, r[0], 12, T.ink, 'start', '600') + T.text(w - 112, y + 19, r[2], 12, T.ink, 'start', mono=True) + T.text(w - 52, y + 19, r[3], 12, T.ink, 'start', mono=True)
    by = h - 36
    o += T.btn(14, by, 74, 28, '+ PLAYLIST', False, T.accent, 11) + T.btn(94, by, 56, 28, 'LOAD A', False, T.colA, 11) + T.btn(156, by, 56, 28, 'LOAD B', False, T.colB, 11)
    o += T.btn(218, by, 58, 28, 'COMPAT', True, T.accent, 11) + T.text(w - 14, by + 19, '8 of 1,284 tracks', 11, T.dim, 'end')
    return o, w, h


def tooth(x, y, w, h, up=True, r=None):
    r = r if r is not None else w * 0.3
    if up:
        return 'M%s,%s h%s v%s q0,%s -%s,%s h-%s q-%s,0 -%s,-%s z' % (n(x), n(y), n(w), n(h - r), n(r), n(r), n(r), n(w - 2 * r), n(r), n(r), n(r))
    return 'M%s,%s h%s v%s h-%s v-%s q0,-%s %s,-%s z' % (n(x + 0), n(y + h), n(w), n(-(h - r)), n(w), n(-0), n(0), n(0), n(0)) if False else \
        'M%s,%s h%s v-%s q0,-%s -%s,-%s h-%s q-%s,0 -%s,%s z' % (n(x), n(y + h), n(w), n(h - r), n(r), n(r), n(r), n(w - 2 * r), n(r), n(r), n(r))


def b_fxslot(T, d, col, i):
    D = DECK[d]
    o = T.btn(0, 0, 76, 28, D['fxn'][i], False, col, 11) + T.btn(80, 0, 34, 28, 'ON', i == 0 or (d == 'B' and i == 2), col, 11)
    o += T.fader_h(120, 0, 76, 28, [.7, .4, .55][i], col)
    return o, 196, 28


def b_strip_h(T, ch, col, kr=17, sp=62):
    lab = ['TRIM', 'HIGH', 'MID', 'LOW', 'FILTER']
    vals = [.5, .62, .45, .6, .5]
    o = ''
    for i in range(5):
        o += T.knob(kr + 4 + i * sp, kr + 2, kr, vals[i], lab[i], col, bipolar=i > 0)
    x = 5 * sp + 10
    o += T.fader_h(x, 6, 120, 28, .8 if ch == 1 else .68, col) + T.text(x + 60, 52, 'CH %d' % ch, 11, T.dim, weight='700')
    o += T.meter(x + 134, 10, 100, 9, .75 if ch == 1 else .6, True, 12) + T.meter(x + 134, 24, 100, 9, .7 if ch == 1 else .55, True, 12)
    o += T.btn(x + 244, 3, 46, 30, 'CUE', ch == 2, col, 12)
    return o, x + 290, 58


def both(D, T, name, fn, x, y, poly=None, ca=None, cb=None, **kw):
    for d, col in (('A', ca or T.colA), ('B', cb or T.colB)):
        D.put(name + ' ' + d, fn(T, d, col, **kw), x, y, mirror=(d == 'B'), poly=poly)


def tooth_btn(T, x, y, w, h, txt, on, col, up=True, fs=12, fill='#fbf6ea', stroke='#8f836a', ink='#2b261c', onink='#111'):
    o = '<path d="%s" fill="%s" stroke="%s" stroke-width="2.5"/>' % (tooth(x, y, w, h, up), col if on else fill, stroke)
    fs = max(11, fs)
    o += T.text(x + w / 2, y + h / 2 + (2 if up else 6) + fs * .2, txt, fs, onink if on else ink, weight='700', halo='')
    return o


def flower(cx, cy, r, petals, col, col2=None, rot=0, pr=None):
    o = ''
    pr = pr or r * 0.42
    for k in range(petals):
        a = rot + 360.0 * k / petals
        px, py = cx + (r - pr) * math.cos(math.radians(a)), cy + (r - pr) * math.sin(math.radians(a))
        o += '<ellipse cx="%s" cy="%s" rx="%s" ry="%s" transform="rotate(%s %s %s)" fill="%s" stroke="%s" stroke-width="1.5"/>' % (
            n(px), n(py), n(pr), n(pr * 0.62), n(a), n(px), n(py), col, col2 or '#00000033')
    return o
