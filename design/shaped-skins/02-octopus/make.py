import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1690
D = Doc(W, H)
CA, CB = '#41e3ff', '#ff6bd6'
class Abyss(Theme):
    ink = '#f0eaff'; dim = '#cdbfee'; btn_fill = '#1a1030'; btn_stroke = '#bda0ff'; btn_text = '#f0eaff'
    accent = CA; colA = CA; colB = CB; knob_fill = '#241446'; knob_stroke = '#bda0ff'; track = '#0b0618'; wave_bg = '#070412'; on_text = '#06101a'
    halo = '#1a0c33'
T = Abyss()
D.d('<radialGradient id="mant" cx="50%" cy="30%" r="80%"><stop offset="0" stop-color="#9b63e8"/><stop offset=".6" stop-color="#5a2fa0"/><stop offset="1" stop-color="#2b1458"/></radialGradient>')
D.d('<linearGradient id="tent" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#6a3cb8"/><stop offset="1" stop-color="#3a1c78"/></linearGradient>')
TP = [[(330, 600), (220, 780), (110, 900), (170, 1170)], [(450, 640), (390, 820), (300, 960), (390, 1200)],
      [(570, 660), (550, 840), (500, 980), (580, 1210)], [(690, 670), (690, 830), (640, 960), (720, 1190)]]
paths = TP + [[(1600 - x, y) for x, y in p] for p in TP]
def taper(p, w0, w1, steps=18):
    o = ''
    for i in range(steps):
        t0, t1 = i / steps, (i + 1) / steps
        a, b = bez(p, t0), bez(p, t1)
        w = w0 + (w1 - w0) * t0
        o += '<path d="M%s,%s L%s,%s" stroke="url(#tent)" stroke-width="%s" stroke-linecap="round"/>' % (n(a[0]), n(a[1]), n(b[0]), n(b[1]), n(w))
    return o
for p in paths:
    D.add('<g>' + taper(p, 84, 34) + '</g>')
    for k in range(10):
        x, y = bez(p, .18 + k * .08)
        D.add('<circle cx="%s" cy="%s" r="%s" fill="#c9a8ff" fill-opacity=".35"/>' % (n(x + 20 - k), n(y), n(7 - k * .4)))
# mantle + body
left = [(800, 30), (560, 50), (380, 150), (300, 300), (320, 450), (420, 560), (560, 628), (800, 650)]
pts = sym(left, W)
D.add('<path d="%s" fill="url(#mant)" stroke="#d8c2ff" stroke-width="4"/>' % smooth(pts))
for k in range(12):
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#e8d6ff" fill-opacity=".16"/>' % (560 + (k * 97) % 480, 80 + (k * 53) % 40, 6 + k % 4))
# eyes (jog) and brow waveform
EY, R = 410, 150
D.add('<rect x="420" y="108" width="760" height="146" rx="30" fill="#070412" stroke="#d8c2ff" stroke-width="4"/>')
D.add('<clipPath id="bw"><rect x="426" y="112" width="748" height="138" rx="26"/></clipPath><g clip-path="url(#bw)">' + wave(426, 116, 748, 62, 11, CA, '#fff', head=374, cues=[.08, .2, .31, .62, .8], pitch=4) + wave(426, 182, 748, 62, 29, CB, '#fff', head=374, cues=[.1, .27, .5, .7], pitch=4) + '</g>')
D.names['wave'] = (426, 116, 748, 128)
for cx, d, col in ((560, 'A', CA), (1040, 'B', CB)):
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#e8d6ff" stroke="#fff" stroke-width="3"/><circle cx="%d" cy="%d" r="%d" fill="#15092e"/>' % (cx, EY + 50, R, cx, EY + 50, R - 10))
    D.add('<path d="%s" stroke="%s" stroke-width="12" fill="none" stroke-linecap="round"/>' % (arc(cx, EY + 50, R - 24, 110, 300 if d == 'A' else 250), col))
    ma = 235 if d == 'A' else 305
    D.add('<circle cx="%s" cy="%s" r="12" fill="%s" stroke="#fff" stroke-width="3"/>' % (n(cx + (R - 24) * math.cos(math.radians(ma))), n(EY + 50 + (R - 24) * math.sin(math.radians(ma))), col))
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#07030f" stroke="%s" stroke-width="2"/>' % (cx, EY + 50, R - 44, col))
    Dd = DECK[d]
    D.add(T.text(cx, EY + 22, Dd['title'], 17, '#fff', weight='700', halo='') + T.text(cx, EY + 70, Dd['bpm'], 34, col, weight='800', mono=True, halo='') + T.text(cx, EY + 96, 'KEY ' + Dd['key'] + '   ' + Dd['t'], 13, '#cdbfee', mono=True, halo='') + T.text(cx, EY + 114, Dd['rem'], 12, '#9d8fc0', mono=True, halo=''))
    D.put('jog ' + d, ('', 2 * R, 2 * R), cx - R, EY + 50 - R)
