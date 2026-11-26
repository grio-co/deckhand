import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1500, 2000
D = Doc(W, H)
CA, CB, RED, YEL = '#4fe0d0', '#ff9a52', '#d8453a', '#ffe08a'
class Lh(Theme):
    ink = '#f4f6fa'; dim = '#b8c4d8'; btn_fill = '#141c2c'; btn_stroke = '#aebbd4'; btn_text = '#f4f6fa'; accent = CA; on_text = '#021418'
    colA = CA; colB = CB; knob_fill = '#1c2840'; knob_stroke = '#aebbd4'; track = '#060a14'; wave_bg = '#050810'; halo = '#0a1020'
T = Lh()
D.d('<linearGradient id="stripe" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#cfd6e2"/><stop offset=".5" stop-color="#fff"/><stop offset="1" stop-color="#aab4c6"/></linearGradient>')
D.d('<linearGradient id="beam" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#fff2b0" stop-opacity=".7"/><stop offset="1" stop-color="#fff2b0" stop-opacity="0"/></linearGradient>')
D.d('<radialGradient id="bulb" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="#fffbe0"/><stop offset=".6" stop-color="#ffd35a"/><stop offset="1" stop-color="#c8801a"/></radialGradient>')
# rocks + tower + stripes
D.add('<path d="M20,1990 Q60,1900 160,1920 Q300,1860 480,1900 Q600,1890 640,1990Z" fill="#3a3f4c" stroke="#1c2030" stroke-width="4"/>')
D.add('<path d="M108,350 L90,1900 H550 L532,350Z" fill="url(#stripe)" stroke="#2a3040" stroke-width="5"/>')
for (y0, y1) in ((850, 870), (1330, 1350), (1830, 1900)):
    pass
