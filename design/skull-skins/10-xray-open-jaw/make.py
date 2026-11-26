import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1950
D = Doc(W, H)
CA, CB = '#3fe6ff', '#ffc245'
BONE_HI, BONE_LO = '#f2faff', '#8db6dc'

class XR(Theme):
    ink = '#0c2236'; dim = '#1f4a6c'; btn_fill = '#0b1b2e'; btn_stroke = '#9fd8ff'; btn_text = '#e8f6ff'
    accent = CA; on_text = '#021018'; knob_fill = '#0b1b2e'; knob_stroke = '#9fd8ff'; track = '#071422'
    meter_off = '#4b6a88'; pad_off = '#0b1b2e'; wave_bg = '#04101c'; colA = CA; colB = CB; btn_r = 7
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        o = Theme.knob(s, cx, cy, r, val, None, col, bipolar)
        if label:
            o += s.text(cx, cy + r + 13, label, 11, s.dim, weight='700', halo='')
        return o
T = XR()                                      # on bone
DX = XR(ink='#e8f6ff', dim='#a9d4f2')         # on dark
D.d('<filter id="glow" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="4" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>')
D.d('<radialGradient id="bone" cx="50%" cy="35%" r="80%"><stop offset="0" stop-color="#f6fbff"/><stop offset=".6" stop-color="#bcdcf5"/><stop offset="1" stop-color="#7fa9d4"/></radialGradient>')
D.d('<linearGradient id="vb" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#eaf6ff"/><stop offset="1" stop-color="#9fc4e6"/></linearGradient>')
D.d('<radialGradient id="soc" cx="50%" cy="45%" r="60%"><stop offset="0" stop-color="#0c2038"/><stop offset="1" stop-color="#02060d"/></radialGradient>')
# ---- spine (behind everything)
D.add('<rect x="772" y="1420" width="56" height="500" fill="#6f9bc6" stroke="#cfeaff" stroke-width="2" opacity=".8"/>')
vy = [1452 + i * 64 for i in range(7)]
names = ['C1  FX 1', 'C2  FX 2', 'C3  FX 3', 'C4  MASTER', 'C5  CHANNEL FADERS', 'C6  MIC', 'C7  APP']
for i, y in enumerate(vy):
    D.add('<path d="M500,%d Q620,%d 704,%d H896 Q980,%d 1100,%d Q1100,%d 980,%d H620 Q500,%d 500,%d Z" fill="url(#vb)" stroke="#e8f6ff" stroke-width="3" filter="url(#glow)"/>' % (y + 12, y - 4, y, y - 4, y + 12, y + 56, y + 56, y + 56, y + 12))
    D.add('<rect x="704" y="%d" width="192" height="56" rx="16" fill="url(#vb)" stroke="#3a6a96" stroke-width="2.5"/>' % y)
    D.add(T.text(800, y + 35, names[i], 12, T.dim, weight='800', halo=''))
fl = [('A', CA), ('B', CB)]
for i in range(3):
    D.put('fxslot %d' % i, ('', 0, 0), 0, 0) if False else None
    for d, col in fl:
        svg_, w_, h_ = b_fxslot(T, d, col, i)
        x = 506 if d == 'A' else 898
        D.add('<g transform="translate(%d,%d)">%s</g>' % (x, vy[i] + 14, svg_))
D.names['fx A'] = (506, vy[0] + 14, 196, 28 + 2 * 64); D.names['fx B'] = (898, vy[0] + 14, 196, 28 + 2 * 64)
y = vy[3]
D.add(T.knob(556, y + 22, 18, .72, 'MASTER', T.accent) + T.knob(650, y + 22, 18, .5, 'HP LEVEL', T.accent) + T.knob(948, y + 22, 18, .35, 'HP MIX', T.accent))
D.add(T.meter(990, y + 12, 100, 10, .78, True, 12) + T.meter(990, y + 28, 100, 10, .72, True, 12) + T.text(1040, y + 52, 'MASTER L / R', 11, T.dim, weight='700', halo=''))
D.names['master'] = (530, y, 560, 56)
y = vy[4]
D.add(T.btn(512, y + 12, 46, 30, 'CUE', False, CA, 12) + T.fader_h(566, y + 8, 128, 28, .8, CA) + T.meter(566, y + 42, 128, 8, .75, True, 14))
D.add(T.btn(1042, y + 12, 46, 30, 'CUE', True, CB, 12) + T.fader_h(906, y + 8, 128, 28, .68, CB) + T.meter(906, y + 42, 128, 8, .6, True, 14))
D.names['ch1 fader'] = (512, y, 190, 56)
y = vy[5]
D.add(T.btn(512, y + 12, 54, 30, 'MIC', True, T.accent, 12) + T.btn(572, y + 12, 66, 30, 'TALK', False, T.accent, 12) + T.knob(672, y + 27, 15, .55, None, T.accent))
D.add(T.btn(906, y + 12, 78, 32, 'REC', False, '#ff3b3b', 13) + '<circle cx="920" cy="%d" r="5" fill="#ff3b3b"/>' % (y + 28))
D.names['mic'] = (512, y, 190, 56); D.names['rec'] = (906, y, 78, 56)
y = vy[6]
D.add(T.btn(512, y + 12, 56, 30, '⚙ SET', False, T.accent, 11) + T.btn(574, y + 12, 56, 30, 'SKIN', False, T.accent, 11) + T.btn(906, y + 12, 56, 30, 'EN', False, T.accent, 11))
D.names['appbtns'] = (512, y, 450, 56)
# ---- mandible (dropped open)
jl = [(800, 1180), (420, 1182), (350, 1250), (360, 1330), (460, 1395), (620, 1425), (800, 1432)]
jp = sym(jl, W)
for sx in (1, -1):
    def X(x): return x if sx == 1 else W - x
    D.add('<path d="M%s,1240 L%s,780 L%s,740 L%s,1180 Z" fill="url(#bone)" stroke="#e8f6ff" stroke-width="3" filter="url(#glow)" opacity=".92"/>' % (n(X(350)), n(X(262)), n(X(236)), n(X(420))))