# body controls
both(D, T, 'transport', b_transport, 320, 565, orient='v') if False else None
both(D, T, 'pads', b_pads, 345, 590 - 0, s=48)
both(D, T, 'transport', b_transport, 575, 575, orient='v') if False else None
D.add(T.knob(800, 470, 18, .72, 'MASTER', '#fff') + T.knob(800, 540, 15, .5, 'HP LVL', '#fff') + T.knob(800, 604, 15, .35, 'HP MIX', '#fff'))
D.add(T.meter(840, 440, 12, 150, .8) + T.meter(860, 440, 12, 150, .74))
D.names['master'] = (770, 450, 120, 170)
# tentacles: tempo (T1/T8), EQ (T2/T7), fader (T3/T6), loop (T4/T5)
for side, d, col in ((0, 'A', CA), (1, 'B', CB)):
    P = paths[0 + 4 * side] if side == 0 else paths[4]
    pT, pE, pF, pL = (paths[0], paths[1], paths[2], paths[3]) if side == 0 else (paths[7], paths[6], paths[5], paths[4])
    D.add(curve_fader(T, pT, .5 + (.04 if d == 'A' else -.03), col, w=14, cap=(46, 30)))
    x, y = bez(pT, 0.0)
    D.add(T.btn(x - 28 + (-70 if side == 0 else 70), y - 10, 56, 30, '±8%', False, col, 12))
    D.add(T.text(bez(pT, 1)[0], bez(pT, 1)[1] + 36, '+0.0%' if d == 'A' else '-1.5%', 12, '#fff', mono=True, weight='700'))
    for i, (lab, v) in enumerate(zip(['TRIM', 'HIGH', 'MID', 'LOW', 'FILT'], [.5, .62, .45, .6, .5])):
        x, y = bez(pE, .30 + i * .15)
        D.add(T.knob(x, y, 18, v, None, col, bipolar=i > 0) + T.text(x + (34 if side else -34), y + 5, lab, 11, T.dim, weight='700'))
    D.add(curve_fader(T, pF, .78 if d == 'A' else .66, col, w=14, cap=(46, 30)))
    x, y = bez(pF, .93)
    D.add(T.btn(x + (20 if side == 0 else -68), y - 15, 48, 30, 'CUE', d == 'B', col, 12))
    x, y = bez(pF, .5)
    D.add(T.meter(x + (30 if side == 0 else -42), y - 50, 12, 100, .75 if d == 'A' else .6))
    for i, lab in enumerate(['IN', 'OUT', '◂', DECK[d]['loop'], '▸']):
        x, y = bez(pL, .30 + i * .14)
        D.add(T.btn(x - 19, y - 15, 38, 30, lab, i == 1, col, 12))
    D.names['tempo ' + d] = (bez(pT, .3)[0] - 25, bez(pT, .3)[1] - 15, 50, 200)
    D.names['loop ' + d] = (bez(pL, .1)[0] - 20, bez(pL, .1)[1], 40, 300)
D.names['ch1'] = (bez(paths[1], .1)[0] - 20, bez(paths[1], .1)[1] - 20, 40, 300); D.names['ch2'] = (bez(paths[6], .1)[0] - 20, bez(paths[6], .1)[1] - 20, 40, 300)
both(D, T, 'transport', b_transport, 560, 640, orient='v') if False else None
# crossfader along the beak smile
D.add(curve_fader(T, [(660, 700), (760, 770), (840, 770), (940, 700)], .46, '#fff', w=14, cap=(40, 32)))
D.add(T.text(640, 700, 'A', 14, CA, weight='800', halo='') + T.text(960, 700, 'B', 14, CB, weight='800', halo='') + T.text(800, 810, 'CROSSFADER', 11, T.dim, weight='700'))
D.names['xfader'] = (660, 690, 280, 90)
# transport under eyes (small shelf)
both(D, T, 'transport', b_transport, 330, 700)
# floating clam browser
D.add('<path d="M120,1350 Q120,1300 200,1300 H900 Q980,1300 980,1350 V1610 Q980,1650 900,1650 H200 Q120,1650 120,1610Z" fill="#d8b8c8" stroke="#8a5a78" stroke-width="5"/>')
for k in range(9):
    D.add('<path d="M550,1300 L%d,1300" stroke="#b88aa6" stroke-width="3" opacity=".6"/>' % (150 + k * 100))
D.add(grip(550, 1316, '#6a3a58'))
D.put('browser', b_browser(T, CA, 800, 290, 'CLAM  ·  TRACK BROWSER', nrows=5), 150, 1340)
# floating bubbles: sampler + app
for (bx, by, br) in ((1180, 1400, 130), (1380, 1520, 100), (1350, 1360, 60)):
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#41e3ff" fill-opacity=".16" stroke="#bdf4ff" stroke-width="3"/>' % (bx, by, br))
D.add(grip(1180, 1298, '#bdf4ff'))
D.put('sampler', b_sampler(T), 1080, 1350)
D.put('appbtns', b_appbtns(T), 1330, 1490)
D.put('rec', b_rec(T), 1230, 1420) if False else None
D.put('mic', b_mic(T), 1030, 1500) if False else None
D.put('status', b_status(T), 30, 1590) if False else None
D.save(os.path.dirname(__file__))