for y in (338, 838, 1318, 1798):
    D.add('<rect x="%d" y="%d" width="%d" height="22" fill="%s"/>' % (96 - (y - 338) // 250, y, 450 + (y - 338) // 125, RED))
# lantern + dome + gallery
D.add('<path d="M190,120 H450 V300 H190Z" fill="#1a2438" stroke="#aebbd4" stroke-width="5"/><path d="M170,120 Q320,0 470,120Z" fill="%s" stroke="#7a1f18" stroke-width="5"/><path d="M320,2 V-14" stroke="#aebbd4" stroke-width="5"/>' % RED)
D.add('<polygon points="450,190 700,100 700,280" fill="url(#beam)"/><polygon points="190,190 -60,100 -60,280" fill="url(#beam)" opacity=".0"/>')
D.add('<rect x="40" y="296" width="560" height="56" rx="10" fill="#2a3040" stroke="#aebbd4" stroke-width="4"/>')
for k in range(15):
    D.add('<path d="M%d,296 V260" stroke="#aebbd4" stroke-width="4"/>' % (60 + k * 38))
D.add('<path d="M52,262 H588" stroke="#aebbd4" stroke-width="4"/>')
# lantern: bulb as master arc meter
D.add('<circle cx="320" cy="205" r="62" fill="url(#bulb)" stroke="#7a5018" stroke-width="3"/>')
for k in range(24):
    a = 140 + k * 10
    p, q = polar(320, 205, 66, a), polar(320, 205, 80, a)
    c = '#35d36b' if k < 14 else ('#f2d13a' if k < 20 else '#ff4545')
    D.add('<path d="M%s,%s L%s,%s" stroke="%s" stroke-width="6" stroke-linecap="round" opacity="%s"/>' % (n(p[0]), n(p[1]), n(q[0]), n(q[1]), c, '1' if k < 17 else '.25'))
D.add(T.knob(320, 205, 32, .72, None, '#222') + T.text(320, 168, '', 1))
D.add(T.knob(226, 258, 15, .5, 'HP LVL', '#fff') + T.knob(414, 258, 15, .35, 'HP MIX', '#fff')) if False else None
D.names['master'] = (230, 140, 180, 130)
D.add(T.text(320, 276, 'MASTER', 11, '#222', weight='700', halo=''))
D.add(T.knob(226, 226, 15, .5, None, '#fff') + T.knob(414, 226, 15, .35, None, '#fff') + T.text(226, 258, 'HP LVL', 11, T.dim, weight='700') + T.text(414, 258, 'HP MIX', 11, T.dim, weight='700'))
# gallery: mic + rec
D.put('mic', b_mic(T), 70, 298) if False else None
# deck panels
def panel(y0, y1, col):
    return '<rect x="128" y="%d" width="384" height="%d" rx="16" fill="#0e1424" stroke="%s" stroke-width="4"/>' % (y0, y1 - y0, col)
def deck(y0, d, col):
    D.add(panel(y0, y0 + 488, col))
    Dd = DECK[d]
    D.add('<rect x="140" y="%d" width="360" height="72" rx="8" fill="#050810" stroke="#2a3040"/><clipPath id="lw%s"><rect x="142" y="%d" width="356" height="68"/></clipPath><g clip-path="url(#lw%s)">' % (y0 + 8, d, y0 + 10, d) + wave(142, y0 + 11, 356, 66, Dd['seed'], col, '#fff', head=178, cues=Dd['cues'], pitch=4) + '</g>')
    cx, cy, r = 340, y0 + 184, 102
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#141c30" stroke="%s" stroke-width="4"/><circle cx="%d" cy="%d" r="%d" fill="none" stroke="#24304c" stroke-width="26"/>' % (cx, cy, r, col, cx, cy, r - 15))
    D.add('<path d="%s" stroke="%s" stroke-width="8" fill="none" stroke-linecap="round"/>' % (arc(cx, cy, r - 15, 110, 300), col))
    p = polar(cx, cy, r - 15, 235)
    D.add('<circle cx="%s" cy="%s" r="10" fill="%s" stroke="#fff" stroke-width="2.5"/><circle cx="%d" cy="%d" r="%d" fill="#050810"/>' % (n(p[0]), n(p[1]), col, cx, cy, r - 34))
    D.add(T.text(cx, cy - 16, Dd['title'][:12], 11, '#fff', weight='700', halo='') + T.text(cx, cy + 20, Dd['bpm'], 26, col, weight='800', mono=True, halo='') + T.text(cx, cy + 40, Dd['t'], 11, '#b8c4d8', mono=True, halo=''))
    D.put('jog ' + d, ('', 2 * r, 2 * r), cx - r, cy - r)
    D.put('tempo ' + d, b_tempo(T, d, col, h=100), 144, y0 + 98)
    D.put('transport ' + d, b_transport(T, d, col), 140, y0 + 304)
    D.put('pads ' + d, b_pads(T, d, col, 48), 140, y0 + 360)
    D.put('loop ' + d, b_loop(T, d, col, 'grid'), 360, y0 + 360)
    D.put('keyshift ' + d, b_keyshift(T, d, col), 360, y0 + 432)
    D.names['wave'] = (142, y0 + 11, 356, 66) if d == 'A' else D.names['wave']
deck(358, 'A', CA)
D.add(panel(856, 1336, '#ffffff'))
D.put('ch1', b_strip(T, 1, CA, 80, 50), 140, 864)
D.put('ch2', b_strip(T, 2, CB, 80, 50), 404, 864)
D.add(T.meter(300, 880, 12, 330, .78) + T.meter(324, 880, 12, 330, .72) + T.text(318, 1226, 'MASTER', 11, T.dim, weight='700'))
D.put('xfader', b_xfader(T, w=330), 155, 1262)
deck(1346, 'B', CB)
# base: door with app buttons, mic, rec
D.add('<path d="M200,1900 V1850 Q200,1830 220,1830 H420 Q440,1830 440,1850 V1900Z" fill="#3a2a1c" stroke="#1c2030" stroke-width="4"/>') if False else None
D.put('mic', b_mic(T), 70, 1838) if False else None
D.add('<rect x="130" y="1840" width="380" height="56" rx="10" fill="#0e1424" stroke="#aebbd4" stroke-width="3"/>')
D.put('mic', b_mic(T), 138, 1842) if False else None
D.put('rec', b_rec(T), 138, 1852)
D.put('appbtns', b_appbtns(T), 322, 1852)
D.names['mic'] = (60, 296, 180, 48)
D.add('<rect x="50" y="1798" width="0" height="0"/>')
# mic on the gallery
D.add('<g transform="translate(70,302)">%s</g>' % b_mic(T)[0].replace('>MIC<', '>MIC<'))
# floating boat browser (+ sail sampler)
D.add('<path d="M740,1120 H1460 Q1420,1420 1200,1440 H1000 Q780,1420 740,1120Z" fill="#7a4a28" stroke="#3a220f" stroke-width="5"/>')
D.add('<path d="M1100,1110 V500 M1100,540 L780,1090 H1100Z" stroke="#3a220f" stroke-width="10" fill="none"/><path d="M1100,540 L780,1090 H1100Z" fill="#f4f1e8" stroke="#c9c2ae" stroke-width="4"/><path d="M1100,500 l80,26 l-80,26z" fill="%s"/>' % RED)
D.add(grip(1100, 1136, '#e8d6b4'))
D.put('browser', b_browser(T, CA, 660, 270, 'LOGBOOK  ·  TRACK BROWSER', nrows=4), 770, 1152)
D.put('sampler', b_sampler(T), 880, 980)
D.add('<path d="M700,1460 q60,-24 120,0 t120,0 t120,0 t120,0 t120,0 t120,0" stroke="#4fa0d8" stroke-width="6" fill="none"/><path d="M700,1500 q60,-24 120,0 t120,0 t120,0 t120,0 t120,0 t120,0" stroke="#4fa0d8" stroke-width="6" fill="none" opacity=".6"/>')
D.put('status', b_status(T), 720, 1560)
D.save(os.path.dirname(__file__))
