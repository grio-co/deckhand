import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1640
D = Doc(W, H, '#2a1238')
PINK, ORG, TEAL, PUR, GOLD = '#ee4f8b', '#ffa51f', '#14b8a6', '#5b2a86', '#ffd25a'

class Sugar(Theme):
    ink = '#3b1d4a'; dim = '#6b3c7d'; btn_fill = '#4a1f5c'; btn_stroke = '#ffd25a'; btn_text = '#fff4d6'
    accent = ORG; on_text = '#2a0e00'; knob_fill = '#4a1f5c'; knob_stroke = '#ffd25a'; track = '#2b1038'
    meter_off = '#8a6aa0'; pad_off = '#fff2dc'; wave_bg = '#1b0a2a'; colA = '#10a89b'; colB = '#e0307a'; btn_r = 12; pad_r = 16
    core = '#fff'
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        col = col or s.accent
        ang = 135 + 270 * val
        o = flower(cx, cy, r + 6, 8, col, '#3b1d4a')
        o += '<circle cx="%s" cy="%s" r="%s" fill="#fff4d6" stroke="#3b1d4a" stroke-width="2"/>' % (n(cx), n(cy), n(r * .55))
        o += '<path d="M%s,%s L%s,%s" stroke="#3b1d4a" stroke-width="3.5" stroke-linecap="round"/>' % (n(cx), n(cy), n(cx + (r + 4) * math.cos(math.radians(ang))), n(cy + (r + 4) * math.sin(math.radians(ang))))
        if label:
            o += s.text(cx, cy + r + 18, label, 11, s.dim, weight='700')
        return o
    def pad(s, x, y, w, h, i, lit, col=None, label=None):
        col = col or HC[i % 8]
        o = s.shape(x, y, w, h, col if lit else '#fff2dc', '#3b1d4a', 3, 16)
        o += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="2" stroke-dasharray="3 4"/>' % (n(x + w / 2), n(y + h / 2), n(w / 2 - 6), '#3b1d4a')
        o += s.text(x + w / 2, y + h / 2 + 6, label or str(i + 1), 17, '#2a0e00' if lit else '#3b1d4a', weight='800')
        return o
T = Sugar(); DK = Theme()
D.d('<radialGradient id="sg" cx="50%" cy="40%" r="75%"><stop offset="0" stop-color="#fffdf6"/><stop offset=".7" stop-color="#fff0d8"/><stop offset="1" stop-color="#f3d9b8"/></radialGradient>')
D.d('<radialGradient id="bg" cx="50%" cy="35%" r="80%"><stop offset="0" stop-color="#5a2a78"/><stop offset="1" stop-color="#1c0b28"/></radialGradient>')
D.d('<radialGradient id="soc" cx="50%" cy="50%" r="60%"><stop offset="0" stop-color="#2c1240"/><stop offset="1" stop-color="#10051a"/></radialGradient>')
left = [(800, 58), (590, 62), (410, 118), (290, 225), (238, 350), (228, 480), (250, 580), (222, 650), (280, 720), (330, 790), (345, 880), (380, 1010), (450, 1110), (600, 1170), (800, 1190)]
pts = sym(left, W); poly = sample(pts)
# ears
for sx in (1, -1):
    cx = 800 - sx * 670
    D.add('<ellipse cx="%s" cy="455" rx="112" ry="205" fill="url(#sg)" stroke="#3b1d4a" stroke-width="5"/>' % cx)
    D.add('<ellipse cx="%s" cy="455" rx="96" ry="190" fill="none" stroke="%s" stroke-width="3" stroke-dasharray="2 9" stroke-linecap="round"/>' % (cx, PINK))
D.d('<mask id="cut"><rect width="4000" height="4000" fill="#fff"/><path d="M800,612 C700,520 680,640 800,700 C920,640 900,520 800,612Z" fill="#000"/></mask>')
D.add('<path d="%s" fill="url(#sg)" stroke="#3b1d4a" stroke-width="6" mask="url(#cut)"/>' % smooth(pts))
# crown flowers along dome
crown = [(330, 210), (372, 158), (428, 112), (500, 82), (575, 62), (650, 50)]
for k, (x, y) in enumerate(crown):
    for sx in (1, -1):
        px = x if sx == 1 else W - x
        c = [PINK, ORG, TEAL][k % 3]
        D.add(flower(px, y - 10, 38, 8, c, '#3b1d4a') + '<circle cx="%d" cy="%d" r="13" fill="%s" stroke="#3b1d4a" stroke-width="2"/>' % (px, y - 10, GOLD))
# decorative swirls, dots
for sx in (1, -1):
    def X(x): return x if sx == 1 else W - x
    D.add('<path d="M%d,265 Q%d,225 %d,300 M%d,300 Q%d,250 %d,320" stroke="%s" stroke-width="4" fill="none" stroke-linecap="round"/>' % (X(260), X(250), X(300), X(240), X(235), X(285), PINK))
    for k in range(7):
        D.add('<circle cx="%d" cy="%d" r="5" fill="%s"/>' % (X(300 + k * 18), 780 - abs(k - 3) * 6, TEAL))
