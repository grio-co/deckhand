import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1640
D = Doc(W, H, '#1d2327')
BRS, BRD, BONE = '#d4a443', '#7a5416', '#e6d8b4'

def gear(cx, cy, r, teeth, fill, stroke, hole=0.35, rot=0):
    pts = []
    for k in range(teeth * 2):
        a = math.radians(rot + 360.0 * k / (teeth * 2))
        rr = r if k % 2 == 0 else r * 0.84
        for da in (-0.5, 0.5):
            aa = a + math.radians(da * 180.0 / teeth * 0.5)
            pts.append((cx + rr * math.cos(aa), cy + rr * math.sin(aa)))
    d = 'M' + ' L'.join('%s,%s' % (n(x), n(y)) for x, y in pts) + 'Z'
    return '<path d="%s" fill="%s" stroke="%s" stroke-width="3" stroke-linejoin="round"/><circle cx="%s" cy="%s" r="%s" fill="#1d2327" stroke="%s" stroke-width="3"/>' % (d, fill, stroke, n(cx), n(cy), n(r * hole), stroke)

def gauge(cx, cy, r, val, label):
    o = '<circle cx="%s" cy="%s" r="%s" fill="url(#brass)" stroke="%s" stroke-width="3"/><circle cx="%s" cy="%s" r="%s" fill="#f4ead0" stroke="#5a3f10" stroke-width="2"/>' % (n(cx), n(cy), n(r), BRD, n(cx), n(cy), n(r - 6))
    o += '<path d="%s" stroke="#3a2a14" stroke-width="2" fill="none"/>' % arc(cx, cy, r - 12, 200, 340)
    o += '<path d="%s" stroke="#c0392b" stroke-width="4" fill="none"/>' % arc(cx, cy, r - 12, 310, 340)
    for k in range(7):
        a = math.radians(200 + k * 140 / 6.0)
        o += '<path d="M%s,%s L%s,%s" stroke="#3a2a14" stroke-width="2"/>' % (n(cx + (r - 12) * math.cos(a)), n(cy + (r - 12) * math.sin(a)), n(cx + (r - 18) * math.cos(a)), n(cy + (r - 18) * math.sin(a)))
    a = math.radians(200 + 140 * val)
    o += '<path d="M%s,%s L%s,%s" stroke="#1a1006" stroke-width="3" stroke-linecap="round"/><circle cx="%s" cy="%s" r="4" fill="#1a1006"/>' % (n(cx), n(cy + 8), n(cx + (r - 14) * math.cos(a)), n(cy + 8 + (r - 14) * math.sin(a)), n(cx), n(cy + 8))
    o += '<text x="%s" y="%s" font-size="11" font-weight="700" fill="#3a2a14" text-anchor="middle">%s</text>' % (n(cx), n(cy + r - 14), label)
    return o

