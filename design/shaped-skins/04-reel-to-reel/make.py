import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1480
D = Doc(W, H)
OR, SK = '#ffa24a', '#52d1e6'
class Reel(Theme):
    ink = '#f4ead2'; dim = '#d8c9a4'; btn_fill = '#26221c'; btn_stroke = '#cdb98f'; btn_text = '#f4ead2'; accent = OR; on_text = '#1a1208'
    colA = OR; colB = SK; knob_fill = '#3a342a'; knob_stroke = '#cdb98f'; track = '#0c0a07'; wave_bg = '#0f0c08'; halo = '#141008'
T = Reel()
D.d('<linearGradient id="face" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2a2620"/><stop offset="1" stop-color="#14120e"/></linearGradient>')
D.d('<radialGradient id="fl" cx="35%" cy="30%" r="80%"><stop offset="0" stop-color="#f4f4f6"/><stop offset=".5" stop-color="#aab0bc"/><stop offset="1" stop-color="#6c7280"/></radialGradient>')
D.d('<linearGradient id="champ" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#e9d9b0"/><stop offset="1" stop-color="#a89468"/></linearGradient>')
RC, RY, RR = (370, 1230), 320, 262
# plinth + tower (single silhouette with feet)
D.add('<path d="M70,560 Q70,520 120,520 H1480 Q1530,520 1530,560 V860 Q1530,900 1490,900 H1440 L1420,940 H1180 L1160,900 H440 L420,940 H180 L160,900 H110 Q70,900 70,860Z" fill="url(#champ)" stroke="#6a5a3a" stroke-width="5"/>')
D.add('<rect x="100" y="556" width="1400" height="316" rx="20" fill="url(#face)" stroke="#6a5a3a" stroke-width="3"/>')
D.add('<path d="M610,150 Q610,110 650,110 H950 Q990,110 990,150 V540 H610Z" fill="url(#champ)" stroke="#6a5a3a" stroke-width="5"/>')
# reels
for cx, d, col in ((370, 'A', OR), (1230, 'B', SK)):
    cy = 330
    pack = 232 if d == 'A' else 198
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#6b4a2a" stroke="#3a2812" stroke-width="3"/>' % (cx, cy, pack))
    D.add('<mask id="hm%s"><rect width="4000" height="4000" fill="#fff"/>' % d + ''.join('<circle cx="%s" cy="%s" r="66" fill="#000"/>' % (n(cx + 150 * math.cos(math.radians(a))), n(cy + 150 * math.sin(math.radians(a)))) for a in (90, 210, 330)) + '</mask>')
    D.add('<circle cx="%d" cy="%d" r="262" fill="url(#fl)" stroke="#5a6070" stroke-width="5" mask="url(#hm%s)"/>' % (cx, cy, d))
    D.add('<path d="%s" stroke="%s" stroke-width="10" fill="none" stroke-linecap="round" opacity=".95"/>' % (arc(cx, cy, 250, 115, 300 if d == 'A' else 245), col))   # position arc on the rim
    D.add('<circle cx="%d" cy="%d" r="96" fill="#1c1a16" stroke="#cdb98f" stroke-width="5"/>' % (cx, cy))
    ma = 235 if d == 'A' else 305
    D.add('<circle cx="%s" cy="%s" r="12" fill="%s" stroke="#fff" stroke-width="3"/>' % (n(cx + 84 * math.cos(math.radians(ma))), n(cy + 84 * math.sin(math.radians(ma))), col))
    Dd = DECK[d]
    D.add(T.text(cx, cy - 22, Dd['title'], 12, '#fff', weight='700', halo='') + T.text(cx, cy + 18, Dd['bpm'], 28, col, weight='800', mono=True, halo='') + T.text(cx, cy + 40, Dd['t'], 12, '#d8c9a4', mono=True, halo=''))
    D.put('jog ' + d, ('', 524, 524), cx - 262, cy - 262)
# tape ribbons and head window with the two waveform runs
D.add('<path d="M560,300 L640,300 M560,420 L640,420 M960,300 L1040,300 M960,420 L1040,420" stroke="#6b4a2a" stroke-width="22"/>') if False else None
D.add('<rect x="630" y="236" width="340" height="206" rx="14" fill="#0f0c08" stroke="#cdb98f" stroke-width="4"/>')
D.add('<g clip-path="url(#hw)">'.replace('url(#hw)','url(#hw)') + wave(640, 246, 320, 86, 11, OR, '#fff', head=160, cues=[.1, .3, .62, .8], pitch=4) + wave(640, 346, 320, 86, 29, SK, '#fff', head=160, cues=[.12, .27, .55], pitch=4) + '</g>')
D.d('<clipPath id="hw"><rect x="634" y="240" width="332" height="198" rx="10"/></clipPath>')
D.names['wave'] = (640, 246, 320, 186)
D.add(T.text(650, 270, 'A', 16, OR, 'start', '800', halo='') + T.text(650, 372, 'B', 16, SK, 'start', '800', halo=''))
D.add('<rect x="664" y="112" width="272" height="118" rx="14" fill="#26221c" stroke="#cdb98f" stroke-width="2"/>')
D.put('master', b_master(T), 678, 120)
D.add('<rect x="610" y="446" width="380" height="20" fill="none"/>')
# plinth controls
both(D, T, 'tempo', b_tempo, 118, 574, h=150)
both(D, T, 'pads', b_pads, 196, 606, s=48)
both(D, T, 'transport', b_transport, 196, 726)
both(D, T, 'loop', b_loop, 196, 784)
D.put('ch1', b_strip_h(T, 1, OR), 503, 584)
D.put('ch2', b_strip_h(T, 2, SK), 503, 646)
D.put('xfader', b_xfader(T, w=420), 590, 714)
D.put('mic', b_mic(T), 620, 800)
D.put('rec', b_rec(T), 830, 806)
# floating tape box (browser)
D.add('<path d="M100,1000 L1010,1000 L1030,960 H130 Z" fill="#cdb98f" stroke="#6a5a3a" stroke-width="4"/><rect x="100" y="1000" width="910" height="430" rx="10" fill="#e6d9b6" stroke="#6a5a3a" stroke-width="5"/>')
D.add(grip(555, 980, '#4a3a1a'))
D.put('browser', b_browser(T, OR, 840, 340, 'TAPE BOX  ·  TRACK BROWSER', nrows=7), 135, 1040)
# floating tape-loop cartridge (sampler + app)
D.add('<path d="M1110,1000 H1470 a110,110 0 0 1 0,220 H1110 a110,110 0 0 1 0,-220Z" fill="#26221c" stroke="#cdb98f" stroke-width="5"/><circle cx="1138" cy="1110" r="34" fill="#cdb98f"/><circle cx="1138" cy="1110" r="12" fill="#26221c"/><circle cx="1442" cy="1110" r="34" fill="#cdb98f"/><circle cx="1442" cy="1110" r="12" fill="#26221c"/>')
D.add(grip(1290, 1016, '#cdb98f'))
D.put('sampler', b_sampler(T), 1190, 1062)
D.put('appbtns', b_appbtns(T), 1200, 1250)
D.put('status', b_status(T), 1030, 1400) if False else None
D.put('status', b_status(T), 970, 1440)
D.save(os.path.dirname(__file__))
