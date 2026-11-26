import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 800, 900
D = Doc(W, H)
BL, OR = '#2f7df6', '#f2602f'
class Peb(Theme):
    ink = '#2a2535'; dim = '#4a4560'; btn_fill = '#2a2535'; btn_stroke = '#2a2535'; btn_text = '#ffffff'; accent = BL; on_text = '#ffffff'
    colA = BL; colB = OR; knob_fill = '#2a2535'; knob_stroke = '#2a2535'; track = '#1c1828'; wave_bg = '#14111e'; meter_off = '#8f8aa6'; halo = ''
    btn_sw = 1
T = Peb(); DK = Theme()
D.d('<radialGradient id="peb" cx="40%" cy="25%" r="85%"><stop offset="0" stop-color="#f2eef8"/><stop offset=".6" stop-color="#cdc6dc"/><stop offset="1" stop-color="#9d95b4"/></radialGradient>')
left = [(400, 56), (300, 38), (150, 52), (60, 120), (28, 200), (58, 290), (160, 346), (400, 362)]
D.add('<path d="%s" fill="url(#peb)" stroke="#7d7596" stroke-width="4"/>' % smooth(sym(left, W)))
D.add('<ellipse cx="260" cy="90" rx="150" ry="28" fill="#fff" opacity=".35"/>')
# mini jogs with play inside
for cx, d, col in ((150, 'A', BL), (650, 'B', OR)):
    D.add('<circle cx="%d" cy="160" r="76" fill="#2a2535" stroke="%s" stroke-width="4"/><circle cx="%d" cy="160" r="60" fill="none" stroke="#443d5c" stroke-width="18"/>' % (cx, col, cx))
    D.add('<path d="%s" stroke="%s" stroke-width="6" fill="none" stroke-linecap="round"/>' % (arc(cx, 160, 60, 120, 300), col))
    p = polar(cx, 160, 60, 235 if d == 'A' else 305)
    D.add('<circle cx="%s" cy="%s" r="8" fill="%s" stroke="#fff" stroke-width="2"/>' % (n(p[0]), n(p[1]), col))
    D.add(T.btn(cx - 26, 134, 52, 52, '', d == 'A', col, 12, 'play'))
    D.put('jog ' + d, ('', 152, 152), cx - 76, 84)
    D.put('transport ' + d, (T.btn(0, 0, 64, 34, 'CUE', False, col, 12) + T.btn(70, 0, 64, 34, 'SYNC', d == 'B', col, 12), 134, 34), cx - 67, 262)
D.names['transport A'] = (83, 262, 134, 34); D.names['transport B'] = (583, 262, 134, 34)
# centre: two lanes, bpm, crossfader
D.add('<rect x="250" y="66" width="300" height="112" rx="10" fill="#14111e" stroke="#2a2535" stroke-width="2"/>')
D.add('<g>' + wave(254, 70, 292, 50, 11, '#6fa8ff', '#fff', head=146, cues=[.1, .3, .6], pitch=3, labels=False) + wave(254, 124, 292, 50, 29, '#ff9a6a', '#fff', head=146, cues=[.12, .5], pitch=3, labels=False) + '</g>')
D.names['wave'] = (254, 70, 292, 104)
D.add(T.text(300, 208, DECK['A']['bpm'], 22, BL, weight='800', mono=True) + T.text(500, 208, DECK['B']['bpm'], 22, OR, weight='800', mono=True) + T.text(400, 206, 'BPM', 11, T.dim, weight='700'))
D.put('xfader', b_xfader(T, w=280), 260, 222)
D.add(T.btn(356, 296, 88, 34, '⤢ EXPAND', False, '#2a2535', 12))
D.add(T.knob(240, 316, 15, .72, None, BL) if False else '')
D.names['master'] = (246, 296, 100, 34)
D.add(T.knob(290, 312, 15, .72, 'MASTER', '#2a2535') + T.knob(510, 312, 15, .5, 'HP', '#2a2535'))
D.names['master'] = (285, 296, 230, 36)
D.names['tempo A'] = (83, 262, 134, 34); D.names['tempo B'] = (583, 262, 134, 34)
D.names['loop A'] = (83, 262, 134, 34); D.names['loop B'] = (583, 262, 134, 34)
D.add(grip(400, 50, '#2a2535', .6))
# floating mini modules
def pill(x, y, w, h):
    return '<rect x="%d" y="%d" width="%d" height="%d" rx="%d" fill="url(#peb)" stroke="#7d7596" stroke-width="3"/>' % (x, y, w, h, min(h // 2, 36)) + grip(x + w / 2, y + 12, '#2a2535', .7)
D.add(pill(30, 410, 440, 76))
D.put('pads A', pad_row(T, 'A', BL, 46, 6), 48, 428) if False else None
x0 = 50
for d, col, xx in (('A', BL, 50), ('B', OR, 270)):
    sv = ''.join(T.pad(i * 54, 0, 46, 46, i, i in DECK[d]['lit']) for i in range(4))
    D.put('pads ' + d, (sv, 210, 46), xx, 432)
D.add(pill(30, 510, 740, 150))
D.put('ch1', b_strip_h(T, 1, BL), 60, 540)
D.put('ch2', b_strip_h(DK, 2, OR), 60, 600) if False else None
D.add('<g transform="translate(60,600)">%s</g>' % b_strip_h(T, 2, OR)[0]); D.names['ch2'] = (60, 600, 380, 58)
D.add(pill(500, 410, 270, 76).replace('</svg>', ''))
D.add(T.btn(530, 440, 104, 34, 'LOOP IN/OUT', False, '#2a2535', 11) if False else '')
D.add(T.btn(530, 436, 60, 34, 'IN', False, '#2a2535', 12) + T.btn(596, 436, 60, 34, 'OUT', True, BL, 12) + T.btn(662, 436, 36, 34, '4', False, '#2a2535', 13) + T.btn(704, 436, 36, 34, '±', False, '#2a2535', 13))
# browser pill (floating)
D.add('<rect x="30" y="690" width="640" height="190" rx="48" fill="url(#peb)" stroke="#7d7596" stroke-width="3"/>')
D.add(grip(350, 704, '#2a2535', .7))
D.put('browser', b_browser(DK, BL, 560, 150, 'PICKER', nrows=3), 70, 722) if False else None
rows = [('Midnight Ossuary', '128.0'), ('Bone China Heart', '126.0'), ('Tibia Tides', '124.0')]
D.add('<rect x="64" y="724" width="570" height="140" rx="26" fill="#14111e"/>' + DK.text(90, 752, '⌕ search', 12, '#a9a4c0', 'start') + ''.join(DK.text(90, 780 + i * 26, r[0], 13, '#fff' if i != 1 else '#ffd0a8', 'start', '700') + DK.text(560, 780 + i * 26, r[1], 13, '#cfc9e6', 'end', mono=True) for i, r in enumerate(rows)))
D.add(T.btn(574, 728, 52, 30, 'LOAD', False, BL, 11) if False else '')
D.names['browser'] = (64, 724, 570, 140)
D.put('status', b_status(DK, 560), 100, 880) if False else None
D.save(os.path.dirname(__file__))
