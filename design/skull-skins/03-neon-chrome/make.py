import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1570
D = Doc(W, H, '#04060c')
CY, MG, LM = '#19e6ff', '#ff2fb0', '#b6ff3b'

def cham(x, y, w, h, c):
    return 'M%s,%s h%s l%s,%s v%s l-%s,%s h-%s l-%s,-%s v-%s z' % (n(x + c), n(y), n(w - 2 * c), n(c), n(c), n(h - 2 * c), n(c), n(c), n(w - 2 * c), n(c), n(c), n(h - 2 * c))

class Neon(Theme):
    ink = '#eaf6ff'; dim = '#8fb3cc'; btn_fill = '#0b1220'; btn_stroke = '#2fd8ee'; btn_text = '#e8fbff'
    accent = CY; on_text = '#001014'; knob_fill = '#0b1220'; knob_stroke = '#2fd8ee'; track = '#02040a'
    meter_off = '#16202e'; pad_off = '#0b1220'; wave_bg = '#02050a'; colA = CY; colB = MG; btn_r = 6
    def shape(s, x, y, w, h, fill, stroke, sw=None, r=None, extra=''):
        return '<path d="%s" fill="%s" stroke="%s" stroke-width="%s" %s/>' % (cham(x, y, w, h, min(8, h / 4)), fill, stroke, n(sw or 1.8), extra)
    def knob(s, cx, cy, r, val, label=None, col=None, bipolar=False):
        col = col or s.accent
        ang = 135 + 270 * val
        o = '<circle cx="%s" cy="%s" r="%s" fill="#050a14" stroke="%s" stroke-width="2"/>' % (n(cx), n(cy), n(r), '#3a7f9c')
        o += '<path d="%s" stroke="%s" stroke-width="4" fill="none" filter="url(#glow)"/>' % (arc(cx, cy, r - 2, 135, ang) if not bipolar else arc(cx, cy, r - 2, min(270, ang), max(270, ang)), col)
        o += '<path d="M%s,%s L%s,%s" stroke="#fff" stroke-width="2.5"/>' % (n(cx + (r - 9) * math.cos(math.radians(ang))), n(cy + (r - 9) * math.sin(math.radians(ang))), n(cx + (r - 3) * math.cos(math.radians(ang))), n(cy + (r - 3) * math.sin(math.radians(ang))))
        if label:
            o += s.text(cx, cy + r + 13, label, 11, s.dim, weight='600')
        return o
T = Neon(); DK = Neon()
D.d('<filter id="glow" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="3" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>')
D.d('<linearGradient id="chrome" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#3c465c"/><stop offset=".35" stop-color="#151b28"/><stop offset=".6" stop-color="#2a3347"/><stop offset="1" stop-color="#0b0f18"/></linearGradient>')
D.d('<linearGradient id="vis" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#06222c"/><stop offset="1" stop-color="#02060c"/></linearGradient>')
D.d('<pattern id="scan" width="4" height="4" patternUnits="userSpaceOnUse"><rect width="4" height="1" fill="#19e6ff" opacity=".12"/></pattern>')
D.d('<pattern id="grid" width="60" height="60" patternUnits="userSpaceOnUse"><path d="M60,0 H0 V60" fill="none" stroke="#19e6ff" stroke-opacity=".08"/></pattern>')
left = [(800, 30), (600, 38), (420, 90), (300, 190), (250, 310), (250, 430), (265, 520), (228, 600), (250, 690), (300, 770), (330, 850), (335, 930), (380, 1040), (500, 1130), (800, 1165)]
pts = sym(left, W)
poly = pts
pp = 'M' + ' L'.join('%s,%s' % (n(x), n(y)) for x, y in pts) + 'Z'
# headphone cups (ears)
for sx in (1, -1):
    def X(x): return x if sx == 1 else W - x
    cup = [(X(30), 300), (X(205), 285), (X(250), 330), (X(250), 660), (X(205), 705), (X(30), 690)]
    D.add('<path d="M%s Z" fill="url(#chrome)" stroke="%s" stroke-width="3" filter="url(#glow)"/>' % (' L'.join('%s,%s' % (n(a), n(b)) for a, b in cup), MG if sx == 1 else CY))
    D.add('<path d="M%d,300 L%d,690" stroke="#ffffff" stroke-opacity=".08" stroke-width="30"/>' % (X(130), X(130)))
D.d('<mask id="cut"><rect width="4000" height="4000" fill="#fff"/><path d="M800,540 L700,690 L735,730 L800,700 L865,730 L900,690Z" fill="#000"/></mask>')
D.add('<path d="%s" fill="url(#chrome)" stroke="%s" stroke-width="3" filter="url(#glow)" mask="url(#cut)"/>' % (pp, CY))
# facet lines
fl = [(800, 30, 420, 90), (800, 30, 1180, 90), (420, 90, 300, 190), (300, 190, 800, 230), (1300, 190, 800, 230), (250, 310, 330, 340), (265, 520, 330, 560), (228, 600, 330, 640), (330, 850, 400, 780), (380, 1040, 560, 960), (1220, 1040, 1040, 960), (500, 1130, 560, 960), (1100, 1130, 1040, 960), (800, 1165, 800, 1130)]
D.add('<path d="%s" stroke="%s" stroke-opacity=".28" stroke-width="2" fill="none"/>' % (''.join('M%d,%d L%d,%d' % f for f in fl), CY))
# visors
def vis(sx):
    def X(x): return x if sx == 1 else W - x
    return [(X(300), 360), (X(470), 322), (X(655), 372), (X(640), 472), (X(470), 515), (X(310), 482)]
