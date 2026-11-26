import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1760
D = Doc(W, H, '#17181c')
class Ivory(Theme):
    ink = '#2b261c'; dim = '#5a503c'; btn_fill = '#3b3529'; btn_stroke = '#cbbf9f'; btn_text = '#f6efdc'
    accent = '#e0902b'; on_text = '#1d1305'; knob_fill = '#3b3529'; knob_stroke = '#cbbf9f'; track = '#2a251c'
    meter_off = '#6b6350'; pad_off = '#f3ebd5'; wave_bg = '#0d0f13'; colA = '#1fb6e8'; colB = '#ef4f8f'
    tooth_up = True
    def pad(s, x, y, w, h, i, lit, col=None, label=None):
        col = col or HC[i % 8]
        o = '<path d="%s" fill="%s" stroke="%s" stroke-width="2.5"/>' % (tooth(x, y, w, h, s.tooth_up), col if lit else '#f6efdc', '#8f836a' if not lit else '#fff')
        if lit:
            o += '<path d="%s" fill="#fff" fill-opacity=".25"/>' % tooth(x + 4, y + 4, w * .4, h * .55, s.tooth_up, 6)
        else:
            cy = y + h - 12 if s.tooth_up else y + 12
            o += '<circle cx="%s" cy="%s" r="5" fill="%s"/>' % (n(x + w / 2), n(cy), col)
        o += s.text(x + w / 2, y + (h / 2 + 2 if s.tooth_up else h / 2 + 12), label or str(i + 1), 17, '#1d1305' if lit else '#3b3529', weight='700')
        return o
T = Ivory()
DK = Theme()
TA = Ivory(); 

# ---- backdrop: spine + ribcage window drawn first
D.d('<radialGradient id="bone" cx="50%" cy="38%" r="75%"><stop offset="0" stop-color="#fbf6e6"/><stop offset=".6" stop-color="#e8dec3"/><stop offset="1" stop-color="#bfb293"/></radialGradient>')
D.d('<radialGradient id="soc" cx="50%" cy="45%" r="60%"><stop offset="0" stop-color="#1c212c"/><stop offset="1" stop-color="#06070a"/></radialGradient>')
D.d('<linearGradient id="vb" x1="0" x2="0" y1="0" y2="1"><stop offset="0" stop-color="#efe6cd"/><stop offset="1" stop-color="#c9bc9c"/></linearGradient>')
# spine
D.add('<path d="M760,1110 V1380" stroke="#8a7e64" stroke-width="150"/>')
for i in range(3):
    y = 1128 + i * 66
    D.add('<path d="M480,%d Q600,%d 720,%d H880 Q1000,%d 1120,%d Q1120,%d 1000,%d H600 Q480,%d 480,%d Z" fill="url(#vb)" stroke="#8f836a" stroke-width="3"/>' % (y + 10, y - 6, y, y - 6, y + 10, y + 52, y + 52, y + 52, y + 10))
    D.add('<rect x="720" y="%d" width="160" height="54" rx="16" fill="url(#vb)" stroke="#8f836a" stroke-width="3"/>' % (y))
    D.add('<ellipse cx="800" cy="%d" rx="18" ry="8" fill="#a99d82" opacity=".6"/>' % (y + 27))
D.add('<g transform="translate(0,50)">')
# ribcage browser window
D.add('<path d="M800,1390 C420,1380 260,1470 250,1610 C250,1660 330,1680 450,1664 L1150,1664 C1270,1680 1350,1660 1350,1610 C1340,1470 1180,1380 800,1390Z" fill="#cbbf9f" stroke="#8f836a" stroke-width="4"/>')
for k in range(6):
    D.add('<path d="M800,%d C %d,%d 330,%d 270,%d" stroke="#e8dec3" stroke-width="6" fill="none" opacity=".7"/>' % (1395 + k * 3, 560 - k * 10, 1400 + k * 18, 1440 + k * 36, 1500 + k * 28))
    D.add('<path d="M800,%d C %d,%d 1270,%d 1330,%d" stroke="#e8dec3" stroke-width="6" fill="none" opacity=".7"/>' % (1395 + k * 3, 1040 + k * 10, 1400 + k * 18, 1440 + k * 36, 1500 + k * 28))
