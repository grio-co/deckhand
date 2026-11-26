import sys, os, random
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1640
D = Doc(W, H, '#1b1109')
TQ, RD, YL, CRM = '#27c4b4', '#d6452e', '#f1b93b', '#fbe8c2'

class Wood(Theme):
    ink = CRM; dim = '#f0d8a8'; btn_fill = '#2f1c0d'; btn_stroke = '#e6b66a'; btn_text = CRM
    accent = TQ; on_text = '#1a0f06'; knob_fill = '#6b4220'; knob_stroke = '#2a1708'; track = '#1a0f06'
    meter_off = '#4a2f18'; pad_off = '#2f1c0d'; wave_bg = '#140c06'; colA = TQ; colB = '#f08a2e'; btn_r = 5; pad_r = 4
    def shape(s, x, y, w, h, fill, stroke, sw=None, r=None, extra=''):
        o = '<rect x="%s" y="%s" width="%s" height="%s" rx="4" fill="%s" stroke="%s" stroke-width="%s"/>' % (n(x), n(y), n(w), n(h), fill, stroke, n(sw or 2.5))
        o += '<path d="M%s,%s h%s" stroke="#000" stroke-opacity=".35" stroke-width="2"/>' % (n(x + 3), n(y + h - 2.5), n(w - 6))
        return o
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        ang = 135 + 270 * val
        o = '<circle cx="%s" cy="%s" r="%s" fill="url(#wood2)" stroke="#2a1708" stroke-width="3"/>' % (n(cx), n(cy), n(r))
        for k in (.8, .55):
            o += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="#2a1708" stroke-opacity=".6" stroke-width="2"/>' % (n(cx), n(cy), n(r * k))
        o += '<path d="%s" stroke="%s" stroke-width="3" fill="none" stroke-linecap="round"/>' % (arc(cx, cy, r + 4, 135, ang), col or TQ)
        o += '<path d="M%s,%s L%s,%s" stroke="%s" stroke-width="4" stroke-linecap="round"/>' % (n(cx), n(cy), n(cx + (r - 3) * math.cos(math.radians(ang))), n(cy + (r - 3) * math.sin(math.radians(ang))), CRM)
        if label:
            o += s.text(cx, cy + r + 15, label, 11, s.dim, weight='700', halo='')
        return o
    def pad(s, x, y, w, h, i, lit, col=None, label=None):
        col = col or HC[i % 8]
        o = s.shape(x, y, w, h, col if lit else '#3a2412', '#e6b66a', 3)
        o += '<path d="M%s,%s l%s,%s l%s,-%s" stroke="%s" stroke-width="2" fill="none" opacity=".6"/>' % (n(x + 6), n(y + h - 6), n(w / 2 - 6), n(-(h - 14)), n(w / 2 - 6), n(h - 14), '#000' if lit else col) if False else ''
        o += s.text(x + w / 2, y + h / 2 + 6, label or str(i + 1), 17, s.on_text if lit else col, weight='800', halo='')
        return o
T = Wood()
rr = random.Random(8)
D.d('<linearGradient id="wood" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#7a4a22"/><stop offset=".5" stop-color="#9a6130"/><stop offset="1" stop-color="#70431e"/></linearGradient>')
D.d('<radialGradient id="wood2" cx="40%" cy="35%" r="80%"><stop offset="0" stop-color="#a06a38"/><stop offset="1" stop-color="#5a3518"/></radialGradient>')
D.d('<radialGradient id="mouthg" cx="50%" cy="50%" r="70%"><stop offset="0" stop-color="#3a1f10"/><stop offset="1" stop-color="#120a05"/></radialGradient>')
D.d('<radialGradient id="eyeg" cx="50%" cy="50%" r="60%"><stop offset="0" stop-color="#1a0f08"/><stop offset="1" stop-color="#050302"/></radialGradient>')
D.d('<radialGradient id="bgw" cx="50%" cy="30%" r="80%"><stop offset="0" stop-color="#3a2412"/><stop offset="1" stop-color="#120a05"/></radialGradient>')
# feather crown
for k in range(15):
    a = -90 + (k - 7) * 11
    ln = 260 - abs(k - 7) * 10
    x2, y2 = 800 + ln * math.cos(math.radians(a)) * 1.3, 150 + ln * math.sin(math.radians(a))
    c = [RD, YL, TQ][k % 3]
    D.add('<ellipse cx="%s" cy="%s" rx="26" ry="%s" transform="rotate(%s %s %s)" fill="%s" stroke="#2a1708" stroke-width="3"/>' % (n((800 + x2) / 2), n((150 + y2) / 2 + 20), n(ln / 2 + 10), n(a + 90), n((800 + x2) / 2), n((150 + y2) / 2 + 20), c))
