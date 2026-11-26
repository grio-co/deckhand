import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1560
D = Doc(W, H, '#090818')
CYN, PNK, ICE = '#5cf2ff', '#ff7ae0', '#d8f4ff'

def cham(x, y, w, h, c):
    return 'M%s,%s h%s l%s,%s v%s l-%s,%s h-%s l-%s,-%s v-%s z' % (n(x + c), n(y), n(w - 2 * c), n(c), n(c), n(h - 2 * c), n(c), n(c), n(w - 2 * c), n(c), n(c), n(h - 2 * c))

class Crystal(Theme):
    ink = '#f2fbff'; dim = '#bfe3f5'; btn_fill = '#0c1b3a'; btn_stroke = '#b8ecff'; btn_text = '#f2fbff'
    accent = CYN; on_text = '#031018'; knob_fill = '#0c1b3a'; knob_stroke = '#b8ecff'; track = '#050b1c'
    meter_off = '#223457'; pad_off = '#0c1b3a'; wave_bg = '#050b1c'; colA = CYN; colB = PNK; halo = '#06102a'
    def shape(s, x, y, w, h, fill, stroke, sw=None, r=None, extra=''):
        o = '<path d="%s" fill="%s" fill-opacity=".92" stroke="%s" stroke-width="%s"/>' % (cham(x, y, w, h, min(7, h / 4)), fill, stroke, n(sw or 1.8))
        o += '<path d="M%s,%s h%s" stroke="#fff" stroke-opacity=".55" stroke-width="1.5"/>' % (n(x + 8), n(y + 2.5), n(max(0, w - 16)))
        return o
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        col = col or s.accent
        ang = 135 + 270 * val
        oct_ = ' '.join('%s,%s' % (n(cx + r * math.cos(math.radians(22.5 + k * 45))), n(cy + r * math.sin(math.radians(22.5 + k * 45)))) for k in range(8))
        oct2 = ' '.join('%s,%s' % (n(cx + r * .6 * math.cos(math.radians(k * 45))), n(cy + r * .6 * math.sin(math.radians(k * 45)))) for k in range(8))
        o = '<polygon points="%s" fill="#12285a" stroke="#b8ecff" stroke-width="2"/><polygon points="%s" fill="%s" fill-opacity=".35" stroke="#e8fbff" stroke-width="1.5"/>' % (oct_, oct2, col)
        o += '<path d="%s" stroke="%s" stroke-width="3" fill="none" stroke-linecap="round"/>' % (arc(cx, cy, r + 4, 135, ang), col)
        o += '<path d="M%s,%s L%s,%s" stroke="#fff" stroke-width="3" stroke-linecap="round"/>' % (n(cx), n(cy), n(cx + (r - 3) * math.cos(math.radians(ang))), n(cy + (r - 3) * math.sin(math.radians(ang))))
        if label:
            o += s.text(cx, cy + r + 15, label, 11, s.dim, weight='600', halo=None)
        return o
