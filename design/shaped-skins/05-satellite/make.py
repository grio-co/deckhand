import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1580
D = Doc(W, H)
CA, CB, FOIL = '#4fd2ff', '#ffcf4a', '#e7c36a'
class Sat(Theme):
    ink = '#e9f3ff'; dim = '#a9c4e4'; btn_fill = '#0e1a33'; btn_stroke = '#9fc3f0'; btn_text = '#e9f3ff'; accent = CA; on_text = '#02101c'
    colA = CA; colB = CB; knob_fill = '#16294d'; knob_stroke = '#9fc3f0'; track = '#050b18'; wave_bg = '#050b18'; halo = '#0a1630'
T = Sat()
D.d('<linearGradient id="cell" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#26418a"/><stop offset="1" stop-color="#0d1c46"/></linearGradient>')
D.d('<linearGradient id="foil" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#f6dc92"/><stop offset=".5" stop-color="#c79a3a"/><stop offset="1" stop-color="#8a6420"/></linearGradient>')
D.d('<linearGradient id="hub" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#aeb8c8"/><stop offset=".5" stop-color="#e8edf5"/><stop offset="1" stop-color="#8f9aad"/></linearGradient>')
def cham(x, y, w, h, c):
    return 'M%s,%s h%s l%s,%s v%s l-%s,%s h-%s l-%s,-%s v-%s z' % (n(x + c), n(y), n(w - 2 * c), n(c), n(c), n(h - 2 * c), n(c), n(c), n(w - 2 * c), n(c), n(c), n(h - 2 * c))
# truss + antenna
D.add('<path d="M580,300 H620 M580,620 H620 M980,300 H1020 M980,620 H1020" stroke="#8f9aad" stroke-width="16"/>')
D.add('<path d="M800,120 V40" stroke="url(#hub)" stroke-width="10"/><circle cx="800" cy="34" r="12" fill="#ff5a4a"/><path d="M730,90 Q800,20 870,90" stroke="url(#hub)" stroke-width="8" fill="none"/>')
# solar wings (decks)
for sx, d, col in ((1, 'A', CA), (-1, 'B', CB)):
    x0 = 60 if sx == 1 else W - 580
    D.add('<path d="%s" fill="url(#cell)" stroke="url(#foil)" stroke-width="6"/>' % cham(x0, 190, 520, 540, 36))
    for k in range(1, 8):
        D.add('<path d="M%d,196 V724" stroke="#6f93e8" stroke-opacity=".18"/>' % (x0 + k * 65))
    for k in range(1, 9):
        D.add('<path d="M%d,%d H%d" stroke="#6f93e8" stroke-opacity=".18"/>' % (x0 + 6, 190 + k * 60, x0 + 514))
JR = 135
for cx, d, col in ((320, 'A', CA), (W - 320, 'B', CB)):
    cy = 370
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#0a1630" stroke="url(#foil)" stroke-width="6"/><circle cx="%d" cy="%d" r="%d" fill="none" stroke="#16294d" stroke-width="30"/>' % (cx, cy, JR, cx, cy, JR - 22))
    D.add('<path d="%s" stroke="%s" stroke-width="9" fill="none" stroke-linecap="round"/>' % (arc(cx, cy, JR - 22, 120, 300 if d == 'A' else 240), col))
    ma = 235 if d == 'A' else 305
    D.add('<circle cx="%s" cy="%s" r="11" fill="%s" stroke="#fff" stroke-width="2.5"/><circle cx="%d" cy="%d" r="%d" fill="#040a16"/>' % (n(cx + (JR - 22) * math.cos(math.radians(ma))), n(cy + (JR - 22) * math.sin(math.radians(ma))), col, cx, cy, JR - 48))
    Dd = DECK[d]
    D.add(T.text(cx, cy - 18, Dd['title'], 14, '#fff', weight='700', halo='') + T.text(cx, cy + 22, Dd['bpm'], 32, col, weight='800', mono=True, halo='') + T.text(cx, cy + 44, Dd['t'] + '  ' + Dd['key'], 12, '#a9c4e4', mono=True, halo=''))
    D.put('jog ' + d, ('', 2 * JR, 2 * JR), cx - JR, cy - JR)
both(D, T, 'pads', b_pads, 80, 540, s=48)
both(D, T, 'loop', b_loop, 306, 540, orient='grid')
both(D, T, 'keyshift', b_keyshift, 306, 612)
both(D, T, 'transport', b_transport, 80, 662)
both(D, T, 'tempo', b_tempo, 480, 230, h=170)
both(D, T, 'toggles', b_toggles, 306, 662, orient='grid') if False else None
# hub
D.add('<path d="%s" fill="url(#hub)" stroke="#6b7690" stroke-width="5"/>' % cham(604, 120, 392, 770, 50))
D.add('<rect x="618" y="136" width="364" height="190" rx="14" fill="#050b18" stroke="#9fc3f0" stroke-width="3"/>')
D.add('<g>' + wave(626, 144, 348, 84, 11, CA, '#fff', head=174, cues=[.1, .3, .62, .8], pitch=4) + wave(626, 232, 348, 84, 29, CB, '#fff', head=174, cues=[.12, .27, .55], pitch=4) + '</g>')
D.names['wave'] = (626, 144, 348, 172)
D.add('<rect x="630" y="336" width="340" height="410" rx="12" fill="#0e1a33" fill-opacity=".92"/>')
D.put('ch1', b_strip(T, 1, CA, 90, 50), 640, 344)
D.put('ch2', b_strip(T, 2, CB, 90, 50), 864, 344)
D.add(T.knob(800, 372, 18, .72, 'MASTER', '#fff') + T.knob(800, 442, 15, .5, 'HP LVL', '#fff') + T.knob(800, 508, 15, .35, 'HP MIX', '#fff'))
D.add(T.meter(782, 560, 12, 150, .8) + T.meter(806, 560, 12, 150, .74))
D.names['master'] = (770, 350, 60, 370)
D.put('xfader', b_xfader(T, w=300), 650, 760)
D.put('mic', b_mic(T), 710, 910)
D.put('rec', b_rec(T), 761, 968)
# floating solar-array browser
D.add('<path d="M120,1170 L1020,1140 L1020,1540 L120,1540Z" fill="url(#cell)" stroke="url(#foil)" stroke-width="6"/>')
D.add('<path d="M1020,1250 H1060 M1020,1450 H1060" stroke="#8f9aad" stroke-width="14"/>')
D.add(grip(570, 1160, '#e7c36a'))
D.put('browser', b_browser(T, CA, 780, 330, 'SOLAR ARRAY  ·  BROWSER', nrows=6), 150, 1190)
# floating probe pod: sampler + app
D.add('<rect x="1090" y="1130" width="440" height="410" rx="120" fill="url(#hub)" stroke="#6b7690" stroke-width="5"/><circle cx="1310" cy="1335" r="170" fill="#0e1a33" stroke="#9fc3f0" stroke-width="4"/>')
D.add(grip(1310, 1150, '#4a5670'))
D.put('sampler', b_sampler(T), 1209, 1290)
D.put('appbtns', b_appbtns(T), 1220, 1440)
D.put('status', b_status(T), 30, 1548)
D.save(os.path.dirname(__file__))
