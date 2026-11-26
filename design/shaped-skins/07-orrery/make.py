import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1940
D = Doc(W, H)
CX, CY = 800, 700
CA, CB, BR = '#6fd8ff', '#ffbe55', '#d9aa4a'
class Orr(Theme):
    ink = '#f6ecd2'; dim = '#e0cfa0'; btn_fill = '#1a1426'; btn_stroke = BR; btn_text = '#f6ecd2'; accent = CA; on_text = '#08141c'
    colA = CA; colB = CB; knob_fill = '#271c3c'; knob_stroke = BR; track = '#08060e'; wave_bg = '#07060e'; halo = '#0c0818'
T = Orr()
D.d('<radialGradient id="dome" cx="50%" cy="40%" r="70%"><stop offset="0" stop-color="#2a2150"/><stop offset="1" stop-color="#0a0818"/></radialGradient>')
D.d('<linearGradient id="br" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#f6e0a0"/><stop offset=".5" stop-color="#c4973a"/><stop offset="1" stop-color="#7a5818"/></linearGradient>')
# outer ring + dome
D.add('<circle cx="%d" cy="%d" r="652" fill="url(#br)" stroke="#4a3410" stroke-width="5"/><circle cx="%d" cy="%d" r="556" fill="url(#dome)" stroke="#4a3410" stroke-width="5"/>' % (CX, CY, CX, CY))
for k in range(120):
    a = k * 3
    r1, r2 = (640, 652) if k % 5 else (630, 652)
    p, q = polar(CX, CY, r1, a), polar(CX, CY, r2, a)
    D.add('<path d="M%s,%s L%s,%s" stroke="#4a3410" stroke-width="2"/>' % (n(p[0]), n(p[1]), n(q[0]), n(q[1])))
# stars
for k in range(40):
    a = k * 47; r = 120 + (k * 61) % 400
    p = polar(CX, CY, r, a)
    D.add('<circle cx="%s" cy="%s" r="%s" fill="#fff" fill-opacity=".35"/>' % (n(p[0]), n(p[1]), 1 + k % 3))
# pads along the ring: A left, B right (rotated to follow the ring)
for side, d in ((0, 'A'), (1, 'B')):
    for i in range(8):
        a = (180 - (i - 3.5) * 8.2) if side == 0 else ((i - 3.5) * 8.2)
        x, y = polar(CX, CY, 604, a)
        D.add(rot(T.pad(x - 24, y - 24, 48, 48, i, i in DECK[d]['lit']), x, y, a + 90))
D.names['pads A'] = (150, 450, 100, 500); D.names['pads B'] = (1350, 450, 100, 500)
# planets (decks)
PR = 165
for cx, d, col in ((470, 'A', CA), (1130, 'B', CB)):
    cy = 700
    D.add('<circle cx="%d" cy="%d" r="%d" fill="url(#br)" stroke="#4a3410" stroke-width="4"/><circle cx="%d" cy="%d" r="%d" fill="none" stroke="#14101f" stroke-width="40"/>' % (cx, cy, PR + 8, cx, cy, PR - 22))
    D.add('<path d="%s" stroke="%s" stroke-width="9" fill="none" stroke-linecap="round"/>' % (arc(cx, cy, PR - 22, 110, 300 if d == 'A' else 250), col))
    ma = 235 if d == 'A' else 305
    p = polar(cx, cy, PR - 22, ma)
    D.add('<circle cx="%s" cy="%s" r="12" fill="%s" stroke="#fff" stroke-width="3"/><circle cx="%d" cy="%d" r="%d" fill="#05040b"/><clipPath id="p%s"><circle cx="%d" cy="%d" r="%d"/></clipPath>' % (n(p[0]), n(p[1]), col, cx, cy, PR - 44, d, cx, cy, PR - 45))
    Dd = DECK[d]
    D.add('<g clip-path="url(#p%s)">' % d + wave(cx - 120, cy - 40, 240, 80, Dd['seed'], col, '#fff', head=120, cues=Dd['cues'][:3], pitch=4) + '</g>')
    D.add(T.text(cx, cy - 66, Dd['title'], 14, '#fff', weight='700', halo='') + T.text(cx - 40, cy + 76, Dd['bpm'], 24, col, weight='800', mono=True, halo='') + T.text(cx + 52, cy + 72, Dd['t'], 13, '#e0cfa0', mono=True, halo='') + T.text(cx + 52, cy + 90, Dd['rem'], 11, '#a99c78', mono=True, halo=''))
    D.put('jog ' + d, ('', 2 * PR, 2 * PR), cx - PR, cy - PR)
D.names['wave'] = (350, 660, 240, 80)
# EQ knobs orbit over each planet; channel fader is an arc slider on the sun side
for cx, d, col, s in ((470, 'A', CA, 1), (1130, 'B', CB, -1)):
    cy = 700
    for i, (lab, v) in enumerate(zip(['TRIM', 'HIGH', 'MID', 'LOW', 'FILT'], [.5, .62, .45, .6, .5])):
        a = (222 + i * 24) if s == 1 else (318 - i * 24)
        x, y = polar(cx, cy, 228, a)
        D.add(T.knob(x, y, 18, v, None, col, bipolar=i > 0))
        lx, ly = polar(cx, cy, 262, a)
        D.add(T.text(lx, ly + 4, lab, 11, T.dim, weight='700'))
    pa = arc_bez(cx, cy, 222, -52, 52) if s == 1 else arc_bez(cx, cy, 222, 232, 128)
    D.add(curve_fader(T, pa, .75 if d == 'A' else .65, col, w=12, cap=(40, 28)))
    D.add(T.meter(cx + s * 258 - 6, cy - 55, 12, 110, .75 if d == 'A' else .6))
    e = polar(cx, cy, 222, 60 if s == 1 else 120)
    D.add(T.btn(e[0] - 24 + s * 18, e[1] + 8, 48, 30, 'CUE', d == 'B', col, 12))
