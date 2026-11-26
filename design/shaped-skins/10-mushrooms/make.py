import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1640
D = Doc(W, H)
CA, CB, CRM = '#ffd08a', '#9ff0ff', '#f4e8cc'
class Fung(Theme):
    ink = '#2a1c10'; dim = '#5a4228'; btn_fill = '#2a1c10'; btn_stroke = '#2a1c10'; btn_text = '#fff3d8'; accent = '#d9531e'; on_text = '#1a1008'
    colA = '#ffb347'; colB = '#38c8e0'; knob_fill = '#3a2616'; knob_stroke = '#2a1c10'; track = '#1a1008'; wave_bg = '#150c14'; halo = ''; btn_sw = 1
T = Fung()
DKT = Theme(ink='#fff3d8', dim='#e8d6b8', btn_fill='#2a1c30', btn_stroke='#d8c6f0', colA='#ffb347', colB='#38c8e0', accent='#ffb347')
D.d('<radialGradient id="capA" cx="40%" cy="25%" r="90%"><stop offset="0" stop-color="#e6475a"/><stop offset=".6" stop-color="#b8283c"/><stop offset="1" stop-color="#6e1426"/></radialGradient>')
D.d('<radialGradient id="capB" cx="40%" cy="25%" r="90%"><stop offset="0" stop-color="#ffb347"/><stop offset=".6" stop-color="#e08a1e"/><stop offset="1" stop-color="#8a4a0a"/></radialGradient>')
D.d('<radialGradient id="capC" cx="40%" cy="25%" r="90%"><stop offset="0" stop-color="#a468d0"/><stop offset=".6" stop-color="#6a3a8a"/><stop offset="1" stop-color="#34174a"/></radialGradient>')
D.d('<linearGradient id="stem" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#d6c39c"/><stop offset=".5" stop-color="#f6ecd2"/><stop offset="1" stop-color="#cdb98f"/></linearGradient>')
# ground mound + moss
D.add('<path d="M60,1130 Q200,1010 420,1040 Q800,990 1180,1040 Q1400,1010 1540,1130 Q1400,1180 800,1170 Q200,1180 60,1130Z" fill="#3a5a2a" stroke="#22381a" stroke-width="5"/>')
# stems
def stem(x0, x1, ytop, ybot, bx0, bx1, fill='url(#stem)'):
    return '<path d="M%d,%d Q%d,%d %d,%d L%d,%d Q%d,%d %d,%d Z" fill="%s" stroke="#7a6642" stroke-width="5"/>' % (x0, ytop, x0 - 10, (ytop + ybot) // 2, bx0, ybot, bx1, ybot, x1 + 10, (ytop + ybot) // 2, x1, ytop, fill)
D.add(stem(300, 620, 470, 1070, 250, 690))
D.add(stem(980, 1300, 470, 1070, 910, 1350))
D.add(stem(690, 910, 600, 1090, 640, 960))
D.add('<path d="M300,540 Q460,580 620,540 M980,540 Q1140,580 1300,540" stroke="#bfa774" stroke-width="10" fill="none" opacity=".7"/>')   # annulus ring on the stems
# caps (silhouette: dome with scalloped gill rim)
def cap(cx, cy, rx, ry, fid):
    return '<path d="M%d,%d A%d,%d 0 0 1 %d,%d Q%d,%d %d,%d Q%d,%d %d,%d Z" fill="url(#%s)" stroke="#3a1018" stroke-width="6"/>' % (cx - rx, cy, rx, ry, cx + rx, cy, cx + rx * .5, cy + 60, cx, cy + 46, cx - rx * .5, cy + 60, cx - rx, cy, fid)
D.add(cap(430, 390, 345, 270, 'capA')); D.add(cap(1170, 390, 345, 270, 'capB')); D.add(cap(800, 600, 190, 140, 'capC'))
for cx, cy, spots in ((430, 390, [(150, 330, 18), (730, 330, 14), (300, 150, 14), (600, 140, 12)]), (1170, 390, [(870, 330, 14), (1450, 330, 18), (1330, 150, 14), (1000, 140, 12)]), (800, 600, [(710, 520, 14), (880, 520, 16)])):
    for (sx_, sy_, sr) in spots:
        D.add('<circle cx="%d" cy="%d" r="%d" fill="#fff4dc" fill-opacity=".8"/>' % (sx_, sy_, sr))
# decks on caps
for cx, d, col, s in ((430, 'A', CA, 1), (1170, 'B', CB, -1)):
    jx = cx - s * 120
    D.add('<circle cx="%d" cy="285" r="112" fill="#1c1018" stroke="#fff3d8" stroke-width="5"/><circle cx="%d" cy="285" r="112" fill="none" stroke="#3a2230" stroke-width="30" transform="rotate(0)"/>' % (jx, jx))
    D.add('<path d="%s" stroke="%s" stroke-width="9" fill="none" stroke-linecap="round"/>' % (arc(jx, 285, 97, 110, 300 if d == 'A' else 245), col))
    p = polar(jx, 285, 97, 235 if d == 'A' else 305)
    D.add('<circle cx="%s" cy="%s" r="11" fill="%s" stroke="#fff" stroke-width="2.5"/><circle cx="%d" cy="285" r="78" fill="#0c0610"/>' % (n(p[0]), n(p[1]), col, jx))
    Dd = DECK[d]
    D.add(DKT.text(jx, 272, Dd['bpm'], 30, col, weight='800', mono=True) + DKT.text(jx, 298, 'KEY ' + Dd['key'], 12, '#e8d6b8', weight='700') + DKT.text(jx, 318, Dd['t'], 12, '#e8d6b8', mono=True))
    D.put('jog ' + d, ('', 224, 224), jx - 112, 173)
    lx = cx + s * 120 - (145 if s == 1 else 145)
    D.add('<rect x="%d" y="236" width="290" height="94" rx="12" fill="#150c14" stroke="#fff3d8" stroke-width="3"/>' % lx)
    D.add('<clipPath id="lc%s"><rect x="%d" y="240" width="282" height="86" rx="9"/></clipPath><g clip-path="url(#lc%s)">' % (d, lx + 4, d) + wave(lx + 4, 242, 282, 82, Dd['seed'], col, '#fff', head=141, cues=Dd['cues'][:3], pitch=4) + '</g>')
    D.add(DKT.text(lx + 145, 212, Dd['title'], 18, '#fff', weight='700') + DKT.text(lx + 145, 192, Dd['artist'], 12, '#ffe6d0'))
    D.names['wave'] = (lx + 4, 242, 282, 82) if d == 'A' else D.names['wave']
# gill pads under each cap
for cx, d in ((430, 'A'), (1170, 'B')):
    for i in range(8):
        th = (i - 3.5) * 17
        x = cx + 240 * math.sin(math.radians(th))
        y = 415 + 40 * math.cos(math.radians(th)) 
        D.add(rot(T.pad(x - 23, y - 40, 46, 80, i, i in DECK[d]['lit']), x, y, th * 0.8))
    D.names['pads ' + d] = (cx - 280, 380, 560, 100)
# stems: deck controls
both(D, T, 'transport', b_transport, 312, 500)
both(D, T, 'tempo', b_tempo, 548, 500, h=130)
both(D, T, 'keyshift', b_keyshift, 312, 560)
both(D, T, 'loop', b_loop, 312, 610, orient='grid')
both(D, T, 'toggles', b_toggles, 312, 690, orient='grid')
both(D, T, 'padmodes', b_padmodes, 312, 780, orient='grid')
# centre mushroom: master on cap, mixer on stem
D.add(DKT.knob(738, 588, 18, .72, 'MASTER', '#fff') + DKT.knob(800, 588, 15, .5, 'HP LVL', '#fff') + DKT.knob(862, 588, 15, .35, 'HP MIX', '#fff') if False else '')
D.add(DKT.knob(730, 596, 18, .72, 'MASTER', '#fff') + DKT.knob(800, 596, 15, .5, 'HP', '#fff') + DKT.knob(870, 596, 15, .35, 'MIX', '#fff'))
D.add(DKT.meter(765, 530, 70, 10, .78, True, 12))
D.names['master'] = (700, 520, 200, 110)
D.add('<rect x="700" y="660" width="200" height="420" rx="26" fill="#2a1c30" fill-opacity=".92"/>')
D.put('ch1', b_strip(DKT, 1, '#ffb347', 80, 50), 704, 668)
D.put('ch2', b_strip(DKT, 2, '#38c8e0', 80, 50), 800, 668)
# crossfader along the roots
D.add(curve_fader(T, [(560, 1130), (700, 1090), (900, 1090), (1040, 1130)], .46, '#fff', w=14, cap=(40, 32), fill='#bfa774'))
D.add(DKT.text(540, 1160, 'A', 15, '#ffb347', weight='800') + DKT.text(1060, 1160, 'B', 15, '#38c8e0', weight='800') + DKT.text(800, 1160, 'CROSSFADER', 11, '#e8d6b8', weight='700'))
D.names['xfader'] = (560, 1090, 480, 80)
# floating: bracket-fungus browser, spore sampler, toadstool app stool
D.add('<path d="M110,1360 Q110,1220 560,1210 Q1010,1220 1010,1360 Q1010,1470 900,1480 H220 Q110,1470 110,1360Z" fill="url(#capB)" stroke="#3a1018" stroke-width="5"/><path d="M130,1380 Q560,1330 990,1380" stroke="#fff" stroke-opacity=".25" stroke-width="4" fill="none"/>')
D.add(grip(560, 1236, '#3a1018'))
D.put('browser', b_browser(DKT, '#ffb347', 800, 300, 'BRACKET FUNGUS  ·  BROWSER', nrows=5), 160, 1250) if False else None
D.put('browser', b_browser(DKT, '#ffb347', 760, 240, 'BRACKET FUNGUS  ·  BROWSER', nrows=4), 180, 1230)
for (bx, by, br) in ((1260, 1330, 150), (1480, 1440, 70), (1130, 1450, 54)):
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#a468d0" fill-opacity=".35" stroke="#e8d6f8" stroke-width="3"/>' % (bx, by, br))
D.add(grip(1260, 1198, '#e8d6f8'))
D.put('sampler', b_sampler(DKT), 1159, 1280)
D.put('rec', b_rec(DKT), 1150, 1410) if False else None
D.put('appbtns', b_appbtns(DKT), 1180, 1410)
D.put('mic', b_mic(DKT), 1130, 1500) if False else None
D.put('mic', b_mic(T), 330, 880) if False else None
D.save(os.path.dirname(__file__))