left = [(800, 120), (620, 130), (430, 190), (300, 300), (250, 450), (240, 600), (270, 760), (330, 920), (380, 1040), (470, 1130), (600, 1190), (800, 1212)]
pts = sym(left, W)
# ears
for sx in (1, -1):
    cx = 800 - sx * 650
    D.add('<circle cx="%d" cy="570" r="108" fill="url(#wood2)" stroke="#2a1708" stroke-width="5"/>' % cx)
    D.add('<path d="M%d,570 m-70,0 a70,70 0 1 1 70,70" stroke="#2a1708" stroke-width="3" fill="none" opacity=".5"/>' % cx)
    D.add('<path d="M%d,676 V700" stroke="#e6b66a" stroke-width="4"/>' % cx)
    D.add('<rect x="%d" y="700" width="220" height="130" rx="8" fill="url(#wood)" stroke="#2a1708" stroke-width="4"/>' % (cx - 110))
D.add('<path d="%s" fill="url(#wood)" stroke="#2a1708" stroke-width="6"/>' % smooth(pts))
# grain
for k in range(60):
    x = rr.randrange(260, 1340); y = rr.randrange(160, 1180)
    D.add('<path d="M%d,%d q%d,%d %d,%d" stroke="#3a220f" stroke-opacity=".28" stroke-width="2" fill="none"/>' % (x, y, rr.randrange(-8, 8), 40, rr.randrange(-6, 6), 90))
D.add('<clipPath id="mk"><path d="%s"/></clipPath>' % smooth(pts))
# carved zigzag border
bd = ''
for k in range(0, 40):
    pass
D.add('<path d="%s" fill="none" stroke="#2a1708" stroke-width="3" transform="translate(800 680) scale(.93) translate(-800 -680)" stroke-dasharray="14 8" opacity=".6"/>' % smooth(pts))
# forehead plaque
D.add('<path d="M380,170 L1220,170 L1250,330 L350,330Z" fill="#c78a4a" fill-opacity=".25" stroke="#2a1708" stroke-width="3"/>')
zz = 'M360,326 ' + ' '.join('l20,%d' % (-14 if k % 2 == 0 else 14) for k in range(42))
D.add('<path d="%s" stroke="%s" stroke-width="5" fill="none"/>' % (zz, RD))
# eyes: bead rings
EY = 480
for cx, d, col in ((470, 'A', T.colA), (1130, 'B', T.colB)):
    D.add('<circle cx="%d" cy="%d" r="164" fill="#2a1708"/>' % (cx, EY))
    for k in range(28):
        a = math.radians(k * 360 / 28.0)
        D.add('<circle cx="%s" cy="%s" r="9" fill="%s" stroke="#2a1708" stroke-width="2"/>' % (n(cx + 150 * math.cos(a)), n(EY + 150 * math.sin(a)), [RD, YL, TQ, CRM][k % 4]))
    D.add('<circle cx="%d" cy="%d" r="132" fill="url(#eyeg)" stroke="#e6b66a" stroke-width="4"/>' % (cx, EY))
    ma = 235 if d == 'A' else 305
    D.add('<circle cx="%s" cy="%s" r="16" fill="%s" stroke="#fff" stroke-width="3"/>' % (n(cx + 150 * math.cos(math.radians(ma))), n(EY + 150 * math.sin(math.radians(ma))), col))
    D.add(T.text(cx, EY - 84, d, 26, col, weight='800', halo='') + T.text(cx, EY + 112, 'JOG ring', 11, '#d9b98a', halo=''))
    D.put('jog ' + d, ('', 328, 328), cx - 164, EY - 164)
# shared-centreline band: A grows up, B grows down, across both eyes and the nose
by, bh = EY - 46, 92
D.add('<rect x="340" y="%d" width="920" height="%d" rx="10" fill="#0e0804" stroke="#e6b66a" stroke-width="4"/>' % (by, bh))
D.add('<clipPath id="bd"><rect x="344" y="%d" width="912" height="%d"/></clipPath>' % (by + 2, bh - 4))
def half(seed, col, up, head):
    N = 228
    env = envelope(seed, N)
    d = ''.join('M%s,%s v%s' % (n(344 + i * 4 + 2), n(EY), n((-1 if up else 1) * env[i] * 40)) for i in range(N))
    return '<path d="%s" stroke="%s" stroke-width="2.8"/>' % (d, col)
