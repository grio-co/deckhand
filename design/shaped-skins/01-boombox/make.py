import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1400
D = Doc(W, H)
AM, TL = '#ffb02e', '#3ad1c8'
class BB(Theme):
    ink = '#eef1f6'; dim = '#aab3c2'; btn_fill = '#1a1d24'; btn_stroke = '#c4ccd9'; btn_text = '#eef1f6'
    accent = AM; colA = AM; colB = TL; knob_fill = '#262a33'; knob_stroke = '#c4ccd9'; track = '#0a0b0e'; wave_bg = '#07080b'
T = BB()
D.d('<linearGradient id="body" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#434955"/><stop offset=".5" stop-color="#2a2e37"/><stop offset="1" stop-color="#1b1e25"/></linearGradient>')
D.d('<linearGradient id="chrome" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f4f7fb"/><stop offset=".5" stop-color="#9aa4b4"/><stop offset="1" stop-color="#d9dfe9"/></linearGradient>')
D.d('<radialGradient id="cone" cx="50%" cy="45%" r="55%"><stop offset="0" stop-color="#3a3f4a"/><stop offset=".7" stop-color="#14161b"/><stop offset="1" stop-color="#050608"/></radialGradient>')
# antenna, handle, body (handle interior is a see-through hole)
D.add('<path d="M1440,232 L1565,40" stroke="url(#chrome)" stroke-width="8" stroke-linecap="round"/><circle cx="1565" cy="40" r="10" fill="url(#chrome)"/>')
D.add('<path d="M300,236 C300,40 1300,40 1300,236" stroke="#14161b" stroke-width="64" fill="none" stroke-linecap="round"/><path d="M300,236 C300,40 1300,40 1300,236" stroke="url(#body)" stroke-width="54" fill="none" stroke-linecap="round"/>')
D.add('<rect x="70" y="230" width="1460" height="740" rx="80" fill="url(#body)" stroke="url(#chrome)" stroke-width="6"/>')
D.add('<rect x="590" y="244" width="420" height="206" rx="16" fill="#07080b" stroke="url(#chrome)" stroke-width="4"/>')
CY_, R = 620, 215
for cx, d, col in ((380, 'A', AM), (1220, 'B', TL)):
    D.add('<circle cx="%d" cy="%d" r="%d" fill="url(#chrome)"/><circle cx="%d" cy="%d" r="%d" fill="#0b0c10"/>' % (cx, CY_, R, cx, CY_, R - 12))
    D.add('<circle cx="%d" cy="%d" r="%d" fill="none" stroke="#20242c" stroke-width="34"/>' % (cx, CY_, R - 36))   # surround = jog ring
    D.add('<path d="%s" stroke="%s" stroke-width="8" fill="none" stroke-linecap="round"/>' % (arc(cx, CY_, R - 36, 110, 300 if d == 'A' else 250), col))
    ma = 230 if d == 'A' else 310
    D.add('<circle cx="%s" cy="%s" r="11" fill="%s" stroke="#fff" stroke-width="2.5"/>' % (n(cx + (R - 36) * math.cos(math.radians(ma))), n(CY_ + (R - 36) * math.sin(math.radians(ma))), col))
    D.add('<circle cx="%d" cy="%d" r="%d" fill="url(#cone)" stroke="#000" stroke-width="3"/>' % (cx, CY_, R - 56))
    for k in range(1, 6):
        D.add('<circle cx="%d" cy="%d" r="%d" fill="none" stroke="#fff" stroke-opacity=".07" stroke-width="3"/>' % (cx, CY_, (R - 56) * k // 6))
    D.add('<circle cx="%d" cy="%d" r="68" fill="#101217" stroke="%s" stroke-width="3"/>' % (cx, CY_, col))
    D.add(T.text(cx, CY_ - 6, d, 40, col, weight='800', halo='') + T.text(cx, CY_ + 30, DECK[d]['bpm'] + ' BPM', 16, '#fff', weight='700', mono=True, halo=''))
    D.put('jog ' + d, ('', 2 * R, 2 * R), cx - R, CY_ - R)
# centre display: two lanes
D.add('<g>' + wave(600, 252, 400, 86, 11, AM, '#fff', head=200, cues=[.1, .3, .62, .8], pitch=4) + wave(600, 350, 400, 86, 29, TL, '#fff', head=200, cues=[.12, .27, .55], pitch=4) + '</g>')
D.names['wave'] = (600, 252, 400, 184)
both(D, T, 'transport', b_transport, 150, 252)
both(D, T, 'info', b_info, 150, 304, w=300)
both(D, T, 'tempo', b_tempo, 92, 410, h=170)
both(D, T, 'loop', b_loop, 140, 838)
both(D, T, 'pads', pad_row, 140, 884, s=48)
D.put('ch1', b_strip(T, 1, AM, 90, 50), 640, 456)
D.put('ch2', b_strip(T, 2, TL, 90, 50), 864, 456)
D.add(T.knob(800, 482, 18, .72, 'MASTER', '#fff') + T.knob(800, 552, 15, .5, 'HP LVL', '#fff') + T.knob(800, 618, 15, .35, 'HP MIX', '#fff'))
D.add(T.meter(782, 680, 12, 150, .8) + T.meter(806, 680, 12, 150, .74))
D.names['master'] = (770, 460, 60, 380)
D.put('xfader', b_xfader(T, w=380), 610, 888)
D.add(T.text(800, 960, '', 1))
# floating cassette browser
D.add('<rect x="120" y="1010" width="960" height="340" rx="26" fill="#d8cdb4" stroke="#6a604c" stroke-width="5"/><path d="M240,1350 L300,1300 H900 L960,1350Z" fill="#a89d85" stroke="#6a604c" stroke-width="4"/>')
D.add('<circle cx="330" cy="1325" r="14" fill="#2a2e37"/><circle cx="870" cy="1325" r="14" fill="#2a2e37"/>')
D.add(grip(600, 1026, '#4a4130'))
D.put('browser', b_browser(T, AM, 860, 250, 'CASSETTE  ·  TRACK BROWSER', nrows=4), 170, 1040)
# floating sampler door
D.add('<path d="M1130,1010 H1500 a26,26 0 0 1 26,26 V1330 a20,20 0 0 1 -20,20 H1150 a20,20 0 0 1 -20,-20 Z" fill="#2a2e37" stroke="url(#chrome)" stroke-width="5"/>')
D.add(grip(1328, 1026, '#c4ccd9'))
D.put('sampler', b_sampler(T), 1226, 1070)
D.put('rec', b_rec(T), 1150, 1200)
D.put('appbtns', b_appbtns(T), 1150, 1260)
D.put('status', b_status(T), 540, 1366) if False else None
D.put('status', b_status(T), 30, 1362)
D.save(os.path.dirname(__file__))
