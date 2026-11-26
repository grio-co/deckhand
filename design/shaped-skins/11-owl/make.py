import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1980
PALS = {
 'night': dict(b1='#4a3d58', b2='#1a1322', b3='#0b0710', edge='#ff9a2a', glow='#ffc23a', A='#ffc23a', B='#ff5a8a', plate='#17101f', ink='#f6ecff', dim='#cdbfe0', claw='#0a0610', beak='#c9791c', pad_off='#1d1428', wood1='#5a4030', wood2='#2a1c14', wave_bg='#07040c', brow='#0b0710', dim_l='#cdbfe0', tb='#ffffff'),
 'ice': dict(b1='#f2f8ff', b2='#b9d0e8', b3='#6f93b8', edge='#2f7fd0', glow='#8ff2ff', A='#2d9bff', B='#8a5cff', plate='#102038', ink='#eef6ff', dim='#1f4268', claw='#1a2a40', beak='#e8b84a', pad_off='#1a3052', wood1='#c9d6e4', wood2='#8aa0b8', wave_bg='#061226', brow='#10243c', dim_l='#b8d0ea', tb='#10243c'),
}
def feather(L, w):
    return 'M0,0 C%s,%s %s,%s %s,%s Q%s,0 %s,%s C%s,%s %s,%s 0,0Z' % (n(L * .25), n(-w * .28), n(L * .6), n(-w * .52), n(L * .88), n(-w * .36), n(L * 1.03), n(L * .88), n(w * .36), n(L * .6), n(w * .52), n(L * .25), n(w * .28))