D.add('<g clip-path="url(#bd)">' + half(11, TQ, True, 126) + half(29, '#f08a2e', False, 1126 - 344) + '<path d="M344,%d H1256" stroke="#e6b66a" stroke-opacity=".6"/>' % EY +
      '<path d="M470,%d V%d" stroke="%s" stroke-width="3"/><path d="M1130,%d V%d" stroke="#f08a2e" stroke-width="3"/>' % (by + 4, EY, TQ, EY, by + bh - 4) + '</g>')
D.add(T.text(366, EY - 24, 'A ▲', 14, TQ, 'start', '800', halo='') + T.text(366, EY + 38, 'B ▼', 14, '#f08a2e', 'start', '800', halo=''))
D.put('wave', ('', 912, 88), 344, by + 2)
# nose
D.add('<path d="M800,560 L730,760 L770,790 L800,772 L830,790 L870,760Z" fill="url(#wood2)" stroke="#2a1708" stroke-width="4"/><circle cx="772" cy="772" r="9" fill="#120a05"/><circle cx="828" cy="772" r="9" fill="#120a05"/>')
# mouth
D.add('<rect x="330" y="800" width="940" height="364" rx="26" fill="url(#mouthg)" stroke="#2a1708" stroke-width="6"/>')
tw, pitch, tx0 = 54, 58.75, 335
loA = ['IN', 'OUT', '◂', '4', '▸', 'KEY−', 'KEY+', '±8%']
for i in range(16):
    d, j = ('A', i) if i < 8 else ('B', i - 8)
    x = 331 + i * pitch + 2
    lit = j in DECK[d]['lit']
    D.add('<rect x="%s" y="806" width="%d" height="80" rx="6" fill="%s" stroke="#2a1708" stroke-width="3"/>' % (n(x), tw, HC[j] if lit else CRM))
    D.add(T.text(x + 27, 856, str(j + 1), 17, '#1a0f06', weight='800', halo=''))
    if not lit: D.add('<circle cx="%s" cy="874" r="5" fill="%s"/>' % (n(x + 27), HC[j]))
    col = T.colA if d == 'A' else T.colB
    D.add('<rect x="%s" y="1092" width="%d" height="62" rx="6" fill="%s" stroke="#2a1708" stroke-width="3"/>' % (n(x), tw, col if (j == 1 and d == 'A') else CRM))
    D.add(T.text(x + 27, 1129, loA[j], 12, '#1a0f06', weight='800', halo=''))
D.names['pads A'] = (331, 806, 8 * pitch, 80); D.names['pads B'] = (331 + 8 * pitch, 806, 8 * pitch, 80)
for dd in 'AB':
    x0 = 331 + (0 if dd == 'A' else 8 * pitch)
    D.names['loop ' + dd] = (x0, 1092, 5 * pitch, 62); D.names['keyshift ' + dd] = (x0 + 5 * pitch, 1092, 2 * pitch, 62)
D.put('ch1', b_strip_h(T, 1, T.colA), 503, 892)
D.put('ch2', b_strip_h(T, 2, T.colB), 503, 949)
D.put('xfader', b_xfader(Wood(track='#6a4424'), w=420), 590, 1014)
# forehead plaque, bridge, cheeks, ears
both(D, T, 'info', b_info, 400, 180, w=270)
both(D, T, 'overview', b_overview, 405, 268, w=260)
D.put('master', b_master(T), 678, 190)
D.put('sampler', b_sampler(T), 699, 34)
both(D, T, 'transport', b_transport, 650, 545, orient='v')
D.put('mic', b_mic(T), 710, 345)
D.put('rec', b_rec(T), 761, 396)
both(D, T, 'toggles', b_toggles, 300, 650, orient='row')
both(D, T, 'loop', b_loop, 300, 694, orient='row')
both(D, T, 'padmodes', b_padmodes, 504, 694)
both(D, T, 'keyshift', b_keyshift, 300, 740)
both(D, T, 'tempo', b_tempo, 122, 482, h=100)
both(D, T, 'fx', b_fx, 42, 712)
# totem base browser
D.add('<path d="M330,1214 L1270,1214 L1290,1590 L310,1590Z" fill="url(#wood)" stroke="#2a1708" stroke-width="5"/>')
D.add('<path d="M320,1230 ' + ' '.join('l20,%d' % (-12 if k % 2 == 0 else 12) for k in range(48)) + '" stroke="%s" stroke-width="5" fill="none"/>' % RD)
D.put('browser', b_browser(T, TQ, 880, 280, 'TOTEM BASE  \u00b7  TRACK BROWSER', nrows=5), 360, 1270)
D.put('appbtns', b_appbtns(T), 1290, 1270)
D.put('status', b_status(T), 540, 1612)
D.save(os.path.dirname(__file__))