class Steam(Theme):
    ink = '#2b2010'; dim = '#e9d9ae'; btn_fill = '#3b2a1a'; btn_stroke = BRS; btn_text = '#f6e7c1'
    accent = BRS; on_text = '#1a1006'; knob_fill = BRS; knob_stroke = BRD; track = '#1a1006'
    meter_off = '#4a3a24'; pad_off = '#3b2a1a'; wave_bg = '#0d1114'; colA = '#4ec9d4'; colB = '#ff8f3d'; btn_r = 4
    def shape(s, x, y, w, h, fill, stroke, sw=None, r=None, extra=''):
        o = '<rect x="%s" y="%s" width="%s" height="%s" rx="4" fill="%s" stroke="%s" stroke-width="%s"/>' % (n(x), n(y), n(w), n(h), fill, stroke, n(sw or 2.5))
        if w > 40 and h > 22:
            for rx_, ry_ in ((x + 5, y + 5), (x + w - 5, y + 5), (x + 5, y + h - 5), (x + w - 5, y + h - 5)):
                o += '<circle cx="%s" cy="%s" r="1.8" fill="%s"/>' % (n(rx_), n(ry_), stroke)
        return o
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        ang = 135 + 270 * val
        o = '<circle cx="%s" cy="%s" r="%s" fill="url(#brass)" stroke="%s" stroke-width="2.5"/>' % (n(cx), n(cy), n(r), BRD)
        for k in range(16):
            a = math.radians(k * 22.5)
            o += '<path d="M%s,%s L%s,%s" stroke="%s" stroke-width="2"/>' % (n(cx + (r - 1) * math.cos(a)), n(cy + (r - 1) * math.sin(a)), n(cx + (r - 5) * math.cos(a)), n(cy + (r - 5) * math.sin(a)), BRD)
        o += '<circle cx="%s" cy="%s" r="%s" fill="#3b2a1a"/><path d="M%s,%s L%s,%s" stroke="#ffe9a8" stroke-width="3.5" stroke-linecap="round"/>' % (n(cx), n(cy), n(r * .55), n(cx), n(cy), n(cx + (r * .5) * math.cos(math.radians(ang))), n(cy + (r * .5) * math.sin(math.radians(ang))))
        o += '<path d="M%s,%s L%s,%s" stroke="#1a1006" stroke-width="3" stroke-linecap="round"/>' % (n(cx + (r - 4) * math.cos(math.radians(ang))), n(cy + (r - 4) * math.sin(math.radians(ang))), n(cx + (r * .6) * math.cos(math.radians(ang))), n(cy + (r * .6) * math.sin(math.radians(ang))))
        if label:
            o += s.text(cx, cy + r + 13, label, 11, s.dim, weight='700')
        return o
T = Steam(); DK = Steam(ink='#f1e3bb', dim='#cdbb8c')
LT = Steam(dim='#4a3714')   # labels on bone
D.d('<linearGradient id="brass" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffe7a4"/><stop offset=".45" stop-color="#d4a443"/><stop offset="1" stop-color="#7a5416"/></linearGradient>')
D.d('<radialGradient id="aged" cx="50%" cy="35%" r="78%"><stop offset="0" stop-color="#f7ecc9"/><stop offset=".65" stop-color="#e0cfa4"/><stop offset="1" stop-color="#a99870"/></radialGradient>')
D.d('<radialGradient id="soc" cx="50%" cy="45%" r="60%"><stop offset="0" stop-color="#1b2a30"/><stop offset="1" stop-color="#05090b"/></radialGradient>')
D.d('<linearGradient id="pipe" x1="0" x2="1"><stop offset="0" stop-color="#7a5416"/><stop offset=".35" stop-color="#ffe7a4"/><stop offset="1" stop-color="#7a5416"/></linearGradient>')
# skull
left = [(800, 24), (590, 32), (410, 82), (285, 175), (220, 300), (205, 420), (228, 520), (205, 595), (262, 670), (318, 740), (335, 840), (380, 930), (450, 1000), (600, 1048), (800, 1062)]
pts = sym(left, W)
# ear horns
for sx in (1, -1):
    cx = 800 - sx * 700
    d = 'M%s,300 L%s,250 L%s,700 L%s,650 Z' % (n(cx + sx * 100), n(cx - sx * 85), n(cx - sx * 85), n(cx + sx * 100))
    D.add('<path d="%s" fill="url(#brass)" stroke="%s" stroke-width="4"/><ellipse cx="%s" cy="475" rx="26" ry="225" fill="none" stroke="%s" stroke-width="3" opacity="0"/>' % (d, BRD, n(cx - sx * 85), BRD))
    D.add('<ellipse cx="%s" cy="475" rx="22" ry="225" fill="#1a1006" stroke="%s" stroke-width="4"/>' % (n(cx - sx * 85), BRD))
    D.add('<path d="M%s,320 L%s,650" stroke="#fff" stroke-opacity=".18" stroke-width="12"/>' % (n(cx + sx * 20), n(cx + sx * 20)))
