import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1625
CX = 845
D = Doc(W, H, '#000'); D.cx = CX
BR, PARCH, RED, WOOD = '#d9a441', '#f3e3bd', '#c8402e', '#3a2616'

class Pirate(Theme):
    ink = PARCH; dim = '#cdbf96'; btn_fill = WOOD; btn_stroke = BR; btn_text = PARCH
    accent = BR; on_text = '#1a0d04'; knob_fill = BR; knob_stroke = '#8a6420'; track = '#1a0d04'
    meter_off = '#33291f'; pad_off = '#2a1d12'; wave_bg = '#071420'; colA = '#4fd1c5'; colB = '#ff7a52'; btn_r = 8
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        ang = 135 + 270 * val
        o = '<circle cx="%s" cy="%s" r="%s" fill="url(#brass)" stroke="#5c3f10" stroke-width="2"/>' % (n(cx), n(cy), n(r))
        o += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="#5c3f10" stroke-width="1.5" stroke-dasharray="2 3"/>' % (n(cx), n(cy), n(r - 5))
        o += '<path d="M%s,%s L%s,%s" stroke="#1a0d04" stroke-width="4" stroke-linecap="round"/>' % (n(cx), n(cy), n(cx + (r - 3) * math.cos(math.radians(ang))), n(cy + (r - 3) * math.sin(math.radians(ang))))
        o += '<circle cx="%s" cy="%s" r="4" fill="%s"/>' % (n(cx), n(cy), col or RED)
        if label:
            o += s.text(cx, cy + r + 14, label, 11, s.dim, weight='700', halo='#0a1a26')
        return o