T = Crystal()
D.d('<filter id="glow" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="3.5" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>')
D.d('<filter id="soft" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="40"/></filter>')
D.d('<radialGradient id="core" cx="50%" cy="42%" r="55%"><stop offset="0" stop-color="#ffffff" stop-opacity=".95"/><stop offset=".3" stop-color="#8fe6ff" stop-opacity=".55"/><stop offset="1" stop-color="#6a5cff" stop-opacity="0"/></radialGradient>')
D.d('<linearGradient id="f1" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#d9f7ff" stop-opacity=".55"/><stop offset="1" stop-color="#6aa0ff" stop-opacity=".15"/></linearGradient>')
D.d('<linearGradient id="f2" x1="1" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#c3a8ff" stop-opacity=".45"/><stop offset="1" stop-color="#40d8ff" stop-opacity=".1"/></linearGradient>')
D.d('<linearGradient id="beam" x1="0" x2="1"><stop offset="0" stop-color="#8fe6ff" stop-opacity="0"/><stop offset=".5" stop-color="#ffffff" stop-opacity=".55"/><stop offset="1" stop-color="#8fe6ff" stop-opacity="0"/></linearGradient>')
D.d('<radialGradient id="orb" cx="40%" cy="35%" r="75%"><stop offset="0" stop-color="#2a4a9a"/><stop offset=".6" stop-color="#0a1440"/><stop offset="1" stop-color="#03061a"/></radialGradient>')
D.d('<radialGradient id="bgg" cx="50%" cy="30%" r="80%"><stop offset="0" stop-color="#241a58"/><stop offset="1" stop-color="#05040f"/></radialGradient>')
D.add('<ellipse cx="800" cy="420" rx="520" ry="380" fill="#5a7bff" opacity=".35" filter="url(#soft)"/>')
left = [(800, 24), (590, 32), (410, 82), (285, 175), (220, 300), (205, 420), (228, 520), (205, 595), (262, 670), (318, 740), (335, 840), (380, 930), (450, 1010), (600, 1062), (800, 1075)]
pts = sym(left, W)
D.d('<mask id="cut"><rect width="4000" height="4000" fill="#fff"/><path d="M800,625 L748,712 L778,740 L800,722 L822,740 L852,712Z" fill="#000"/></mask>')
D.add('<path d="%s" fill="#6aa8ff" fill-opacity=".22" stroke="#e8fbff" stroke-width="4" filter="url(#glow)" mask="url(#cut)"/>' % smooth(pts, k=0.25))
D.add('<path d="%s" fill="url(#f1)"/>' % smooth(pts, k=0.25))
# facets
facets = [((800, 24), (590, 32), (800, 200)), ((800, 24), (1010, 32), (800, 200)), ((590, 32), (410, 82), (500, 250)), ((1010, 32), (1190, 82), (1100, 250)), ((410, 82), (285, 175), (330, 300)), ((1190, 82), (1315, 175), (1270, 300)),
          ((220, 300), (205, 420), (330, 380)), ((1380, 300), (1395, 420), (1270, 380)), ((205, 595), (262, 670), (330, 640)), ((1395, 595), (1338, 670), (1270, 640)), ((335, 840), (380, 930), (500, 900)), ((1265, 840), (1220, 930), (1100, 900)), ((450, 1010), (600, 1062), (700, 960)), ((1150, 1010), (1000, 1062), (900, 960))]
for i, f in enumerate(facets):
    D.add('<polygon points="%s" fill="url(#%s)" stroke="#e8fbff" stroke-opacity=".5" stroke-width="1.5"/>' % (' '.join('%d,%d' % p for p in f), 'f1' if i % 2 else 'f2'))
D.add('<ellipse cx="800" cy="440" rx="470" ry="300" fill="url(#core)" opacity=".5"/>')
# eyes: glass orbs with gem ring
EY = 440
for cx, d, col in ((450, 'A', CYN), (1150, 'B', PNK)):
    D.add('<circle cx="%d" cy="%d" r="176" fill="#dff6ff" fill-opacity=".25" stroke="#e8fbff" stroke-width="3"/>' % (cx, EY))
    D.add('<circle cx="%d" cy="%d" r="164" fill="url(#orb)" stroke="%s" stroke-width="3" filter="url(#glow)"/>' % (cx, EY, col))
    D.add('<path d="%s" stroke="%s" stroke-width="8" fill="none" stroke-linecap="round" filter="url(#glow)"/>' % (arc(cx, EY, 150, 100, 300 if d == 'A' else 250), col))
    for k in range(24):
        a = math.radians(k * 15)
        D.add('<circle cx="%s" cy="%s" r="3" fill="#e8fbff"/>' % (n(cx + 170 * math.cos(a)), n(EY + 170 * math.sin(a))))
    ma = 230 if d == 'A' else 315
    D.add('<circle cx="%s" cy="%s" r="13" fill="%s" stroke="#fff" stroke-width="3" filter="url(#glow)"/>' % (n(cx + 150 * math.cos(math.radians(ma))), n(EY + 150 * math.sin(math.radians(ma))), col))
    D.add('<ellipse cx="%d" cy="%d" rx="90" ry="40" transform="rotate(-25 %d %d)" fill="#fff" opacity=".12"/>' % (cx - 50, EY - 90, cx - 50, EY - 90))
    D.add(T.text(cx, EY - 96, d, 26, col, weight='800') + T.text(cx, EY + 112, 'JOG', 11, '#9fc4e6', weight='700'))
    D.put('jog ' + d, ('', 352, 352), cx - 176, EY - 176)
