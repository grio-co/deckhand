import os, subprocess
from lib import CHROME
NAMES = [('01-anatomical-ivory', 'Anatomical Ivory'), ('02-sugar-skull', 'Sugar Skull'), ('03-neon-chrome', 'Neon Chrome'), ('04-pixel-art', 'Pixel Art'),
         ('05-pirate-flag', 'Pirate Scraps'), ('06-steampunk', 'Steampunk'), ('07-crystal-glass', 'Crystal Glass'), ('08-tiki-mask', 'Tiki Mask'),
         ('09-wireframe', 'Wireframe'), ('10-xray-open-jaw', 'X-Ray Open Jaw (owner request)')]
here = os.path.dirname(os.path.abspath(__file__))
cells = ''.join('<div class="c"><img src="%s/mockup.png"><div class="l"><b>%d</b> %s</div></div>' % (d, i + 1, t) for i, (d, t) in enumerate(NAMES))
html = '<html><body style="margin:0;background:#1b1e26;font-family:Helvetica,Arial,sans-serif"><style>.g{display:grid;grid-template-columns:repeat(5,480px);gap:0}.c{width:480px;height:640px;position:relative}.c img{width:480px;height:600px;object-fit:contain}.l{position:absolute;left:0;right:0;bottom:4px;text-align:center;color:#fff;font-size:20px}.l b{background:#fff;color:#000;border-radius:12px;padding:1px 9px;margin-right:6px}</style><div class="g">%s</div></body></html>' % cells
open(os.path.join(here, '_contact.html'), 'w').write(html)
subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--screenshot=' + os.path.join(here, 'contact-sheet.png'), '--window-size=2400,1280', 'file://' + os.path.join(here, '_contact.html')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
os.remove(os.path.join(here, '_contact.html'))