D.add('</g>')
D.put('browser', b_browser(DK, DK.accent, 760, 262, 'RIBCAGE  ·  TRACK BROWSER ', nrows=5), 420, 1446)
# ---- skull silhouette
left = [(800, 24), (590, 32), (410, 82), (285, 175), (220, 300), (205, 420), (228, 520), (205, 595), (262, 670), (318, 740), (335, 840), (360, 940), (420, 1030), (540, 1100), (800, 1122)]
pts = sym(left, W)
poly = sample(pts)
# ears (temporal bones)
for sx in (1, -1):
    cx = 800 - sx * 705
    D.add('<ellipse cx="%s" cy="470" rx="72" ry="150" fill="url(#bone)" stroke="#8f836a" stroke-width="4"/><ellipse cx="%s" cy="578" rx="22" ry="30" fill="#0b0c10" stroke="#8f836a" stroke-width="3"/>' % (cx + sx * 50, cx + sx * 40))
D.add('<path d="%s" fill="url(#bone)" stroke="#8f836a" stroke-width="5"/>' % smooth(pts))
# bone shading + sutures
D.add('<path d="M260,230 Q800,70 1340,230" stroke="#a99d82" stroke-width="3" fill="none" stroke-dasharray="14 6 4 6" opacity=".55"/>')
D.add('<path d="M800,26 V86" stroke="#a99d82" stroke-width="3" opacity=".5"/>')
D.add('<path d="M330,610 Q460,660 640,640 M1270,610 Q1140,660 960,640" stroke="#a99d82" stroke-width="12" fill="none" opacity=".35" stroke-linecap="round"/>')
D.add('<path d="M340,760 Q400,900 520,1010 M1260,760 Q1200,900 1080,1010" stroke="#a99d82" stroke-width="3" fill="none" opacity=".55"/>')
# eyes
EY = 395
for cx, d, col in ((455, 'A', T.colA), (1145, 'B', T.colB)):
    D.add('<circle cx="%d" cy="%d" r="183" fill="#b5a888"/><circle cx="%d" cy="%d" r="178" fill="url(#soc)" stroke="#7d725a" stroke-width="3"/>' % (cx, EY, cx, EY))
    # jog ring: annulus with progress arc and ticks
    D.add('<circle cx="%d" cy="%d" r="150" fill="none" stroke="#242831" stroke-width="36"/>' % (cx, EY))
    D.add('<path d="%s" stroke="%s" stroke-width="8" fill="none" stroke-linecap="round" opacity=".9"/>' % (arc(cx, EY, 161, 100, 290 if d == 'A' else 240), col))
    tk = ''.join('M%s,%s L%s,%s' % (n(cx + 134 * math.cos(math.radians(a))), n(EY + 134 * math.sin(math.radians(a))), n(cx + 142 * math.cos(math.radians(a))), n(EY + 142 * math.sin(math.radians(a)))) for a in range(0, 360, 10))
    D.add('<path d="%s" stroke="#6c7283" stroke-width="2"/>' % tk)
    ma = 235 if d == 'A' else 305
    mx, my = cx + 150 * math.cos(math.radians(ma)), EY + 150 * math.sin(math.radians(ma))
    D.add('<circle cx="%s" cy="%s" r="11" fill="%s" stroke="#fff" stroke-width="2"/>' % (n(mx), n(my), col))
    D.add('<circle cx="%d" cy="%d" r="128" fill="#07080b" stroke="#303541" stroke-width="2"/>' % (cx, EY))
    D.add(T.text(cx, 306, d, 34, col, weight='800', halo=None))
    D.add(T.text(cx, 506, 'JOG · drag ring to nudge', 11, '#7a8090'))
    D.put('jog ' + d, ('', 356, 356), cx - 178, EY - 178)