# waveform beam across both orbs and the nasal bridge
D.add('<rect x="300" y="%d" width="1000" height="140" fill="url(#beam)" opacity=".35"/>' % (EY - 70))
D.add('<clipPath id="bm"><rect x="310" y="%d" width="980" height="120"/></clipPath>' % (EY - 60))
D.add('<g clip-path="url(#bm)" filter="url(#glow)">' + wave(310, EY - 56, 490, 112, 11, CYN, '#ffffff', head=140, cues=[.08, .2, .31, .62, .8], pitch=3, grid=False) + wave(800, EY - 56, 490, 112, 29, PNK, '#ffffff', head=350, cues=[.1, .27, .5, .7], pitch=3, grid=False) + '</g>')
D.add('<path d="M450,%d V%d M1150,%d V%d" stroke="#fff" stroke-width="3" opacity=".9"/>' % (EY - 80, EY + 80, EY - 80, EY + 80))
D.put('wave', ('', 980, 120), 310, EY - 60)
# nose
D.add('<path d="M800,625 L748,712 L778,740 L800,722 L822,740 L852,712Z" fill="none" stroke="#e8fbff" stroke-width="2.5"/>')
# brow ridge strip: transport + toggles
D.add('<path d="M240,262 Q450,150 800,205 Q1150,150 1360,262" stroke="#e8fbff" stroke-width="3" fill="none" opacity=".7"/>')
both(D, T, 'transport', b_transport, 262, 190)
both(D, T, 'toggles', b_toggles, 480, 197, orient='row')
both(D, T, 'info', b_info, 440, 62, w=300)
both(D, T, 'overview', b_overview, 445, 148)
# temples tempo, cheeks fx, jaw pads
both(D, T, 'tempo', b_tempo, 218, 286, h=130)
both(D, T, 'fx', b_fx, 310, 632, orient='cols')
both(D, T, 'pads', b_pads, 338, 938, s=48)
both(D, T, 'padmodes', b_padmodes, 560, 950, orient='grid')
# teeth: upper loop/keyshift/range, lower sampler
tw, pitch, tx0 = 54, 58.75, 330
loA = ['IN', 'OUT', '◂', '4', '▸', 'KEY−', 'KEY+', '±8%']
for i in range(16):
    d, j = ('A', i) if i < 8 else ('B', i - 8)
    col = CYN if d == 'A' else PNK
    x = tx0 + i * pitch
    D.add(tooth_btn(T, x, 760, tw, 78, loA[j], j == 1 and d == 'A', col, True, 12, '#0c1b3a', '#d8f4ff', '#f2fbff', '#031018'))
    D.add(T.pad(x, 848, tw, 62, i % 8, i in (1, 4, 6, 9, 12), HC[i % 8], 'S%d' % (i + 1)))
for dd in 'AB':
    x0 = tx0 + (0 if dd == 'A' else 8 * pitch)
    D.names['loop ' + dd] = (x0, 760, 5 * pitch, 78); D.names['keyshift ' + dd] = (x0 + 5 * pitch, 760, 2 * pitch, 78)