D.d('<mask id="cut"><rect width="4000" height="4000" fill="#fff"/><path d="M800,470 C735,490 715,560 722,610 C740,650 770,650 800,625 C830,650 860,650 878,610 C885,560 865,490 800,470Z" fill="#000"/></mask>')
D.add('<path d="%s" fill="url(#aged)" stroke="#6e5f3c" stroke-width="5" mask="url(#cut)"/>' % smooth(pts))
# brass cranial plate, seams
D.add('<path d="M560,60 Q800,10 1040,60 L1000,240 Q800,270 600,240Z" fill="url(#brass)" stroke="%s" stroke-width="4" opacity=".0"/>' % BRD)
D.add('<path d="M260,230 Q800,60 1340,230" stroke="#6e5f3c" stroke-width="3" fill="none" stroke-dasharray="3 7"/>')
for k in range(14):
    x = 300 + k * 78
    D.add('<circle cx="%d" cy="%s" r="4" fill="%s"/>' % (x, n(205 - 125 * math.sin((x - 260) / 1080.0 * math.pi)), BRD))
# gauges on forehead centre (master L/R)
D.add(gauge(764, 130, 32, .74, 'L') + gauge(836, 130, 32, .68, 'R'))
# goggles
EY = 395
for cx, d, col in ((450, 'A', T.colA), (1150, 'B', T.colB)):
    D.add('<circle cx="%d" cy="%d" r="190" fill="url(#brass)" stroke="%s" stroke-width="5"/>' % (cx, EY, BRD))
    D.add(gear(cx, EY, 186, 36, 'none', BRD, hole=0.01, rot=0 if d == 'A' else 5))
    D.add('<circle cx="%d" cy="%d" r="160" fill="#3b2a1a" stroke="%s" stroke-width="4"/>' % (cx, EY, BRD))
    D.add('<path d="%s" stroke="%s" stroke-width="8" fill="none" stroke-linecap="round"/>' % (arc(cx, EY, 150, 110, 300 if d == 'A' else 240), col))
    ma = 235 if d == 'A' else 305
    D.add('<circle cx="%s" cy="%s" r="12" fill="#ffe7a4" stroke="#1a1006" stroke-width="3"/>' % (n(cx + 150 * math.cos(math.radians(ma))), n(EY + 150 * math.sin(math.radians(ma)))))
    D.add('<circle cx="%d" cy="%d" r="132" fill="url(#soc)" stroke="#000" stroke-width="3"/><clipPath id="g%s"><circle cx="%d" cy="%d" r="131"/></clipPath>' % (cx, EY, d, cx, EY))
    lanes = [('#ff6a3d', 'LOW', 1), ('#ffd24a', 'MID', 7), ('#6fd0ff', 'HIGH', 13)]
    g = '<g clip-path="url(#g%s)">' % d
    for k, (lc, ln, sd) in enumerate(lanes):
        y = EY - 55 + k * 38
        g += wave(cx - 131, y, 262, 34, DECK[d]['seed'] + sd, lc, '#fff', head=131, cues=DECK[d]['cues'][:2] if k == 0 else (), pitch=4, grid=(k == 0), labels=(k == 0))
    D.add(g + '</g>')
    D.add(T.text(cx, EY - 78, d, 26, col, weight='800') + T.text(cx, EY + 92, 'LOW · MID · HIGH lanes', 11, '#9fb3b8', weight='700') + T.text(cx, EY + 112, 'JOG = gear ring', 11, '#9fb3b8'))
    D.put('jog ' + d, ('', 380, 380), cx - 190, EY - 190)
