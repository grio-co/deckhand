import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from lib import *

W, H = 1600, 1840
D = Doc(W, H)
MI, AM, GD = '#6ff0d8', '#ffb15e', '#e6c25a'
class Scarab(Theme):
    ink = '#eafff8'; dim = '#b9e6da'; btn_fill = '#0c2a2c'; btn_stroke = GD; btn_text = '#eafff8'; accent = MI; on_text = '#021311'
    colA = MI; colB = AM; knob_fill = '#123a3a'; knob_stroke = GD; track = '#04100f'; wave_bg = '#04100f'; halo = '#04201f'
T = Scarab()
D.d('<linearGradient id="shell" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#37c7a8"/><stop offset=".5" stop-color="#127a6e"/><stop offset="1" stop-color="#0a3d49"/></linearGradient>')
D.d('<linearGradient id="shellB" x1="1" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e8a24a"/><stop offset=".5" stop-color="#9a4f2a"/><stop offset="1" stop-color="#4a2a3a"/></linearGradient>')
D.d('<linearGradient id="body" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#0f1f1d"/><stop offset=".5" stop-color="#27403b"/><stop offset="1" stop-color="#0f1f1d"/></linearGradient>')
# legs
for sx in (1, -1):
    X = (lambda x: x) if sx == 1 else (lambda x: W - x)
    for (y0, kx, ky, ex, ey) in ((420, 130, 300, 40, 470), (700, 60, 700, 30, 880), (980, 140, 1080, 70, 1260)):
        D.add('<path d="M%s,%d L%s,%d L%s,%d" stroke="#0b1614" stroke-width="22" fill="none" stroke-linecap="round" stroke-linejoin="round"/>' % (n(X(640)), y0, n(X(kx)), ky, n(X(ex)), ey))
# head, pronotum, abdomen
D.add('<path d="M740,120 L700,40 M860,120 L900,40" stroke="url(#body)" stroke-width="16" stroke-linecap="round"/><ellipse cx="800" cy="95" rx="80" ry="52" fill="url(#body)" stroke="%s" stroke-width="3"/>' % GD)
D.add('<path d="M640,150 Q800,100 960,150 L990,340 Q800,380 610,340Z" fill="url(#body)" stroke="%s" stroke-width="4"/>' % GD)
D.add('<path d="M660,340 Q800,360 940,340 Q980,760 900,1020 Q800,1240 700,1020 Q620,760 660,340Z" fill="url(#body)" stroke="%s" stroke-width="4"/>' % GD)
# opened elytra (rotated outward)
for sx, d, col, fillid in ((1, 'A', MI, 'shell'), (-1, 'B', AM, 'shellB')):
    cx = 380 if sx == 1 else W - 380
    D.add('<g transform="rotate(%d %d 700)"><ellipse cx="%d" cy="700" rx="335" ry="575" fill="url(#%s)" stroke="%s" stroke-width="6"/>' % (-5 * sx, cx, cx, fillid, GD) +
          ''.join('<ellipse cx="%d" cy="700" rx="%d" ry="%d" fill="none" stroke="#fff" stroke-opacity=".12" stroke-width="3"/>' % (cx, 335 - k * 34, 575 - k * 34) for k in range(1, 4)) + '</g>')
# deck A on left elytra, B mirrored
JX, JY, JR = 390, 520, 140
for cx, d, col in ((JX, 'A', MI), (W - JX, 'B', AM)):
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#08201f" stroke="%s" stroke-width="5"/><circle cx="%d" cy="%d" r="%d" fill="none" stroke="#0e3b38" stroke-width="30"/>' % (cx, JY, JR, GD, cx, JY, JR - 22))
    D.add('<path d="%s" stroke="%s" stroke-width="9" fill="none" stroke-linecap="round"/>' % (arc(cx, JY, JR - 22, 120, 300 if d == 'A' else 240), col))
    ma = 235 if d == 'A' else 305
    D.add('<circle cx="%s" cy="%s" r="11" fill="%s" stroke="#fff" stroke-width="2.5"/>' % (n(cx + (JR - 22) * math.cos(math.radians(ma))), n(JY + (JR - 22) * math.sin(math.radians(ma))), col))
    D.add('<circle cx="%d" cy="%d" r="%d" fill="#04100f"/>' % (cx, JY, JR - 48))
    Dd = DECK[d]
    D.add(T.text(cx, JY - 18, Dd['title'], 14, '#fff', weight='700', halo='') + T.text(cx, JY + 22, Dd['bpm'], 32, col, weight='800', mono=True, halo='') + T.text(cx, JY + 44, Dd['t'] + '  ' + Dd['key'], 12, '#b9e6da', mono=True, halo=''))
    D.put('jog ' + d, ('', 2 * JR, 2 * JR), cx - JR, JY - JR)
    D.add('<rect x="%d" y="236" width="360" height="100" rx="14" fill="#04100f" stroke="%s" stroke-width="3"/>' % (cx - 180, GD))
    D.add('<clipPath id="e%s"><rect x="%d" y="240" width="352" height="92" rx="12"/></clipPath><g clip-path="url(#e%s)">' % (d, cx - 176, d) + wave(cx - 176, 244, 352, 84, Dd['seed'], col, '#fff', head=176, cues=Dd['cues'][:3], pitch=4) + '</g>')