D.names['ch1'] = (440, 470, 300, 260); D.names['ch2'] = (860, 470, 300, 260)
# sun: master + arc meter
D.add('<circle cx="%d" cy="%d" r="92" fill="url(#br)" stroke="#4a3410" stroke-width="4"/><circle cx="%d" cy="%d" r="70" fill="#1c1430" stroke="#4a3410" stroke-width="2"/>' % (CX, CY, CX, CY))
for k in range(20):
    a = 150 + k * 12
    p, q = polar(CX, CY, 76, a), polar(CX, CY, 86, a)
    c = '#35d36b' if k < 12 else ('#f2d13a' if k < 17 else '#ff4545')
    D.add('<path d="M%s,%s L%s,%s" stroke="%s" stroke-width="6" stroke-linecap="round" opacity="%s"/>' % (n(p[0]), n(p[1]), n(q[0]), n(q[1]), c, '1' if k < 15 else '.25'))
D.add(T.knob(CX, CY, 38, .72, None, '#fff') + T.text(CX, CY + 66 - 18, '', 1))
D.add(T.text(CX, CY + 6, 'MASTER', 11, '#fff', weight='700', halo=''))
D.add(T.knob(CX, CY - 150, 18, .5, 'HP LVL', '#fff') + T.knob(CX, CY + 150, 18, .35, 'HP MIX', '#fff'))
D.names['master'] = (700, 550, 200, 300)
# crossfader along the lower orbit
D.add(curve_fader(T, [(560, 930), (700, 1020), (900, 1020), (1040, 930)], .46, '#fff', w=14, cap=(40, 32)))
D.add(T.text(540, 944, 'A', 15, CA, weight='800', halo='') + T.text(1060, 944, 'B', 15, CB, weight='800', halo='') + T.text(800, 1066, 'CROSSFADER', 11, T.dim, weight='700'))
D.names['xfader'] = (560, 930, 480, 120)
# deck controls in the lower dome
both(D, T, 'transport', b_transport, 300, 960)
both(D, T, 'loop', b_loop, 310, 1020)
both(D, T, 'tempo', b_tempo, 232, 360, h=150) if False else None
for cx, d, col, a0, a1 in ((470, 'A', CA, 128, 200), (1130, 'B', CB, 52, -20)):
    D.add(curve_fader(T, arc_bez(cx, 700, 222, a0, a1), .5, col, w=12, cap=(40, 28)))
    e = polar(cx, 700, 222, a0 - (14 if d == 'A' else -14))
    D.add(T.btn(e[0] - 28 + (-30 if d == 'A' else 30), e[1] + 6, 56, 30, '\u00b18%', False, col, 12) + T.text(e[0] + (-30 if d == 'A' else 30), e[1] + 52, '+0.0%' if d == 'A' else '-1.5%', 12, '#fff', mono=True, weight='700'))
D.names['tempo A'] = (260, 620, 120, 270); D.names['tempo B'] = (1220, 620, 120, 270)
D.put('mic', b_mic(T), 710, 1110)
D.put('rec', b_rec(T), 761, 1170)
# floating scroll browser
D.add('<rect x="130" y="1510" width="900" height="400" rx="14" fill="#e8d6a2" stroke="#7a5818" stroke-width="4"/><rect x="100" y="1490" width="40" height="440" rx="20" fill="url(#br)" stroke="#4a3410" stroke-width="3"/><rect x="1020" y="1490" width="40" height="440" rx="20" fill="url(#br)" stroke="#4a3410" stroke-width="3"/>')
D.add(grip(580, 1518, '#4a3410'))
D.put('browser', b_browser(T, CA, 840, 360, 'SCROLL  ·  TRACK BROWSER', nrows=6), 160, 1530)
# floating hex sampler
hx = [(1330 + 230 * math.cos(math.radians(60 * k + 30)), 1700 + 230 * math.sin(math.radians(60 * k + 30))) for k in range(6)]
D.add('<polygon points="%s" fill="url(#br)" stroke="#4a3410" stroke-width="5"/><polygon points="%s" fill="#1a1426" stroke="#4a3410" stroke-width="3"/>' % (' '.join('%s,%s' % (n(a), n(b)) for a, b in hx), ' '.join('%s,%s' % (n(1330 + (a - 1330) * .88), n(1700 + (b - 1700) * .88)) for a, b in hx)))
D.add(grip(1330, 1520, '#4a3410'))
D.put('sampler', b_sampler(T), 1229, 1640)
D.put('appbtns', b_appbtns(T), 1240, 1770)
D.put('status', b_status(T), 30, 1906)
D.save(os.path.dirname(__file__))