D.add('<path d="M800,20 L812,52 L846,54 L820,74 L830,106 L800,88 L770,106 L780,74 L754,54 L788,52Z" fill="%s" stroke="#3b1d4a" stroke-width="2"/>' % PINK) if False else None
# eyes: rosettes
EY = 455
for cx, d, col in ((462, 'A', T.colA), (1138, 'B', T.colB)):
    D.add('<circle cx="%d" cy="%d" r="194" fill="#3b1d4a"/>' % (cx, EY))
    D.add(flower(cx, EY, 190, 18, PINK if d == 'B' else TEAL, '#3b1d4a', rot=0, pr=34))
    D.add(flower(cx, EY, 160, 18, ORG, '#3b1d4a', rot=10, pr=26))
    D.add('<circle cx="%d" cy="%d" r="134" fill="%s" stroke="#3b1d4a" stroke-width="4"/><circle cx="%d" cy="%d" r="126" fill="url(#soc)" stroke="%s" stroke-width="3"/>' % (cx, EY, GOLD, cx, EY, GOLD))
    ma = 215 if d == 'A' else 325
    D.add('<circle cx="%s" cy="%s" r="13" fill="#fff" stroke="#3b1d4a" stroke-width="3"/>' % (n(cx + 175 * math.cos(math.radians(ma))), n(EY + 175 * math.sin(math.radians(ma)))))
    D.add('<clipPath id="lens%s"><circle cx="%d" cy="%d" r="124"/></clipPath>' % (d, cx, EY))
    D.add('<g clip-path="url(#lens%s)">' % d + wave(cx - 124, EY - 52, 248, 104, DECK[d]['seed'], '#35e0d2' if d == 'A' else '#ff6fb1', '#fff', head=124, cues=DECK[d]['cues'][:3], pitch=4) + '</g>')
    D.add(T.text(cx, EY - 76, d, 28, GOLD, weight='800') + T.text(cx, EY + 94, 'JOG', 11, '#b69ac8', weight='700'))
    D.put('jog ' + d, ('', 380, 380), cx - 190, EY - 190)
D.put('wave', ('', 248, 104), 338, EY - 52)
# beat garland across bridge
for k in range(11):
    x = 664 + k * 27.2
    D.add('<circle cx="%s" cy="%d" r="%s" fill="%s" stroke="#3b1d4a" stroke-width="2"/>' % (n(x), EY + 22 * math.sin(k / 10 * math.pi) - 10 + 12, 9 if k != 5 else 14, [ORG, PINK, GOLD, TEAL][k % 4]))
D.add(T.text(800, EY + 52, 'beat garland', 11, T.dim, weight='700'))
# nose heart + cavity
D.add('<path d="M800,612 C700,520 680,640 800,700 C920,640 900,520 800,612Z" fill="none" stroke="#3b1d4a" stroke-width="5"/>')
# teeth
tw, pitch, tx0 = 54, 58.75, 330
upA = ['▶', 'CUE', 'SYNC', 'KEY LK', 'QUANT', 'SLIP', 'REV', '±8%']
loA = ['IN', 'OUT', '◂', '4', '▸', 'KEY−', 'KEY+', 'EJECT']
for i in range(16):
    d, j = ('A', i) if i < 8 else ('B', i - 8)
    col = T.colA if d == 'A' else T.colB
    on_up = {'A': (0, 3, 4), 'B': (2, 4, 6)}[d]
    x = tx0 + i * pitch
    D.add(tooth_btn(T, x, 800, tw, 80, upA[j], j in on_up, col if True else ORG, True, 12, '#fffdf6', '#3b1d4a', '#3b1d4a', '#fff'))
    D.add(tooth_btn(T, x, 890, tw, 66, loA[j], j == 1 and d == 'A', col, False, 12, '#fffdf6', '#3b1d4a', '#3b1d4a', '#fff'))
for d in 'AB':
    x0 = tx0 + (0 if d == 'A' else 8 * pitch)
    for nm, a, b, y, h in (('transport', 0, 3, 800, 80), ('toggles', 3, 7, 800, 80), ('loop', 0, 5, 890, 66), ('keyshift', 5, 7, 890, 66)):
        D.names[nm + ' ' + d] = (x0 + a * pitch, y, (b - a) * pitch, h)
# controls
both(D, T, 'info', b_info, 410, 140, w=260)
both(D, T, 'overview', b_overview, 415, 226, w=250)
both(D, T, 'pads', b_pads, 296, 655, s=48)
both(D, T, 'padmodes', b_padmodes, 524, 655, orient='grid')
both(D, T, 'fx', b_fx, 32, 318)
both(D, T, 'tempo', b_tempo, 102, 440, h=60)
D.put('master', b_master(T), 678, 140)
D.put('sampler', b_sampler(T), 699, 14)
D.put('mic', b_mic(T), 710, 290)
D.put('rec', b_rec(T), 761, 348)
D.put('appbtns', b_appbtns(T), 710, 396)
D.put('ch1', b_strip_h(T, 1, T.colA), 503, 980)
D.put('ch2', b_strip_h(T, 2, T.colB), 503, 1050)
D.put('xfader', b_xfader(T, w=420), 590, 1120)
# papel picado browser window
D.add('<path d="M300,1232 Q800,1262 1300,1232" stroke="%s" stroke-width="3" fill="none"/>' % GOLD)
for k in range(15):
    x = 310 + k * 66
    y = 1236 + 10 * math.sin((x - 300) / 1000 * math.pi) * 1.0
    D.add('<path d="M%d,%d h50 v34 l-8,-8 l-9,8 l-8,-8 l-9,8 l-8,-8 l-8,8z" fill="%s" stroke="#3b1d4a" stroke-width="2"/><circle cx="%d" cy="%d" r="7" fill="#fff4d6"/>' % (x, y, [PINK, ORG, TEAL, GOLD][k % 4], x + 25, y + 16))
D.put('browser', b_browser(DK, ORG, 900, 300, 'OFRENDA  ·  TRACK BROWSER', nrows=6), 350, 1286)
D.put('status', b_status(DK), 540, 1604)
D.save(os.path.dirname(__file__))