D.put('wave', ('', 262, 114), 319, EY - 55)
# nose valve
D.add('<path d="M800,470 C735,490 715,560 722,610 C740,650 770,650 800,625 C830,650 860,650 878,610 C885,560 865,490 800,470Z" fill="none" stroke="#6e5f3c" stroke-width="4"/>')
# teeth: upper = pads, lower = loop + keyshift
tw, pitch, tx0 = 54, 58.75, 330
loA = ['IN', 'OUT', '◂', '4', '▸', 'KEY−', 'KEY+', '±8%']
for i in range(16):
    d, j = ('A', i) if i < 8 else ('B', i - 8)
    x = tx0 + i * pitch
    lit = j in DECK[d]['lit']
    D.add('<path d="%s" fill="%s" stroke="#6e5f3c" stroke-width="2.5"/>' % (tooth(x, 746, tw, 80, True), HC[j] if lit else '#f8efd4'))
    D.add(T.text(x + 27, 796, str(j + 1), 17, '#1a1006', weight='800', halo=None))
    if not lit: D.add('<circle cx="%s" cy="812" r="5" fill="%s"/>' % (n(x + 27), HC[j]))
    D.add(tooth_btn(T, x, 836, tw, 62, loA[j], j == 1 and d == 'A', BRS, False, 12, '#3b2a1a', BRS, '#f6e7c1', '#1a1006'))
D.names['pads A'] = (tx0, 746, 8 * pitch, 80); D.names['pads B'] = (tx0 + 8 * pitch, 746, 8 * pitch, 80)
for dd in 'AB':
    x0 = tx0 + (0 if dd == 'A' else 8 * pitch)
    D.names['loop ' + dd] = (x0, 836, 5 * pitch, 62); D.names['keyshift ' + dd] = (x0 + 5 * pitch, 836, 2 * pitch, 62)
# controls on bone / brass
both(D, LT, 'info', b_info, 420, 88, w=285)
both(D, LT, 'overview', b_overview, 425, 178)
both(D, LT, 'transport', b_transport, 262, 598)
both(D, LT, 'padmodes', b_padmodes, 480, 606)
both(D, LT, 'toggles', b_toggles, 262, 664, orient='row')
both(D, T, 'fx', b_fx, 30, 316)
both(D, T, 'tempo', b_tempo, 100, 430, h=120)
D.put('mic', b_mic(LT), 710, 330)
D.put('rec', b_rec(T), 761, 400)
# workbench: mixer, master vertebrae, browser, misc
D.add(grip(800, 1088, '#e9d9ae'))
D.add('<rect x="600" y="1075" width="400" height="500" rx="20" fill="#2a3338" stroke="#4a5d66" stroke-width="3"/>')
for i in range(3):
    y = 1090 + i * 100
    D.add('<rect x="710" y="%d" width="180" height="84" rx="26" fill="url(#aged)" stroke="#6e5f3c" stroke-width="4"/><ellipse cx="800" cy="%d" rx="18" ry="8" fill="#a99870" opacity=".6"/>' % (y, y + 42))
D.add(LT.knob(800, 1128, 24, .72, 'MASTER', BRS) + LT.knob(800, 1228, 20, .5, 'HP LEVEL', BRS) + LT.knob(800, 1328, 20, .35, 'HP MIX', BRS))
D.names['master'] = (770, 1100, 60, 270)
D.put('ch1', b_strip(DK, 1, T.colA, 70, 54), 612, 1090)
D.put('ch2', b_strip(DK, 2, T.colB, 70, 54), 892, 1090)
D.put('xfader', b_xfader(DK, w=360), 620, 1510)
D.add('<rect x="18" y="1098" width="564" height="464" rx="26" fill="#2a3338" stroke="%s" stroke-width="5"/>' % BRS)
D.put('browser', b_browser(DK, BRS, 540, 440, 'LEDGER  ·  TRACK BROWSER', nrows=8), 30, 1110)
D.put('sampler', b_sampler(DK), 1070, 1120)
D.put('appbtns', b_appbtns(DK), 1070, 1250)
D.add(T.text(1070, 1330, 'sampler pads · brass crank', 11, '#cdbb8c', 'start'))
D.put('status', b_status(DK), 540, 1610)
D.save(os.path.dirname(__file__))
