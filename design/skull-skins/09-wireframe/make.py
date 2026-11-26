import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1500
BG, LN = '#f6f4ee', '#1c1c22'
D = Doc(W, H, BG)
BLU, ORG = '#0a6cff', '#e8451e'

class Wire(Theme):
    ink = LN; dim = '#4a4a55'; btn_fill = BG; btn_stroke = LN; btn_text = LN; btn_r = 3; btn_sw = 2
    accent = BLU; on_text = '#ffffff'; knob_fill = BG; knob_stroke = LN; track = BG; meter_off = '#d6d3c8'
    pad_off = BG; wave_bg = BG; colA = BLU; colB = ORG; core = LN; pad_r = 3
    def pad(s, x, y, w, h, i, lit, col=None, label=None):
        col = col or HC[i % 8]
        o = s.shape(x, y, w, h, col if lit else BG, LN, 2, 3)
        o += s.text(x + w / 2, y + h / 2 + 6, label or str(i + 1), 17, '#000' if lit else LN, weight='700', halo='')
        return o
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        ang = 135 + 270 * val
        o = '<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="2"/>' % (n(cx), n(cy), n(r), BG, LN)
        o += '<path d="%s" stroke="%s" stroke-width="4" fill="none"/>' % (arc(cx, cy, r + 4, 135, ang), col or BLU)
        o += '<path d="M%s,%s L%s,%s" stroke="%s" stroke-width="2.5"/>' % (n(cx), n(cy), n(cx + (r - 2) * math.cos(math.radians(ang))), n(cy + (r - 2) * math.sin(math.radians(ang))), LN)
        if label:
            o += s.text(cx, cy + r + 15, label, 11, s.dim, weight='700', halo='')
        return o
    def fader_v(s, x, y, w, h, val, col=None, label=None):
        return Theme.fader_v(s, x, y, w, h, val, col, label)
T = Wire()
def wline(x, y, w, h, seed, col, head, flat=0):
    N = int(w / 3)
    env = envelope(seed, N + 4)
    top = ' '.join('%s,%s' % (n(x + i * 3), n(y + h / 2 - env[i] * h / 2 * (1 if x + i * 3 < flat or flat == 0 else 0.0))) for i in range(N))
    bot = ' '.join('%s,%s' % (n(x + i * 3), n(y + h / 2 + env[i] * h / 2 * (1 if x + i * 3 < flat or flat == 0 else 0.0))) for i in range(N))
    return '<polyline points="%s" fill="none" stroke="%s" stroke-width="2"/><polyline points="%s" fill="none" stroke="%s" stroke-width="2"/><path d="M%s,%s H%s" stroke="%s" stroke-width="1"/><path d="M%s,%s V%s" stroke="%s" stroke-width="3"/>' % (top, col, bot, col, n(x), n(y + h / 2), n(x + w), col, n(x + head), n(y - 6), n(y + h + 6), LN)
left = [(800, 40), (620, 50), (470, 110), (385, 210), (355, 330), (360, 440), (385, 520), (370, 600), (395, 680), (410, 760), (425, 830), (480, 890), (620, 930), (800, 940)]
pts = sym(left, W)
D.d('<mask id="cut"><rect width="4000" height="4000" fill="#fff"/><path d="M800,520 L735,640 L765,665 L800,650 L835,665 L865,640Z" fill="#000"/></mask>')
D.add('<path d="%s" fill="%s" stroke="%s" stroke-width="3.5" stroke-linejoin="round" mask="url(#cut)"/>' % (smooth(pts), BG, LN))
D.add('<path d="M395,225 Q800,90 1205,225" stroke="%s" stroke-width="1.5" fill="none" stroke-dasharray="10 6"/>' % LN)
D.add('<path d="M395,600 Q500,650 660,630 M1205,600 Q1100,650 940,630" stroke="%s" stroke-width="2" fill="none"/>' % LN)
EY = 340
for cx, d, col in ((570, 'A', BLU), (1030, 'B', ORG)):
    D.add('<circle cx="%d" cy="%d" r="160" fill="none" stroke="%s" stroke-width="3.5"/><circle cx="%d" cy="%d" r="122" fill="none" stroke="%s" stroke-width="2"/>' % (cx, EY, LN, cx, EY, LN))
    D.add('<path d="%s" stroke="%s" stroke-width="7" fill="none"/>' % (arc(cx, EY, 141, 110, 300 if d == 'A' else 250), col))
    for k in range(36):
        a = math.radians(k * 10)
        D.add('<path d="M%s,%s L%s,%s" stroke="%s" stroke-width="1.5"/>' % (n(cx + 153 * math.cos(a)), n(EY + 153 * math.sin(a)), n(cx + 160 * math.cos(a)), n(EY + 160 * math.sin(a)), LN))
    ma = 232 if d == 'A' else 308
    D.add('<circle cx="%s" cy="%s" r="11" fill="%s" stroke="%s" stroke-width="2.5"/>' % (n(cx + 141 * math.cos(math.radians(ma))), n(EY + 141 * math.sin(math.radians(ma))), col, LN))
    D.add(T.text(cx, EY - 80, d, 28, col, weight='800', halo='') + T.text(cx, EY + 100, 'JOG', 11, T.dim, weight='700', halo=''))
    D.put('jog ' + d, ('', 320, 320), cx - 160, EY - 160)