D.add('<path d="%s" fill="url(#bone)" stroke="#e8f6ff" stroke-width="4" filter="url(#glow)"/>' % smooth(jp))
# ---- skull (cranium + maxilla), huge sockets
left = [(800, 18), (560, 24), (380, 68), (255, 150), (185, 260), (155, 400), (165, 540), (205, 640), (245, 720), (290, 790), (320, 885), (560, 894), (800, 896)]
pts = sym(left, W)
D.d('<mask id="cut"><rect width="4000" height="4000" fill="#fff"/><path d="M800,640 L740,760 L772,790 L800,772 L828,790 L860,760Z" fill="#000"/></mask>')
D.add('<path d="%s" fill="url(#bone)" stroke="#e8f6ff" stroke-width="4" filter="url(#glow)" mask="url(#cut)"/>' % smooth(pts))
D.add('<path d="M800,640 L740,760 L772,790 L800,772 L828,790 L860,760Z" fill="none" stroke="#e8f6ff" stroke-width="3"/>')
D.add('<path d="M270,170 Q800,40 1330,170" stroke="#5f8fbf" stroke-width="2" fill="none" stroke-dasharray="12 7"/>')
EY, ER = 400, 232
for cx, d, col in ((462, 'A', CA), (1138, 'B', CB)):
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#5d86b4"/><circle cx="%d" cy="%d" r="%d" fill="url(#soc)" stroke="#e8f6ff" stroke-width="3"/>' % (cx, EY, ER + 6, cx, EY, ER))
    D.add('<path d="%s" stroke="%s" stroke-width="10" fill="none" stroke-linecap="round" filter="url(#glow)"/>' % (arc(cx, EY, 218, 100, 300 if d == 'A' else 250), col))
    for k in range(48):
        a = math.radians(k * 7.5)
        D.add('<path d="M%s,%s L%s,%s" stroke="#6f9bc6" stroke-width="2"/>' % (n(cx + 203 * math.cos(a)), n(EY + 203 * math.sin(a)), n(cx + 212 * math.cos(a)), n(EY + 212 * math.sin(a))))
    ma = 232 if d == 'A' else 308
    D.add('<circle cx="%s" cy="%s" r="14" fill="%s" stroke="#fff" stroke-width="3" filter="url(#glow)"/>' % (n(cx + 218 * math.cos(math.radians(ma))), n(EY + 218 * math.sin(math.radians(ma))), col))
    D.add('<circle cx="%d" cy="%d" r="196" fill="#02070e" stroke="#2a4a6a" stroke-width="2"/><clipPath id="d%s"><circle cx="%d" cy="%d" r="195"/></clipPath>' % (cx, EY, d, cx, EY))
    Dd = DECK[d]
    g = '<g clip-path="url(#d%s)">' % d + wave(cx - 190, EY - 62, 380, 124, Dd['seed'], col, '#fff', head=190, cues=Dd['cues'], pitch=4) + '</g>'
    D.add(g)
    D.add(DX.text(cx, EY - 128, Dd['title'], 21, '#fff', weight='700', halo='') + DX.text(cx, EY - 104, Dd['artist'], 14, '#a9d4f2', halo='') + DX.text(cx, EY - 150, 'DECK ' + d, 13, col, weight='800', halo=''))
    D.add(DX.text(cx - 112, EY + 108, Dd['bpm'], 30, col, 'start', '700', mono=True, halo='') + DX.text(cx - 112, EY + 128, 'BPM  KEY ' + Dd['key'], 12, '#a9d4f2', 'start', '700', halo='') + DX.text(cx + 124, EY + 96, Dd['t'], 18, '#fff', 'end', '700', mono=True, halo='') + DX.text(cx + 124, EY + 118, Dd['rem'], 15, '#a9d4f2', 'end', '600', mono=True, halo=''))
    ov = b_overview(DX, d, col, 236, 30)
    D.add('<g transform="translate(%d,%d)">%s</g>' % (cx - 118, EY + 140, ov[0]))
    D.names['info ' + d] = (cx - 190, EY - 150, 380, 60); D.names['overview ' + d] = (cx - 118, EY + 140, 236, 30)
    D.put('jog ' + d, ('', 2 * ER, 2 * ER), cx - ER, EY - ER)