for sx, d, col in ((1, 'A', CY), (-1, 'B', MG)):
    v = vis(sx)
    vp = 'M' + ' L'.join('%s,%s' % (n(a), n(b)) for a, b in v) + 'Z'
    D.add('<path d="%s" fill="url(#vis)" stroke="%s" stroke-width="4" filter="url(#glow)"/><path d="%s" fill="url(#scan)"/>' % (vp, col, vp))
    D.add('<clipPath id="vc%s"><path d="%s"/></clipPath>' % (d, vp))
# stacked lanes crossing both visors and the nasal bridge; playhead at the bridge
D.add('<rect x="296" y="346" width="1008" height="150" rx="6" fill="#02050a" fill-opacity=".72" stroke="%s" stroke-opacity=".6"/>' % CY)
D.add('<g>' + wave(300, 352, 1000, 66, 11, CY, '#e8fdff', head=500, cues=[.1, .2, .31, .4, .62], pitch=4, past_op=.55) + wave(300, 424, 1000, 66, 29, MG, '#ffe8f6', head=500, cues=[.12, .27, .33, .55], pitch=4, past_op=.55) + '</g>')
D.add('<path d="M800,330 V512" stroke="#fff" stroke-width="3" filter="url(#glow)"/><path d="M786,330 L814,330 L800,344Z M786,512 L814,512 L800,498Z" fill="#fff"/>')
D.add(T.text(330, 372, 'A', 20, CY, 'start', '800') + T.text(330, 478, 'B', 20, MG, 'start', '800'))
D.put('wave', ('', 1000, 150), 300, 346)
for sx, d, col in ((1, 'A', CY), (-1, 'B', MG)):
    D.put('jog ' + d, ('', 0, 0), 0, 0) if False else None
# jog HUDs on the cheeks
for cx, d, col in ((392, 'A', CY), (1208, 'B', MG)):
    D.add('<g filter="url(#glow)"><circle cx="%d" cy="682" r="94" fill="#050a14" stroke="%s" stroke-width="3"/></g>' % (cx, col))
    D.add(T.jog(cx, 682, 88, 300 if d == 'A' else 230, col))
    D.add('<path d="%s" stroke="%s" stroke-width="6" fill="none" stroke-linecap="round" filter="url(#glow)"/>' % (arc(cx, 682, 78, 140, 330), col))
    D.add(T.text(cx, 690, d, 28, col, weight='800') + T.text(cx, 718, 'JOG', 11, T.dim, weight='700'))
    D.put('jog ' + d, ('', 188, 188), cx - 94, 588)
# nose
D.add('<path d="M800,540 L700,690 L735,730 L800,700 L865,730 L900,690Z" fill="none" stroke="#19e6ff" stroke-width="3" filter="url(#glow)"/>')
# teeth: upper = sampler 16 pads, lower = LED level bar
for i in range(16):
    x = 330 + i * 58.75
    col = HC[i % 8]
    lit = i in (1, 4, 6, 9, 12)
    D.add('<path d="%s" fill="%s" stroke="%s" stroke-width="2" %s/>' % (tooth(x, 800, 54, 80, True, 12), col if lit else '#0b1220', col, 'filter="url(#glow)"' if lit else ''))
    D.add(T.text(x + 27, 848, 'S%d' % (i + 1), 15, '#001014' if lit else col, weight='800'))
    lv = [.9, .8, .7, .6, .5, .45, .3, .2]
    seg = lv[i % 8] if i < 8 else lv[7 - i % 8]
    c = '#35d36b' if seg < .6 else ('#f2d13a' if seg < .85 else '#ff4545')
    D.add('<path d="%s" fill="%s" stroke="#1d3a4c" stroke-width="1.5"/>' % (tooth(x, 888, 54, 46, False, 10), c if seg > .1 and i not in (7, 8) else '#101a28'))
D.names['sampler'] = (330, 800, 940, 80)
D.add(T.text(800, 952, 'LOWER TEETH · MASTER LEVEL L/R', 11, T.dim, weight='700'))
# blocks
both(D, T, 'info', b_info, 400, 92, w=320)
both(D, T, 'overview', b_overview, 405, 184)
both(D, T, 'toggles', b_toggles, 340, 232, orient='grid')
both(D, T, 'loop', b_loop, 492, 232, orient='grid')
both(D, T, 'keyshift', b_keyshift, 340, 304)
D.put('master', b_master(T), 678, 236)
both(D, T, 'transport', b_transport, 500, 598, orient='v')
both(D, T, 'padmodes', b_padmodes, 578, 606, orient='grid')
both(D, T, 'fx', b_fx, 26, 306)
both(D, T, 'pads', b_pads, 28, 418, s=44, g=5)
both(D, T, 'tempo', b_tempo, 100, 530, h=90)
D.put('mic', b_mic(T), 710, 740)
D.put('rec', b_rec(T), 761, 796) if False else None
D.put('ch1', b_strip_h(T, 1, CY), 503, 970)
D.put('ch2', b_strip_h(T, 2, MG), 503, 1034)
D.put('xfader', b_xfader(T, w=420), 590, 1095)
D.put('rec', b_rec(T), 1370, 770) if False else None
# second window: holo terminal browser
D.add('<path d="M800,1165 V1215" stroke="%s" stroke-width="4" filter="url(#glow)"/>' % CY)
D.add('<path d="%s" fill="#0a1020" stroke="#19e6ff" stroke-width="3" filter="url(#glow)"/>' % cham(306, 1203, 988, 296, 26))
D.put('browser', b_browser(DK, CY, 960, 270, 'SPINE.TERM  ·  TRACK BROWSER', nrows=5), 320, 1215)
D.put('rec', b_rec(T), 1330, 1215)
D.put('appbtns', b_appbtns(T, 'col'), 1330, 1265)
D.put('status', b_status(DK), 540, 1530)
D.save(os.path.dirname(__file__))