# waveform band across eyes and nasal bridge
bx0, bx1, by, bh = 330, 1270, 345, 100
D.add('<clipPath id="band"><rect x="%d" y="%d" width="%d" height="%d" rx="18"/></clipPath>' % (bx0, by, bx1 - bx0, bh))
D.add('<rect x="%d" y="%d" width="%d" height="%d" rx="18" fill="#0a0c10" stroke="#cbbf9f" stroke-width="4"/>' % (bx0, by, bx1 - bx0, bh))
D.add('<g clip-path="url(#band)">' + wave(bx0, by + 6, 470, bh - 12, 11, T.colA, '#e8fbff', head=455 - bx0, cues=[.08, .2, .31, .62, .8], pitch=4) +
      wave(800, by + 6, 470, bh - 12, 29, T.colB, '#fff0f6', head=1145 - 800, cues=[.1, .27, .5, .7], pitch=4) + '</g>')
D.add('<circle cx="455" cy="%d" r="9" fill="none" stroke="#fff" stroke-width="3"/><circle cx="1145" cy="%d" r="9" fill="none" stroke="#fff" stroke-width="3"/>' % (by + bh / 2, by + bh / 2))
D.put('wave', ('', bx1 - bx0, bh), bx0, by)
# nasal cavity
D.add('<path d="M800,452 C700,470 668,540 676,620 C690,668 740,676 800,640 C860,676 910,668 924,620 C932,540 900,470 800,452Z" fill="url(#soc)" stroke="#8f836a" stroke-width="3"/>')
D.add('<path d="M800,640 V560" stroke="#cbbf9f" stroke-width="8" opacity=".5"/>')
# ---- teeth rows
tw, pitch, tx0 = 54, 58.75, 330
for i in range(16):
    x = tx0 + i * pitch
    d = 'A' if i < 8 else 'B'
    j = i % 8 if i < 8 else i - 8
    T.tooth_up = True
    D.add(T.pad(x, 742, tw, 80, j, j in DECK[d]['lit'], HC[j]))
for i in range(16):
    x = tx0 + i * pitch
    T.tooth_up = False
    D.add(T.pad(x, 832, tw, 66, i % 8, i in (1, 4, 6, 9, 12), HC[i % 8], 'S%d' % (i + 1)))
T.tooth_up = True
D.put('pads A', ('', 460, 80), tx0, 742); D.put('pads B', ('', 460, 80), 800 + 4, 742); D.put('sampler', ('', 940, 66), tx0, 832)
# ---- controls on bone
def both(name, fn, x, y, **kw):
    for d, col in (('A', T.colA), ('B', T.colB)):
        D.put(name + ' ' + d, fn(T, d, col, **kw), x, y, mirror=(d == 'B'))
both('info', b_info, 420, 88, w=320)
both('overview', b_overview, 425, 178)
both('transport', b_transport, 262, 590)
both('keyshift', b_keyshift, 484, 598)
both('toggles', b_toggles, 262, 650, orient='grid')
both('loop', b_loop, 418, 650, orient='grid')
both('padmodes', b_padmodes, 558, 650, orient='grid')
both('tempo', b_tempo, 98, 335, h=140)
D.put('master', b_master(DK), 678, 482)
D.put('mic', b_mic(T), 710, 240)
D.put('rec', b_rec(T), 761, 128)
D.put('appbtns', b_appbtns(T), 710, 56)
# mixer on the mandible
D.put('ch1', b_strip_h(T, 1, T.colA), 505, 918)
D.put('ch2', b_strip_h(T, 2, T.colB), 505, 982)
D.put('xfader', b_xfader(T, w=420), 590, 1045)
# vertebrae = FX rack
for i in range(3):
    for d, col in (('A', T.colA), ('B', T.colB)):
        D.add('<g transform="translate(%s,%d)">%s</g>' % (n(524 if d == 'A' else 880), 1146 + i * 66, b_fxslot(T, d, col, i)[0].replace('<text', '<text')))
    D.add(T.text(800, 1161 + i * 66 + 14, 'FX %d' % (i + 1), 14, T.dim, weight='700'))
D.names['fx A'] = (524, 1146, 196, 28); D.names['fx B'] = (880, 1146, 196, 28)
D.put('status', b_status(DK), 540, 1728)
D.d('')
D.save(os.path.dirname(__file__))