D.names['wave'] = (272, 338, 380, 124)
D.add('<path d="M694,400 H906" stroke="#e8f6ff" stroke-width="2" stroke-dasharray="4 7"/>')
# ---- brow + cheeks + temples
both(D, T, 'transport', b_transport, 450, 42)
both(D, T, 'keyshift', b_keyshift, 664, 50)
both(D, T, 'toggles', b_toggles, 420, 94)
both(D, T, 'loop', b_loop, 424, 130)
both(D, T, 'padmodes', b_padmodes, 330, 700)
both(D, T, 'tempo', b_tempo, 168, 318, h=110)
# ---- teeth: upper = hot cues; lower = loop / key shift
tw, pitch, tx0 = 54, 58.75, 330
loA = ['IN', 'OUT', '◂', '4', '▸', 'KEY−', 'KEY+', '±8%']
for i in range(16):
    d, j = ('A', i) if i < 8 else ('B', i - 8)
    col = CA if d == 'A' else CB
    x = tx0 + i * pitch
    lit = j in DECK[d]['lit']
    D.add('<path d="%s" fill="%s" stroke="#6f9bc6" stroke-width="2.5"/>' % (tooth(x, 806, tw, 82, True), HC[j] if lit else '#f4fbff'))
    D.add(T.text(x + 27, 858, str(j + 1), 17, '#06131f', weight='800', halo=''))
    if not lit: D.add('<circle cx="%s" cy="876" r="5" fill="%s"/>' % (n(x + 27), HC[j]))
    D.add(tooth_btn(T, x, 1176, tw, 70, loA[j], j == 1 and d == 'A', col, False, 12, '#0b1b2e', '#9fd8ff', '#e8f6ff', '#021018'))
D.names['pads A'] = (tx0, 806, 8 * pitch, 82); D.names['pads B'] = (tx0 + 8 * pitch, 806, 8 * pitch, 82)
for dd in 'AB':
    x0 = tx0 + (0 if dd == 'A' else 8 * pitch)
    D.names['loop ' + dd + ' (teeth)'] = (x0, 1176, 5 * pitch, 70); D.names['keyshift ' + dd + ' (teeth)'] = (x0 + 5 * pitch, 1176, 2 * pitch, 70)
# ---- inside the open mouth: mixer on a dim plate
D.add('<rect x="480" y="900" width="640" height="204" rx="30" fill="#04101c" fill-opacity=".82" stroke="#9fd8ff" stroke-opacity=".6" stroke-width="2"/>')
D.put('ch1', b_strip_h(DX, 1, CA), 503, 912)
D.put('ch2', b_strip_h(DX, 2, CB), 503, 974)
D.add(T.text(800, 1098, '', 1))
D.put('xfader', b_xfader(DX, w=420), 590, 1125) if False else None
# ---- mandible body: sampler, crossfader along the lower jaw, mic zone
D.put('sampler', b_sampler(T), 470, 1272)
D.put('xfader', b_xfader(T, w=300), 650, 1348) if False else None
D.add('<path d="M720,1330 Q800,1380 880,1330" stroke="#0b1b2e" stroke-width="10" fill="none" stroke-linecap="round"/>') if False else None
D.names['xfader'] = (700, 1262, 400, 70)
D.add('<rect x="700" y="1300" width="400" height="10" rx="5" fill="#0b1b2e" stroke="#9fd8ff"/>' )
D.add(T.shape(850 - 18, 1284, 36, 42, '#0b1b2e', '#e8f6ff', 2, 5) + '<path d="M850,1290 v30" stroke="#e8f6ff" stroke-width="3"/>')
D.add(T.text(700, 1340, 'A', 14, '#0b4a6a', 'start', '800', halo='') + T.text(1100, 1340, 'B', 14, '#6a4a00', 'end', '800', halo='') + T.text(900, 1340, 'CROSSFADER · along the mandible', 11, T.dim, weight='700', halo=''))
# ---- floating X-ray film browser (separate window)
fx0, fy0 = 1140, 1450
D.add('<path d="M%d,%d H%d L%d,%d V%d H%d Z" fill="#0a1626" stroke="#9fd8ff" stroke-width="3" stroke-linejoin="round" transform="rotate(1.5 %d %d)"/>' % (fx0 - 10, fy0 - 26, fx0 + 420, fx0 + 450, fy0 + 14, fy0 + 440, fx0 - 10, fx0, fy0))
D.put('browser', b_browser(DX, CA, 430, 400, 'X-RAY FILM · BROWSER', nrows=7), fx0, fy0 + 14)
D.put('status', b_status(DX), 30, 1912)
D.save(os.path.dirname(__file__))