T = Pirate(); DK = Pirate(btn_fill='#2a1d12')
D.d('<linearGradient id="brass" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffe29a"/><stop offset=".5" stop-color="#d9a441"/><stop offset="1" stop-color="#8a6420"/></linearGradient>')
D.d('<linearGradient id="wood" x1="0" x2="1"><stop offset="0" stop-color="#5a3a20"/><stop offset=".5" stop-color="#8a5a30"/><stop offset="1" stop-color="#3a2210"/></linearGradient>')
D.d('<linearGradient id="bonec" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fffaf0"/><stop offset="1" stop-color="#cfc4ab"/></linearGradient>')
D.d('<radialGradient id="skullg" cx="50%" cy="35%" r="75%"><stop offset="0" stop-color="#fffaf0"/><stop offset=".7" stop-color="#ece3cf"/><stop offset="1" stop-color="#b9ad92"/></radialGradient>')
D.d('<pattern id="weave" width="8" height="8" patternUnits="userSpaceOnUse"><path d="M0,0 H8 M0,4 H8" stroke="#ffffff" stroke-opacity=".035"/><path d="M4,0 V8" stroke="#000" stroke-opacity=".12"/></pattern>')
# sea backdrop
# flag cloth with wavy edges
top = ' '.join('L%d,%s' % (x, n(34 + 16 * math.sin(x / 120.0))) for x in range(110, 1581, 30))
rt = ' L1590,%s' % 160 + ' L1560,300 L1592,430 L1562,560 L1590,690 L1560,820 L1592,950 L1562,1080 L1590,1200'
bot = ' '.join('L%d,%s' % (x, n(1290 + 16 * math.sin(x / 120.0 + 1))) for x in range(1580, 109, -30))
cloth = 'M110,34 ' + top + rt + ' ' + bot + ' Z'
# patches (sailcloth) behind controls
def patch(x, y, w, h, tilt=0):
    pts_ = []
    for k in range(0, int(w), 26):
        pts_.append((x + k, y + (4 if (k // 26) % 2 else -3)))
    for k in range(0, int(h), 26):
        pts_.append((x + w + (3 if (k // 26) % 2 else -4), y + k))
    for k in range(int(w), 0, -26):
        pts_.append((x + k, y + h + (3 if (k // 26) % 2 else -4)))
    for k in range(int(h), 0, -26):
        pts_.append((x + (-3 if (k // 26) % 2 else 4), y + k))
    d_ = 'M' + ' L'.join('%s,%s' % (n(a), n(b)) for a, b in pts_) + 'Z'
    D.add('<g transform="rotate(%s %s %s)"><path d="%s" fill="#232c42" stroke="#5b6a8c" stroke-width="3" stroke-linejoin="round"/><rect x="%d" y="%d" width="%d" height="%d" rx="9" fill="none" stroke="%s" stroke-width="2" stroke-dasharray="9 7"/>%s</g>' % (tilt, x + w / 2, y + h / 2, d_, x + 10, y + 10, w - 20, h - 20, BR, grip(x + w / 2, y + 2, BR)))
patch(122, 58, 350, 400, -0.6); patch(1218, 58, 350, 400, 0.6); patch(122, 482, 350, 380, 0.5); patch(1218, 482, 350, 380, -0.5)
# bones (crossed) - drawn before skull
def bone(x0, y0, x1, y1):
    a = math.degrees(math.atan2(y1 - y0, x1 - x0)); L = math.hypot(x1 - x0, y1 - y0)
    return '<g transform="translate(%s,%s) rotate(%s)"><rect x="0" y="-15" width="%s" height="30" rx="12" fill="url(#bonec)" stroke="#6b5d44" stroke-width="3"/>' % (n(x0), n(y0), n(a), n(L)) + \
        ''.join('<circle cx="%s" cy="%s" r="22" fill="url(#bonec)" stroke="#6b5d44" stroke-width="3"/>' % (n(cx), n(cy)) for cx in (0, L) for cy in (-16, 16)) + '</g>'
D.add(bone(400, 1250, 1290, 960)); D.add(bone(400, 960, 1290, 1250))
# skull
left = [(845, 60), (660, 70), (520, 130), (440, 230), (410, 350), (420, 470), (440, 560), (430, 650), (450, 740), (490, 800), (560, 850), (700, 880), (845, 888)]
pts = sym(left, 2 * CX)
D.d('<mask id="cut"><rect width="4000" height="4000" fill="#fff"/><path d="M845,520 L815,590 L875,590Z" fill="#000"/></mask>')
D.add('<path d="%s" fill="url(#skullg)" stroke="#5b5038" stroke-width="5" mask="url(#cut)"/>' % smooth(pts))
# bandana
D.add('<path d="M425,150 Q845,60 1265,150 L1275,235 Q845,160 415,235 Z" fill="%s" stroke="#6e1a10" stroke-width="4"/>' % RED)
for k in range(16):
    D.add('<circle cx="%d" cy="%d" r="6" fill="#ffd9c8" opacity=".75"/>' % (460 + k * 50, 190 + 18 * math.sin(k / 15.0 * math.pi) * -1 - 8 + (k % 2) * 26))
D.add('<path d="M1262,190 l60,-26 l-14,40 l50,24 l-62,6 z" fill="%s" stroke="#6e1a10" stroke-width="3"/>' % RED)
# eyes: brass portholes
for cx, d, col in ((CX - 210, 'A', T.colA), (CX + 210, 'B', T.colB)):
    cy = 345
    D.add('<circle cx="%d" cy="%d" r="156" fill="url(#brass)" stroke="#5c3f10" stroke-width="4"/>' % (cx, cy))
    D.add('<circle cx="%d" cy="%d" r="132" fill="#2a1d12" stroke="#5c3f10" stroke-width="3"/>' % (cx, cy))
    D.add('<path d="%s" stroke="%s" stroke-width="7" fill="none" stroke-linecap="round"/>' % (arc(cx, cy, 122, 120, 300 if d == 'A' else 250), col))
    for k in range(24):
        a = math.radians(k * 15)
        D.add('<circle cx="%s" cy="%s" r="4" fill="#5c3f10"/>' % (n(cx + 144 * math.cos(a)), n(cy + 144 * math.sin(a))))
    ma = 230 if d == 'A' else 310
    D.add('<circle cx="%s" cy="%s" r="11" fill="%s" stroke="#fff" stroke-width="2"/>' % (n(cx + 122 * math.cos(math.radians(ma))), n(cy + 122 * math.sin(math.radians(ma))), RED))
    D.add('<circle cx="%d" cy="%d" r="108" fill="#071420" stroke="#000" stroke-width="2"/><clipPath id="p%s"><circle cx="%d" cy="%d" r="107"/></clipPath>' % (cx, cy, d, cx, cy))
    D.add('<g clip-path="url(#p%s)">' % d + wave(cx - 107, cy - 42, 214, 84, DECK[d]['seed'], col, '#fff', head=107, cues=DECK[d]['cues'][:3], pitch=4) + '</g>')
    D.add(T.text(cx, cy - 56, d, 26, col, weight='800') + T.text(cx, cy + 82, 'JOG', 11, '#8fb3c4', weight='700'))
    D.put('jog ' + d, ('', 312, 312), cx - 156, cy - 156)
D.put('wave', ('', 214, 84), CX - 317, 303)
D.add('<path d="M%d,345 Q%d,368 %d,345" stroke="#c9b27a" stroke-width="8" fill="none" stroke-linecap="round"/><path d="M%d,345 Q%d,368 %d,345" stroke="#7a6840" stroke-width="2" stroke-dasharray="5 5" fill="none"/>' % (CX - 54, CX, CX + 54, CX - 54, CX, CX + 54))
D.add('<path d="M%d,520 L%d,590 L%d,590Z" fill="none" stroke="#5b5038" stroke-width="3"/>' % (CX, CX - 30, CX + 30))
# teeth (16 pads on upper teeth), gold tooth
tw, pitch, tx0 = 44, 47, 469
for i in range(16):
    d, j = ('A', i) if i < 8 else ('B', i - 8)
    x = tx0 + i * pitch
    lit = j in DECK[d]['lit']
    D.add('<path d="%s" fill="%s" stroke="#5b5038" stroke-width="2.5"/>' % (tooth(x, 652, tw, 82, True, 12), HC[j] if lit else '#fffaf0'))
    D.add(T.text(x + tw / 2, 702, str(j + 1), 17, '#1a0d04' if lit else '#4a3f2a', weight='800', halo=None))
    if not lit: D.add('<circle cx="%s" cy="%d" r="5" fill="%s"/>' % (n(x + tw / 2), 720, HC[j]))
D.names['pads A'] = (tx0, 652, 8 * pitch, 82); D.names['pads B'] = (tx0 + 8 * pitch, 652, 8 * pitch, 82)
# jaw: crossfader along a curved rail
D.add('<path d="M560,806 Q845,880 1130,806" stroke="#3a2616" stroke-width="16" fill="none" stroke-linecap="round"/><path d="M560,806 Q845,880 1130,806" stroke="%s" stroke-width="5" fill="none" stroke-linecap="round"/>' % BR)
cxp, cyp = 780, 842
D.add('<g transform="rotate(8 %d %d)"><rect x="%d" y="%d" width="40" height="38" rx="8" fill="url(#brass)" stroke="#5c3f10" stroke-width="3"/><path d="M%d,%d v26" stroke="#1a0d04" stroke-width="4"/></g>' % (cxp, cyp, cxp - 20, cyp - 19, cxp, cyp - 13))
D.add(T.text(520, 800, 'A', 15, T.colA, weight='800', halo='#000') + T.text(1170, 800, 'B', 15, T.colB, weight='800', halo='#000') + T.text(CX, 876, 'CROSSFADER — jawbone rail', 11, '#4a3f2a', weight='700'))
D.names['xfader'] = (560, 800, 570, 80)
# left/right patches: deck controls
both(D, T, 'info', b_info, 135, 70, w=320)
both(D, T, 'overview', b_overview, 140, 164)
both(D, T, 'transport', b_transport, 140, 214)
both(D, T, 'keyshift', b_keyshift, 140, 270)
both(D, T, 'toggles', b_toggles, 140, 312, orient='grid')
both(D, T, 'loop', b_loop, 140, 388, orient='grid')
both(D, T, 'padmodes', b_padmodes, 282, 394, orient='grid')
both(D, T, 'fx', b_fx, 140, 500)
both(D, T, 'tempo', b_tempo, 150, 610, h=150)
# bandana master, cheeks
D.put('master', b_master(DK), CX - 122, 112)
D.put('sampler', b_sampler(T), 478, 512)
D.put('mic', b_mic(T), 1010, 512)
D.put('rec', b_rec(T), 1010, 570)
D.put('appbtns', b_appbtns(T), 1100, 570) if False else None
D.put('appbtns', b_appbtns(T), 1350, 905)
# bone mixer: knobs along bone ends, faders slide along bones
sl = (1250 - 960) / 890.0
for ch, y0, sgn, col in ((1, 1250, -1, T.colA), (2, 960, 1, T.colB)):
    lab = ['TRIM', 'HIGH', 'MID', 'LOW', 'FILTER']
    for i in range(5):
        x = 455 + i * 64
        y = y0 + sgn * (x - 400) * sl
        D.add(T.knob(x, y, 18, [.5, .62, .45, .6, .5][i], lab[i], col))
    D.names['ch%d' % ch] = (437, y0 + sgn * (455 - 400) * sl - 18, 330, 70)
    xs0, xs1 = 960, 1210
    ys0, ys1 = y0 + sgn * (xs0 - 400) * sl, y0 + sgn * (xs1 - 400) * sl
    D.add('<path d="M%d,%s L%d,%s" stroke="#1a0d04" stroke-width="10" stroke-linecap="round"/><path d="M%d,%s L%d,%s" stroke="%s" stroke-width="3" stroke-linecap="round"/>' % (xs0, n(ys0), xs1, n(ys1), xs0, n(ys0), xs1, n(ys1), col))
    t = .78 if ch == 1 else .66
    fx, fy = xs0 + (xs1 - xs0) * t, ys0 + (ys1 - ys0) * t
    ang = math.degrees(math.atan2(ys1 - ys0, xs1 - xs0))
    D.add('<g transform="translate(%s,%s) rotate(%s)"><rect x="-17" y="-20" width="34" height="40" rx="7" fill="url(#brass)" stroke="#5c3f10" stroke-width="3"/><path d="M0,-13 v26" stroke="#1a0d04" stroke-width="4"/></g>' % (n(fx), n(fy), n(ang)))
    D.add(T.meter(1226, ys1 - 34 if ch == 1 else ys1 - 14, 70, 9, .74 if ch == 1 else .6, True, 12).replace('rx="1"', 'rx="1"'))
    D.add(T.text(xs0 - 44, ys0 + 5, 'CH %d' % ch, 14, col, 'middle', '800', halo='#0a1a26'))
    D.add(T.btn(1296 if ch == 1 else 1296, (ys1 - 14) if ch == 1 else ys1 - 14, 50, 30, 'CUE', ch == 2, col, 12) if False else '')
D.add(T.btn(1300, 936, 50, 30, 'CUE', False, T.colA, 12)); D.add(T.btn(1300, 1230, 50, 30, 'CUE', True, T.colB, 12))
# map window
mp = 'M310,1320 Q330,1310 360,1322 L1280,1316 Q1300,1330 1292,1350 L1300,1540 Q1280,1556 1262,1544 L360,1548 Q320,1556 312,1540 Q326,1500 312,1460 Q326,1400 312,1380Z'
D.add('<path d="%s" fill="#e8d4a2" stroke="#7a5f2c" stroke-width="4"/>' % mp)
D.put('browser', b_browser(DK, BR, 960, 215, 'TREASURE MAP  ·  TRACK BROWSER', nrows=3), 320, 1328)
D.put('status', b_status(DK), 540, 1578)
D.save(os.path.dirname(__file__))