D.names['wave'] = (214, 244, 352, 84)
both(D, T, 'pads', b_pads, 285, 690, s=48)
both(D, T, 'transport', b_transport, 285, 808)
both(D, T, 'loop', b_loop, 285, 866)
both(D, T, 'tempo', b_tempo, 106, 400, h=170)
both(D, T, 'info', b_info, 5000, 5000) if False else None
# pronotum: master etc
D.put('master', b_master(T), 678, 158)
D.put('mic', b_mic(T), 710, 262)
D.put('appbtns', b_appbtns(T), 710, 306) if False else None
# abdomen: mixer
D.put('ch1', b_strip(T, 1, MI, 90, 50), 704, 392)
D.put('ch2', b_strip(T, 2, AM, 90, 50), 800, 392)
D.put('xfader', b_xfader(T, w=220), 690, 818)
D.put('rec', b_rec(T), 761, 920)
D.put('sampler', b_sampler(T), 699, 978)
D.put('appbtns', b_appbtns(T), 710, 1100)
# floating closed-state compact module
cx0, cy0 = 80, 1340
D.add('<clipPath id="cl"><rect x="%d" y="%d" width="210" height="480"/></clipPath><clipPath id="cr"><rect x="%d" y="%d" width="210" height="480"/></clipPath><ellipse cx="%d" cy="%d" rx="210" ry="222" fill="url(#shell)" clip-path="url(#cl)"/><ellipse cx="%d" cy="%d" rx="210" ry="222" fill="url(#shellB)" clip-path="url(#cr)"/><ellipse cx="%d" cy="%d" rx="210" ry="222" fill="none" stroke="%s" stroke-width="5"/>' % (cx0, cy0, cx0 + 210, cy0, cx0 + 210, cy0 + 235, cx0 + 210, cy0 + 235, cx0 + 210, cy0 + 235, GD))
D.add('<path d="M%d,%d V%d" stroke="%s" stroke-width="6"/>' % (cx0 + 210, cy0 + 20, cy0 + 450, GD))
D.add(grip(cx0 + 210, cy0 + 4, GD))
D.add('<rect x="%d" y="%d" width="140" height="48" rx="8" fill="#04100f" stroke="%s"/><rect x="%d" y="%d" width="140" height="48" rx="8" fill="#04100f" stroke="%s"/>' % (cx0 + 40, cy0 + 90, GD, cx0 + 250, cy0 + 90, GD))
D.add(wave(cx0 + 40, cy0 + 94, 140, 40, 11, MI, '#fff', head=70, pitch=3, grid=False, labels=False) + wave(cx0 + 250, cy0 + 94, 140, 40, 29, AM, '#fff', head=70, pitch=3, grid=False, labels=False))
D.add(T.btn(cx0 + 40, cy0 + 170, 66, 44, '', True, MI, 12, 'play') + T.btn(cx0 + 114, cy0 + 170, 66, 44, 'CUE', False, MI, 13) + T.btn(cx0 + 250, cy0 + 170, 66, 44, '', False, AM, 12, 'play') + T.btn(cx0 + 324, cy0 + 170, 66, 44, 'CUE', False, AM, 13))
D.add(T.text(cx0 + 210, cy0 + 300, 'CLOSED / COMPACT', 11, '#fff', weight='700') + T.text(cx0 + 210, cy0 + 318, 'drag the seam to open', 11, '#cfeee6'))
D.add(T.fader_h(cx0 + 90, cy0 + 340, 240, 36, .46, '#fff'))
D.add(T.btn(cx0 + 160, cy0 + 392, 100, 34, 'OPEN ▲', False, GD, 12))
D.names['closed state'] = (cx0, cy0, 420, 460)
# floating dung-ball browser (circular window)
bx, by, br = 1060, 1530, 262
D.add('<circle cx="%d" cy="%d" r="%d" fill="#5a4630" stroke="%s" stroke-width="6"/>' % (bx, by, br, GD))
for k in range(14):
    a = math.radians(k * 25.7)
    D.add('<path d="M%s,%s L%s,%s" stroke="#7a6240" stroke-width="3"/>' % (n(bx + 200 * math.cos(a)), n(by + 200 * math.sin(a)), n(bx + 252 * math.cos(a)), n(by + 252 * math.sin(a))))
D.add(grip(bx, by - br + 14, GD))
D.put('browser', b_browser(T, MI, 400, 290, 'DUNG BALL  ·  BROWSER', nrows=5), bx - 200, by - 145)
D.put('status', b_status(T), 540, 1770) if False else None
D.put('status', b_status(T), 560, 1806)
D.save(os.path.dirname(__file__))