# one continuous line: waveform through both eyes, flat across the bridge
D.add('<clipPath id="w1"><rect x="450" y="280" width="240" height="120"/></clipPath>')
D.add(wline(448, 290, 244, 100, 11, BLU, 122) + wline(908, 290, 244, 100, 29, ORG, 122))
D.add('<path d="M692,340 H908" stroke="%s" stroke-width="2"/>' % LN)
D.put('wave', ('', 244, 100), 448, 290)
D.add('<path d="M800,520 L735,640 L765,665 L800,650 L835,665 L865,640Z" fill="none" stroke="%s" stroke-width="3"/>' % LN)
# teeth
tw, pitch, tx0 = 44, 47, 424
for i in range(16):
    d, j = ('A', i) if i < 8 else ('B', i - 8)
    x = tx0 + i * pitch
    lit = j in DECK[d]['lit']
    D.add('<path d="%s" fill="%s" stroke="%s" stroke-width="2.5"/>' % (tooth(x, 690, tw, 82, True, 10), HC[j] if lit else BG, LN))
    D.add(T.text(x + 22, 740, str(j + 1), 17, LN, weight='700', halo=''))
    D.add(T.pad(x, 782, tw, 58, i % 8, i in (1, 4, 6, 9, 12), HC[i % 8], 'S%d' % (i + 1)).replace('<rect', '<rect'))
D.names['pads A'] = (tx0, 690, 8 * pitch, 82); D.names['pads B'] = (tx0 + 8 * pitch, 690, 8 * pitch, 82); D.names['sampler'] = (tx0, 782, 752, 58)
# jaw crossfader along curved rail
D.add('<path d="M520,866 Q800,940 1080,866" stroke="%s" stroke-width="3" fill="none"/>' % LN)
D.add('<g transform="rotate(7 755 899)"><rect x="735" y="880" width="40" height="38" rx="4" fill="%s" stroke="%s" stroke-width="3"/><path d="M755,885 v28" stroke="%s" stroke-width="3"/></g>' % (BG, LN, LN))
D.add(T.text(500, 860, 'A', 15, BLU, weight='800', halo='') + T.text(1100, 860, 'B', 15, ORG, weight='800', halo='') + T.text(800, 912, 'CROSSFADER (jaw rail)', 11, T.dim, weight='700', halo=''))
D.names['xfader'] = (520, 860, 560, 80)
# floating deck panels (separate windows)
def panel(x, y, w, h):
    return '<rect x="%d" y="%d" width="%d" height="%d" rx="22" fill="#fbfaf7" stroke="#1c1c22" stroke-width="3"/>' % (x, y, w, h) + grip(x + w / 2, y + 12, LN, .9)
D.add(panel(6, 14, 346, 600)); D.add(panel(1248, 14, 346, 600)); D.add(panel(6, 628, 346, 270)); D.add(panel(1248, 628, 346, 110))
D.add(panel(470, 950, 660, 180))
D.add(panel(1240, 920, 346, 96)) if False else None
# side columns
both(D, T, 'info', b_info, 20, 44, w=320)
both(D, T, 'overview', b_overview, 20, 140)
both(D, T, 'transport', b_transport, 20, 190)
both(D, T, 'keyshift', b_keyshift, 20, 246)
both(D, T, 'toggles', b_toggles, 20, 288, orient='row')
both(D, T, 'loop', b_loop, 20, 330, orient='row')
both(D, T, 'padmodes', b_padmodes, 20, 372)
both(D, T, 'fx', b_fx, 20, 420)
both(D, T, 'tempo', b_tempo, 250, 420, h=110)
D.put('master', b_master(T), 30, 640)
D.put('mic', b_mic(T), 30, 760)
D.put('rec', b_rec(T), 30, 830)
D.put('appbtns', b_appbtns(T), 1370, 678)
D.put('ch1', b_strip_h(T, 1, BLU), 503, 975)
D.put('ch2', b_strip_h(T, 2, ORG), 503, 1040)
# leader lines + callouts
for (x1, y1, x2, y2, t) in ((345, 160, 400, 200, 'brow → deck info'), (345, 560, 400, 560, 'cheek → deck controls'), (700, 610, 760, 640, 'nose')):
    pass
D.put('browser', b_browser(Wire(), BLU, 860, 290, 'TRACK BROWSER (floating window)', nrows=5).__class__ and (lambda r: (r[0].replace('fill="#0a0b0e"', 'fill="#fbfaf7"'), r[1], r[2]))(b_browser(T, BLU, 860, 290, 'TRACK BROWSER (floating window)', nrows=5)), 370, 1160)
D.put('status', b_status(T), 540, 1470)
D.save(os.path.dirname(__file__))