def build(pk):
    P = PALS[pk]
    D = Doc(W, H)
    class Owl(Theme):
        ink = P['ink']; dim = P['dim']; btn_fill = P['plate']; btn_stroke = P['edge']; btn_text = P['ink']; accent = P['A']; on_text = '#150a02'
        colA = P['A']; colB = P['B']; knob_fill = P['plate']; knob_stroke = P['edge']; track = '#05030a'; wave_bg = P['wave_bg']; pad_off = P['pad_off']; halo = ''
    T = Owl()
    T2 = Owl(); T2.dim = P['dim_l']
    CA, CB, ED = P['A'], P['B'], P['edge']
    D.d('<linearGradient id="bd" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/><stop offset=".55" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>' % (P['b1'], P['b2'], P['b3']))
    D.d('<radialGradient id="iris" cx="50%%" cy="45%%" r="55%%"><stop offset="0" stop-color="%s"/><stop offset=".6" stop-color="%s"/><stop offset="1" stop-color="%s"/></radialGradient>' % ('#fff6c8' if pk == 'night' else '#e8fcff', P['glow'], P['edge']))
    D.d('<filter id="glow" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="6" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>')
    D.d('<linearGradient id="wood" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>' % (P['wood1'], P['wood2']))
    # ---------- wings: primaries (feather-shaped pads) behind coverts
    PIV = (500, 500)
    angs = [174 - 8 * i for i in range(8)]
    for side, d, col in ((0, 'A', CA), (1, 'B', CB)):
        for i, a0 in enumerate(angs):
            a = a0 if side == 0 else 180 - a0
            L = 470 - i * 9
            px = PIV[0] if side == 0 else W - PIV[0]
            lit = i in DECK[d]['lit']
            fillc = HC[i] if lit else P['pad_off']
            D.add('<g transform="translate(%d,%d) rotate(%s)"><path d="%s" fill="%s" stroke="%s" stroke-width="3.5" stroke-linejoin="round"/><path d="M20,0 L%s,0" stroke="%s" stroke-width="2" opacity=".5"/></g>' % (px, PIV[1], n(a), feather(L, 58), fillc, HC[i] if not lit else '#fff', n(L * .95), '#000' if lit else ED))
            lx, ly = polar(px, PIV[1], L * .76, a)
            D.add(T.text(lx, ly + 6, str(i + 1), 18, '#150a02' if lit else HC[i], weight='800'))
        D.names['pads ' + d] = ((30 if side == 0 else 1100), 520, 470, 420)
    # ---------- coverts (leading-edge arm)
    for side in (0, 1):
        pts = [(570, 470), (470, 396), (330, 336), (200, 326), (90, 372), (30, 450), (40, 530), (300, 536), (520, 530)]
        pts = pts if side == 0 else [(W - x, y) for x, y in pts]
        D.add('<path d="%s" fill="url(#bd)" stroke="%s" stroke-width="4" stroke-linejoin="round"/>' % (smooth(pts, k=.35), ED))
        for r in range(4):
            for c in range(9):
                x = 80 + c * 50 + (r % 2) * 25 + (0 if side == 0 else 0)
                xx = x if side == 0 else W - x
                D.add('<path d="M%d,%d q12,16 24,0" stroke="%s" stroke-opacity=".28" stroke-width="2" fill="none" transform="translate(%d,0)"/>' % (xx - 12, 400 + r * 28 - (abs(x - 300) // 40) * 4, ED, 0))
    # ---------- body, thighs, head
    D.add('<path d="M620,540 Q800,500 980,540 Q1040,800 990,1010 Q960,1100 800,1110 Q640,1100 610,1010 Q560,800 620,540Z" fill="url(#bd)" stroke="%s" stroke-width="5"/>' % ED)
    for r in range(16):
        for c in range(7):
            x = 650 + c * 50 + (r % 2) * 25; y = 570 + r * 34
            D.add('<path d="M%d,%d q12,16 24,0" stroke="%s" stroke-opacity=".22" stroke-width="2" fill="none"/>' % (x, y, ED))
    D.add('<path d="M800,96 C700,70 520,70 430,170 C372,250 372,400 430,480 C500,570 640,590 740,545 L800,560 L860,545 C960,590 1100,570 1170,480 C1228,400 1228,250 1170,170 C1080,70 900,70 800,96Z" fill="url(#bd)" stroke="%s" stroke-width="5"/>' % ED)
    # ear tufts
    for sx in (1, -1):
        X = (lambda x: x) if sx == 1 else (lambda x: W - x)
        D.add('<path d="M%d,150 L%d,10 L%d,130 L%d,60 L%d,160Z" fill="url(#bd)" stroke="%s" stroke-width="4" stroke-linejoin="round"/>' % (X(470), X(440), X(540), X(560), X(620), ED))
    # brows (angry slabs) holding read-outs
    for sx, d, col in ((1, 'A', CA), (-1, 'B', CB)):
        X = (lambda x: x) if sx == 1 else (lambda x: W - x)
        D.add('<path d="M%d,120 L%d,98 L%d,170 L%d,240 L%d,200 L%d,215Z" fill="%s" stroke="%s" stroke-width="3.5" stroke-linejoin="round"/>' % (X(395), X(560), X(750), X(750), X(560), X(395), P['brow'], ED))
        Dd = DECK[d]
        tx = 432 if sx == 1 else W - 432
        an = 'start' if sx == 1 else 'end'
        D.add(T.text(tx, 148, Dd['title'], 20, '#fff' if pk == 'night' else '#fff', an, '700') + T.text(tx, 170, Dd['artist'], 13, P['dim_l'], an))
        D.names['info ' + d] = (400, 110, 340, 100)
    # eyes
    EY, ER = 372, 140
    for cx, d, col in ((580, 'A', CA), (1020, 'B', CB)):
        D.add('<circle cx="%d" cy="%d" r="%d" fill="%s" stroke="%s" stroke-width="6"/>' % (cx, EY, ER + 12, P['b3'], ED))
        D.add('<circle cx="%d" cy="%d" r="%d" fill="url(#iris)" filter="url(#glow)"/>' % (cx, EY, ER))
        D.add('<circle cx="%d" cy="%d" r="%d" fill="none" stroke="#000" stroke-opacity=".35" stroke-width="30"/>' % (cx, EY, ER - 18))
        D.add('<path d="%s" stroke="#fff" stroke-width="9" fill="none" stroke-linecap="round" opacity=".95"/>' % arc(cx, EY, ER - 18, 110, 300 if d == 'A' else 250))
        mp = polar(cx, EY, ER - 18, 235 if d == 'A' else 305)
        D.add('<circle cx="%s" cy="%s" r="11" fill="#150a02" stroke="#fff" stroke-width="3"/>' % (n(mp[0]), n(mp[1])))
        D.add('<circle cx="%d" cy="%d" r="64" fill="#040208"/>' % (cx, EY))
        D.put('jog ' + d, ('', 2 * ER, 2 * ER), cx - ER, EY - ER)
        Dd = DECK[d]
        D.add(T.text(cx, EY - 92, 'DECK ' + d, 15, '#150a02', weight='800') if pk == 'night' else T.text(cx, EY - 92, 'DECK ' + d, 15, '#06121e', weight='800'))
        D.add('<rect x="%d" y="%d" width="170" height="64" rx="12" fill="#06030c" fill-opacity=".78"/>' % (cx - 85, EY + 70))
        D.add(T.text(cx - 40, EY + 106, Dd['bpm'], 28, col, weight='800', mono=True) + T.text(cx + 48, EY + 98, Dd['t'], 14, '#fff', 'middle', '700', mono=True) + T.text(cx + 48, EY + 118, Dd['rem'], 12, P['dim_l'], 'middle', mono=True))
    # waveform band through both eyes
    D.add('<rect x="450" y="332" width="700" height="80" rx="14" fill="%s" fill-opacity=".92" stroke="%s" stroke-width="3"/>' % (P['wave_bg'], ED))
    D.add('<clipPath id="bw"><rect x="454" y="335" width="692" height="74" rx="11"/></clipPath><g clip-path="url(#bw)">' + wave(454, 337, 346, 70, 11, CA, '#fff', head=126, cues=[.08, .22, .5, .8], pitch=4, labels=False) + wave(800, 337, 346, 70, 29, CB, '#fff', head=220, cues=[.1, .3, .62], pitch=4, labels=False) + '</g>')
    D.names['wave'] = (454, 337, 692, 70)
    # beak: master
    D.add('<path d="M766,410 Q800,392 834,410 Q846,470 816,512 Q800,556 784,516 Q754,470 766,410Z" fill="%s" stroke="#000" stroke-opacity=".5" stroke-width="3"/>' % P['beak'])
    D.add(T.knob(800, 446, 21, .72, None, '#fff') + T.text(800, 492, 'MASTER', 11, '#150a02', weight='800'))
    D.add('<rect x="712" y="506" width="52" height="50" rx="10" fill="#06030c" fill-opacity=".8"/><rect x="836" y="506" width="52" height="50" rx="10" fill="#06030c" fill-opacity=".8"/>')
    D.add(T2.knob(738, 528, 15, .5, 'HP LVL', '#fff') + T2.knob(862, 528, 15, .35, 'HP MIX', '#fff'))
    D.add(T.meter(878, 505, 56, 9, .78, True, 10) + T.meter(878, 519, 56, 9, .72, True, 10)) if False else None
    D.names['master'] = (716, 424, 168, 130)
    # chest: crossfader + mixer
    D.put('xfader', b_xfader(T, w=330), 635, 570)
    D.put('ch1', b_strip(T, 1, CA, 90, 50), 650, 646)
    D.put('ch2', b_strip(T, 2, CB, 90, 50), 854, 646)
    D.add(T.meter(782, 700, 12, 260, .8) + T.meter(806, 700, 12, 260, .74) + T.text(800, 990, 'MASTER', 11, P['dim'], weight='700'))
    # coverts controls (tempo along leading edge, loops, beat jump, key shift)
    for side, d, col in ((0, 'A', CA), (1, 'B', CB)):
        s = 1 if side == 0 else -1
        X = (lambda x: x) if side == 0 else (lambda x: W - x)
        D.add(curve_fader(T, [(X(430), 398), (X(370), 356), (X(290), 340), (X(190), 372)], .5 + (.04 if d == 'A' else -.03), col, w=12, cap=(44, 30), fill=ED))
        D.add(T.btn(X(150) - (0 if side == 0 else 56), 394, 56, 30, '±8%', False, col, 12) if False else '')
        loop = b_loop(T, d, col, 'row'); kj = b_keyshift(T, d, col)
        lx = 190 if side == 0 else W - 190 - loop[1]
        D.add('<g transform="translate(%d,430)">%s</g>' % (lx, loop[0]))
        jx = 190 if side == 0 else W - 190 - 110
        D.add('<g transform="translate(%d,474)">%s</g>' % (jx, T.btn(0, 0, 34, 30, '◂◂', False, col, 12) + T.btn(38, 0, 34, 30, '8', False, col, 13) + T.btn(76, 0, 34, 30, '▸▸', False, col, 12)))
        kx = 310 if side == 0 else W - 310 - 132
        D.add('<g transform="translate(%d,474)">%s</g>' % (kx, kj[0]))
        D.add(T.btn(X(100) - (0 if side == 0 else 56), 436, 56, 30, '±8%', False, col, 12))
        D.add(T.text(X(128), 480, '+0.0%' if d == 'A' else '-1.5%', 12, P['tb'], mono=True, weight='700'))
        D.names['loop ' + d] = (lx, 430, 194, 30); D.names['tempo ' + d] = (X(190) - (0 if side == 0 else 300), 330, 330, 80)
    # talons: toes are transport (outer -> inner: LOAD, SYNC, PLAY, CUE)
    for side, d, col in ((0, 'A', CA), (1, 'B', CB)):
        s = 1 if side == 0 else -1
        ax, ay = (650 if side == 0 else W - 650), 1068
        D.add('<ellipse cx="%d" cy="%d" rx="58" ry="40" fill="url(#bd)" stroke="%s" stroke-width="4"/>' % (ax, ay, ED))
        labels = ['LOAD', 'SYNC', 'PLAY', 'CUE']
        for k, lab in enumerate(labels):
            a = [160, 126, 92, 58][k] if side == 0 else 180 - [160, 126, 92, 58][k]
            on = (lab == 'PLAY' and d == 'A') or (lab == 'SYNC' and d == 'B')
            D.add('<g transform="translate(%d,%d) rotate(%s)"><path d="M0,-12 Q-4,0 0,12 L26,16 L140,22 Q158,0 140,-22 L26,-16Z" fill="%s" stroke="%s" stroke-width="3.5" stroke-linejoin="round"/><path d="M140,-16 Q196,-6 204,44 Q178,14 140,16Z" fill="%s" stroke="%s" stroke-width="2.5"/></g>' % (ax, ay, n(a), col if on else P['plate'], col, P['claw'], '#fff'))
            bx, by = polar(ax, ay, 90, a)
            if lab == 'PLAY':
                D.add('<path d="M%s,%s l0,24 l22,-12z" fill="%s" transform="translate(-9,-12)"/>' % (n(bx), n(by), '#150a02' if on else '#fff'))
            else:
                D.add(T.text(bx, by + 5, lab, 14, '#150a02' if on else '#fff', weight='800'))
        D.names['transport ' + d] = (ax - 150, ay - 20, 300, 160)
    # ---------- floating modules
    # perch branch = browser
    D.add('<rect x="40" y="1420" width="1000" height="330" rx="70" fill="url(#wood)" stroke="%s" stroke-width="5"/>' % ED)
    D.add('<ellipse cx="60" cy="1585" rx="40" ry="165" fill="%s" stroke="%s" stroke-width="4"/><ellipse cx="60" cy="1585" rx="22" ry="100" fill="none" stroke="%s" stroke-width="3" opacity=".6"/>' % (P['wood1'], ED, ED))
    D.add('<path d="M120,1440 q-30,60 20,120 M960,1736 q40,-30 20,-100" stroke="#000" stroke-opacity=".3" stroke-width="4" fill="none"/>')
    D.add(grip(540, 1436, P['ink']))
    D.put('browser', b_browser(T2, CA, 880, 270, 'PERCH  ·  TRACK BROWSER', nrows=5), 100, 1460)
    # shed feather = effects rack
    D.add('<g transform="rotate(-5 1310 1480)"><path d="M1040,1480 C1160,1320 1440,1310 1580,1480 C1440,1650 1160,1640 1040,1480Z" fill="url(#bd)" stroke="%s" stroke-width="4"/><path d="M1040,1480 H1580" stroke="%s" stroke-width="3" opacity=".5"/></g>' % (ED, ED))
    fa = b_fx(T, 'A', CA); fb = b_fx(T, 'B', CB)
    D.add('<g transform="translate(1118,1432) rotate(-5)">%s</g><g transform="translate(1330,1432) rotate(-5)">%s</g>' % (fa[0], fb[0]))
    D.names['fx A'] = (1118, 1432, 196, 96); D.names['fx B'] = (1330, 1432, 196, 96)
    D.add(grip(1310, 1366, P['ink']))
    # pellet = sampler + app buttons
    D.add('<ellipse cx="1300" cy="1790" rx="250" ry="120" fill="url(#bd)" stroke="%s" stroke-width="4"/>' % ED)
    D.add(grip(1300, 1682, P['ink']))
    D.put('sampler', b_sampler(T), 1199, 1730)
    D.put('appbtns', b_appbtns(T), 1210, 1840)
    D.put('status', b_status(T), 30, 1930)
    return D

if __name__ == '__main__':
    here = os.path.dirname(__file__)
    D = build('night')
    D.save(here)
    D2 = build('ice')
    open(os.path.join(here, '_ice.svg'), 'w').write(D2.svg(True))
    render(os.path.join(here, '_ice.svg'), os.path.join(here, 'mockup-ice.png'), W, H)
    os.remove(os.path.join(here, '_ice.svg'))