D.names['sampler'] = (tx0, 848, 940, 62)
D.put('master', b_master(T), 678, 500)
D.put('mic', b_mic(T), 710, 292)
D.put('rec', b_rec(T), 761, 360) if False else None
# crystal spine = mixer rows
rows = [('TRIM', 0), ('HIGH', 1), ('MID', 2), ('LOW', 3), ('FILTER', 4)]
vals = [(.5, .5), (.62, .55), (.45, .5), (.6, .4), (.5, .5)]
for i, (lab, k) in enumerate(rows):
    y = 1030 + i * 62
    D.add('<polygon points="620,%d 690,%d 910,%d 980,%d 910,%d 690,%d" fill="#6aa8ff" fill-opacity=".25" stroke="#e8fbff" stroke-width="2.5"/>' % (y + 28, y, y, y + 28, y + 56, y + 56))
    D.add(T.knob(668, y + 28, 19, vals[i][0], None, CYN, bipolar=i > 0) + T.knob(932, y + 28, 19, vals[i][1], None, PNK, bipolar=i > 0) + T.text(800, y + 34, lab, 13, '#fff', weight='800', halo=None))
    D.add(T.text(700, y + 52, 'CH1', 11, '#9fc4e6', halo=None) + T.text(900, y + 52, 'CH2', 11, '#9fc4e6', halo=None)) if False else None
y = 1030 + 5 * 62
D.add('<polygon points="620,%d 690,%d 910,%d 980,%d 910,%d 690,%d" fill="#6aa8ff" fill-opacity=".25" stroke="#e8fbff" stroke-width="2.5"/>' % (y + 28, y, y, y + 28, y + 56, y + 56))
D.add(T.fader_h(702, y + 10, 196, 36, .46, '#fff') + T.text(700, y + 7, 'A', 12, CYN, weight='800', halo=None) + T.text(900, y + 7, 'B', 12, PNK, weight='800', halo=None) + T.text(800, y + 56, 'CROSSFADER', 11, '#bfe3f5', halo=None))
D.names['xfader'] = (702, y + 10, 196, 36)
y2 = y + 66
D.add('<polygon points="620,%d 690,%d 910,%d 980,%d 910,%d 690,%d" fill="#6aa8ff" fill-opacity=".25" stroke="#e8fbff" stroke-width="2.5"/>' % (y2 + 30, y2, y2, y2 + 30, y2 + 60, y2 + 60))
D.add(T.fader_h(620, y2 - 0, 0, 0, 0) if False else '')
D.add(T.btn(660, y2 + 14, 44, 30, 'CUE', False, CYN, 12) + T.btn(896, y2 + 14, 44, 30, 'CUE', True, PNK, 12))
D.add(T.fader_h(714, y2 + 10, 100, 40, .8, CYN) + T.fader_h(830, y2 + 10, 0.1, 40, .1, PNK) if False else '')
D.add(T.fader_h(712, y2 + 8, 76, 24, .8, CYN) + T.fader_h(812, y2 + 8, 76, 24, .7, PNK))
D.add(T.meter(712, y2 + 38, 76, 8, .75, True, 10) + T.meter(812, y2 + 38, 76, 8, .62, True, 10))
D.names['ch1'] = (660, y2, 130, 60); D.names['ch2'] = (810, y2, 130, 60)
# bottom-left browser shard, bottom-right misc
D.add('<polygon points="20,1150 160,1110 600,1130 620,1520 30,1540" fill="#6aa8ff" fill-opacity=".18" stroke="#e8fbff" stroke-width="3"/>')
D.put('browser', b_browser(T, CYN, 540, 380, 'SHARD  ·  TRACK BROWSER', nrows=7), 50, 1150)
D.put('sampler', b_sampler(T), 5000, 5000) if False else None
D.put('rec', b_rec(T), 1100, 1150)
D.put('appbtns', b_appbtns(T), 1100, 1210)
D.put('status', b_status(T), 540, 1535)
D.save(os.path.dirname(__file__))
