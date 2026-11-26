import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1530
D = Doc(W, H)
CA, CB, GOLD = '#52e0c4', '#ff8fb1', '#e8c46a'
class Spec(Theme):
    ink = '#fff3e0'; dim = '#e6cfae'; btn_fill = '#2a1608'; btn_stroke = GOLD; btn_text = '#fff3e0'; accent = CA; on_text = '#06201a'
    colA = CA; colB = CB; knob_fill = '#3a2010'; knob_stroke = GOLD; track = '#120802'; wave_bg = '#0a0f1c'; halo = '#1a0c04'
T = Spec()
D.d('<linearGradient id="tort" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#c2692a"/><stop offset=".4" stop-color="#7a3a14"/><stop offset=".7" stop-color="#b5601f"/><stop offset="1" stop-color="#4a2008"/></linearGradient>')
D.d('<radialGradient id="lens" cx="40%" cy="30%" r="80%"><stop offset="0" stop-color="#1c2a52"/><stop offset="1" stop-color="#070b18"/></radialGradient>')
LY, LR = 410, 270
# temple arms (vertical bars) + frame
for sx in (1, -1):
    X = (lambda x: x) if sx == 1 else (lambda x: W - x)
    D.add('<rect x="%s" y="230" width="86" height="640" rx="43" fill="url(#tort)" stroke="%s" stroke-width="4"/><circle cx="%s" cy="250" r="9" fill="%s"/>' % (n(X(30) if sx == 1 else X(30) - 86), GOLD, n(X(73) if sx == 1 else X(73)), GOLD))
# bridge plate
D.add('<path d="M670,170 Q800,110 930,170 V940 Q800,990 670,940Z" fill="url(#tort)" stroke="%s" stroke-width="5"/>' % GOLD)
# bifocal tabs
for sx in (1, -1):
    x0 = 190 if sx == 1 else W - 190 - 420
    D.add('<path d="M%d,640 H%d L%d,900 Q%d,930 %d,930 H%d Q%d,930 %d,900Z" fill="url(#tort)" stroke="%s" stroke-width="5"/>' % (x0, x0 + 420, x0 + 420, x0 + 420, x0 + 380, x0 + 40, x0, x0, GOLD))
# lenses
for cx, d, col in ((400, 'A', CA), (1200, 'B', CB)):
    D.add('<circle cx="%d" cy="%d" r="%d" fill="url(#tort)" stroke="%s" stroke-width="6"/>' % (cx, LY, LR + 14, GOLD))
    D.add('<circle cx="%d" cy="%d" r="%d" fill="none" stroke="#1a0c04" stroke-width="32" opacity=".55"/>' % (cx, LY, LR - 16))   # jog ring
    D.add('<path d="%s" stroke="%s" stroke-width="10" fill="none" stroke-linecap="round"/>' % (arc(cx, LY, LR - 16, 110, 300 if d == 'A' else 250), col))
    ma = 235 if d == 'A' else 305
    D.add('<circle cx="%s" cy="%s" r="13" fill="%s" stroke="#fff" stroke-width="3"/>' % (n(cx + (LR - 16) * math.cos(math.radians(ma))), n(LY + (LR - 16) * math.sin(math.radians(ma))), col))
    D.add('<circle cx="%d" cy="%d" r="%d" fill="url(#lens)" stroke="#000" stroke-width="3"/><clipPath id="l%s"><circle cx="%d" cy="%d" r="%d"/></clipPath>' % (cx, LY, LR - 36, d, cx, LY, LR - 37))
    Dd = DECK[d]
    D.add('<g clip-path="url(#l%s)">' % d + wave(cx - 230, LY - 58, 460, 116, Dd['seed'], col, '#fff', head=230, cues=Dd['cues'], pitch=4) + '</g>')
    D.add(T.text(cx, LY - 130, Dd['title'], 20, '#fff', weight='700', halo='') + T.text(cx, LY - 108, Dd['artist'], 13, '#b9c4e4', halo='') + T.text(cx, LY - 150, 'DECK ' + d, 12, col, weight='800', halo=''))
    D.add(T.text(cx - 80, LY + 118, Dd['bpm'], 30, col, 'middle', '800', mono=True, halo='') + T.text(cx + 80, LY + 108, Dd['t'], 17, '#fff', 'middle', '700', mono=True, halo='') + T.text(cx + 80, LY + 130, Dd['rem'], 14, '#b9c4e4', 'middle', mono=True, halo='') + T.text(cx - 80, LY + 138, 'BPM  KEY ' + Dd['key'], 11, '#b9c4e4', weight='700', halo=''))
    D.put('jog ' + d, ('', 2 * LR, 2 * LR), cx - LR, LY - LR)
D.names['wave'] = (170, 352, 460, 116)
both(D, T, 'pads', b_pads, 216, 704, s=48)
both(D, T, 'loop', b_loop, 440, 704, orient='grid')
both(D, T, 'keyshift', b_keyshift, 440, 776)
both(D, T, 'transport', b_transport, 216, 822)
# temple tempo arms
both(D, T, 'tempo', b_tempo, 44, 290, h=190)
# bridge mixer
D.put('ch1', b_strip(T, 1, CA, 90, 50), 690, 190)
D.put('ch2', b_strip(T, 2, CB, 90, 50), 814, 190) if False else None
D.put('ch2', b_strip(T, 2, CB, 90, 50), 804, 190) if False else None
D.names['ch2'] = (804, 190, 96, 400)
D.add(b_strip(T, 2, CB, 90, 50)[0].join(['<g transform="translate(806,190)">', '</g>']))
D.put('master', b_master(T), 678, 610)
D.put('xfader', b_xfader(T, w=220), 690, 730)
D.put('mic', b_mic(T), 710, 830)
D.put('rec', b_rec(T), 761, 884) if False else None
# floating glasses-case browser
D.add('<path d="M100,1010 Q100,980 130,980 H1010 Q1040,980 1040,1010 V1440 Q1040,1470 1010,1470 H130 Q100,1470 100,1440Z" fill="#3a2010" stroke="%s" stroke-width="6"/><path d="M100,1100 H1040" stroke="%s" stroke-width="3" stroke-dasharray="14 8"/><rect x="540" y="1090" width="60" height="30" rx="8" fill="%s"/>' % (GOLD, GOLD, GOLD))
D.add(grip(570, 1000, GOLD))
D.put('browser', b_browser(T, CA, 880, 330, 'CASE  ·  TRACK BROWSER', nrows=6), 120, 1050)
# floating lens-cloth tray: sampler, rec, app
D.add('<path d="M1100,1010 q40,-30 80,0 t80,0 t80,0 t80,0 V1440 q-40,30 -80,0 t-80,0 t-80,0 t-80,0Z" fill="#3a2010" stroke="%s" stroke-width="5"/>' % GOLD)
D.add(grip(1320, 1034, GOLD))
D.put('sampler', b_sampler(T), 1219, 1080)
D.put('rec', b_rec(T), 1150, 1230)
D.put('appbtns', b_appbtns(T), 1150, 1300)
D.put('status', b_status(T), 30, 1486)
D.save(os.path.dirname(__file__))
