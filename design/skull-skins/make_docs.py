"""Generates each concept's notes.md, README.md and index.html from the data below (standard library only)."""
import os, html
here = os.path.dirname(os.path.abspath(__file__))

NEED = {
 'shaped_win': 'Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)',
 'float_win': 'Multiple independent floating windows with remembered positions, optional snapping/docking to each other',
 'drag': 'Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)',
 'ring_jog': 'Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside',
 'clip_wave': 'Waveform (and text) widgets clipped to a circle/polygon (eye socket, visor, lens) instead of a rectangle',
 'shape_btn': 'Per-widget shape outlines for buttons/pads (tooth, chamfer, gem, petal) with per-pixel hit testing',
 'group_anim': 'Animations on containers (transform/opacity of a group including children): jaw drop/sway, eye pulse, vertebra bounce',
 'meter_bind': 'Style properties (glow, opacity, scale, rotate) bindable to level meters and beat phase (eye glow follows level, teeth light as VU)',
 'wave_styles': 'Waveform layouts: stacked lanes (3-band), mirrored shared-centreline halves, outline/line mode, glow beam',
 'curve_slider': 'Slider/fader travelling along a path (arc jaw rail, diagonal bone) with matching hit-testing',
 'glow': 'Soft glow / blur and additive blend per widget',
 'gauge': 'Value-driven rotation (needle gauges bound to meters)',
 'pixel': 'Nearest-neighbour integer scaling and a bitmap font (no smoothing) for the pixel look',
 'rot_widget': 'Rotated widgets (fader caps and knob rows on a diagonal)',
}
ALL = ['shaped_win', 'float_win', 'drag']

C = [
 dict(n='01', d='01-anatomical-ivory', name='Anatomical Ivory', style='Clean vector, ivory bone on charcoal plates', win='Skull + ribcage browser + status pill', dens='Medium', hard='Shaped window with holes', use=(4, 'plain labels, every region has one job'),
  idea='A restrained, anatomically literal ivory skull: each eye is a jog ring around a dark socket, and one continuous waveform band runs through both sockets and across the nasal bridge with a playhead at each pupil. Controls sit on dark engraved plates so nothing competes with the bone.',
  rows=[('Eye sockets', 'Jog ring (marker, progress arc), waveform band with playhead at pupil, deck letter'), ('Forehead / cranium', 'Title, artist, BPM, key, time, remaining, mini overview; app buttons and REC at the crown'),
        ('Cheekbones', 'Play / cue / sync, key shift, key lock / quantise / slip / reverse, loop in/out/size, pad modes'), ('Ears', 'Tempo fader + range'), ('Nasal bridge / cavity', 'Mic on / talk / level; master, headphone level, headphone mix, master meter'),
        ('Upper teeth', '16 teeth = 8 hot-cue pads A | 8 pads B'), ('Lower teeth', '16 sampler pads'), ('Mandible', 'Two channel rows (trim, EQ, filter, fader, meters, cue) and crossfader'), ('Vertebrae', '3 vertebrae = 3 FX slots, deck A left wing, deck B right wing')],
  anim='Jog marker and ring progress turn with the platter; pupil playheads; teeth glow as pads; band amplitude breathes on the beat; jaw nudges 2 px with the kick.',
  float='Ribcage browser (rib-cage silhouette with grip) and a status pill float below the spine.', shapes='Browser: rib cage. Status: pill.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'clip_wave', 'shape_btn', 'group_anim'], extra='Band that crosses from one socket across bone to the other is a single wide widget with two playheads.',
  hardu='Hardest to use: 16-tooth rows need 58 px pitch; the FX rack sits far from the decks. Easiest: strict one-job-per-region mapping, large pads, readable text.'),
 dict(n='02', d='02-sugar-skull', name='Sugar Skull', style='Dia de los Muertos ornament, flowers, papel picado', win='Skull + bunting-framed browser', dens='Dense, decorative', hard='Petal/tooth shaped widgets', use=(3, 'ornament competes with labels'),
  idea='A sugar skull whose eyes are flower rosettes that spin as the jog while each holds its own waveform lane; every knob is a marigold flower. Teeth are real buttons (both rows), pads move to the cheeks, and a strand of beat beads links the eyes.',
  rows=[('Eye rosettes', 'Jog (petal ring + marker), waveform lane through the socket centre'), ('Forehead', 'Info, overview, master / headphone knobs (flowers) and master meter'), ('Crown flowers', '8 sampler pads'),
        ('Ears', 'FX rows (3 slots) + tempo fader and range'), ('Cheekbones', '8 hot-cue pads per deck (rounded), pad modes'), ('Upper teeth', 'Play, cue, sync, key lock, quantise, slip, reverse, range'), ('Lower teeth', 'Loop in/out/size, key shift -/+, eject'),
        ('Nasal bridge', 'Mic, REC, settings / skin / language, beat garland'), ('Jaw', 'Two channel rows with flower knobs, crossfader')],
  anim='Rosettes rotate with the jog; flowers pulse with the kick; garland beads light on beats; petals on a channel knob bloom with level.',
  float='Browser on an ofrenda panel strung with papel-picado bunting; status pill.', shapes='Browser: bunting-topped plaque. Status: pill.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'clip_wave', 'shape_btn', 'group_anim', 'meter_bind'], extra='Flower-shaped knobs need a petal sprite that rotates with value.',
  hardu='Harder than default: labels on busy ornament, buttons moved onto teeth make the grouping unfamiliar. Easier: huge round targets and strong colour per function.'),
 dict(n='03', d='03-neon-chrome', name='Neon Chrome', style='Faceted chrome skull, neon edge glow, holographic visors', win='Skull + terminal browser + app strip', dens='Dense', hard='Glow and polygon clipping', use=(4, 'high contrast, clear lanes'),
  idea='A low-poly chrome skull with slanted holographic visors for eyes: the two decks share one band of stacked lanes (A on top, B below) that scrolls through both visors with a single playhead at the nasal bridge. Jog platters are HUD discs on the cheekbones; the ears are cyber headphone cups full of controls.',
  rows=[('Visors', 'Stacked waveform lanes A / B, central playhead'), ('Forehead', 'Info, overview, key lock / quantise / slip / reverse, loop, key shift, master section'), ('Cheekbones', 'Jog HUD disc, transport (vertical), pad modes'),
        ('Headphone ears', 'FX rows, 8 hot-cue pads, tempo + range'), ('Nose', 'Mic on / talk / level'), ('Upper teeth', '16 sampler pads'), ('Lower teeth', 'Master level L/R as 16 lit segments'), ('Mandible', 'Channel rows, crossfader'), ('Spine terminal (floating)', 'Track browser; REC and app buttons beside it')],
  anim='Edge glow follows master level; lower teeth are a live VU; playhead pulses on beat; HUD arcs turn with the jog.',
  float='Terminal browser (chamfered window), REC, settings / skin / language strip (small separate window).', shapes='Browser: chamfered rectangle. App strip: chamfered capsule.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'clip_wave', 'shape_btn', 'wave_styles', 'glow', 'meter_bind'], extra='Shared two-lane waveform with one playhead is one widget with two decks bound.',
  hardu='Harder: the shared playhead hides each deck\'s own position until you learn it. Easier: very high contrast, deck colours obvious, pads and jog well separated.'),
 dict(n='04', d='04-pixel-art', name='Pixel Art', style='8/16-bit pixels, chunky, playful', win='Skull + save-file browser + arcade corner', dens='Medium, big targets', hard='Pixel-exact scaling', use=(3, 'big and clear, long jaw makes mixer far from decks'),
  idea='A chunky pixel skull on a 20 px grid whose sockets are small arcade screens: waveform lane above, a stepped-disc platter below. The jaw is long so the mixer fits as two vertical strips, like a classic channel pair.',
  rows=[('Eye sockets', 'Waveform lane + jog platter (pixel disc, square marker)'), ('Forehead', 'Info, overview, key lock / quantise / slip / reverse'), ('Cheekbones', 'Transport, key shift, loop, pad modes'), ('Ears', 'FX rows, tempo + range'),
        ('Nose', 'Master section, mic'), ('Upper teeth', '16 hot-cue pads'), ('Lower teeth', '16 sampler pads'), ('Long jaw', 'Two vertical channel strips, crossfader at the chin'), ('Floating', 'Save-file browser; REC, app buttons, title banner')],
  anim='Platter turns in 30 degree steps; pads blink at 2 fps; eyes blink every 8 beats; teeth flash on hits.',
  float='Save-file browser and an arcade corner (REC, settings / skin / language).', shapes='Browser: pixel-notched window. Corner: loose pixel buttons.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'pixel', 'group_anim'], extra='Skin must keep to integer scale factors so pixels stay square.',
  hardu='Easier: huge flat targets, no thin lines. Harder: stepped jog gives coarse visual feedback; waveform lane is small.'),
 dict(n='05', d='05-pirate-flag', name='Pirate Scraps', style='Jolly Roger skull, crossed bones, brass and torn sailcloth', win='Skull + 4 cloth scraps + chart browser', dens='Dense, scattered', hard='Curved and diagonal faders', use=(2, 'diagonal faders and scattered scraps'),
  idea='A bandana-wearing skull in brass portholes with crossed bones underneath: the bones are the mixer, with EQ knobs strung along each bone and a brass cap that slides diagonally for the channel fader. Deck controls live on four torn sailcloth scraps that float beside the skull.',
  rows=[('Portholes (eyes)', 'Jog ring with rivets, waveform lane'), ('Bandana', 'Master, headphone level / mix, master meter'), ('Cloth scrap A / B (floating)', 'Info, overview, transport, key shift, toggles, loop, pad modes'), ('Lower scraps (floating)', 'FX rows, tempo + range'),
        ('Upper teeth', '16 hot-cue pads'), ('Cheekbones', 'Sampler (left), mic + REC (right)'), ('Jaw', 'Crossfader on a curved brass rail'), ('Crossed bones', 'Trim, EQ, filter knobs; channel faders slide along the bones; meters and cue at the bone ends'), ('Floating', 'Treasure-map browser, app buttons')],
  anim='Bandana tail flutters; portholes glint; knobs on a bone turn as set; scraps sway slightly (container rotate).',
  float='Four cloth scraps (deck A, deck B, FX A, FX B), treasure-map browser, app buttons.', shapes='Scraps: torn sailcloth with stitched border. Browser: torn parchment map.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'clip_wave', 'shape_btn', 'curve_slider', 'rot_widget', 'group_anim'], extra='Channel faders are sliders along a straight but rotated track.',
  hardu='Hardest of the set: diagonal faders are awkward and the deck panels are far from the skull. Easiest to read: big brass knobs.'),
 dict(n='06', d='06-steampunk', name='Steampunk', style='Aged bone, brass, rivets, needle gauges', win='Skull + brass console + ledger browser + sampler/app tray', dens='Dense', hard='Needle gauges and 3-band lanes', use=(3, 'rivets are busy; three-band lanes are small'),
  idea='Aged bone in brass goggles whose gear rims are the jog rings; each goggle shows low, mid and high bands as three stacked lanes. Brass trumpets for ears carry the effects and tempo, and the mixer is a separate riveted console with a vertebra stack for master and headphones.',
  rows=[('Goggles (eyes)', 'Gear-ring jog, 3-band stacked waveform lanes'), ('Forehead', 'Info, overview, L/R needle gauges (master level)'), ('Cheekbones', 'Transport, pad modes, key lock / quantise / slip / reverse'), ('Trumpet ears', 'FX rows, tempo + range'),
        ('Nose', 'Mic, REC'), ('Upper teeth', '16 hot-cue pads'), ('Lower teeth', 'Loop in/out/size, key shift, range'), ('Console (floating)', 'Two channel strips, crossfader; three vertebrae = master, headphone level, headphone mix'), ('Floating', 'Ledger browser, sampler + app tray')],
  anim='Gear rims rotate with the jog; gauge needles follow the master meter; steam-vent glow on the beat.',
  float='Brass console, ledger browser, tray with sampler and app buttons.', shapes='Console: riveted brass plate. Browser: brass-bound ledger. Tray: loose brass buttons.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'clip_wave', 'shape_btn', 'wave_styles', 'gauge', 'group_anim', 'meter_bind'], extra='Gear-toothed rim is just a sprite that rotates with position.',
  hardu='Harder: rivet detail is visual noise; stacked band lanes are only 34 px tall. Easier: gauges give master level at a glance, console is conventional.'),
 dict(n='07', d='07-crystal-glass', name='Crystal Glass', style='Translucent crystal, internal light, gem controls', win='Skull + shard browser + app tray', dens='Dense', hard='Blur/glow and blend', use=(3, 'translucency lowers label contrast on bright wallpapers'),
  idea='A faceted crystal skull lit from inside: the eyes are glass orbs and the waveform is a beam of light passing through both orbs and the bridge. Knobs and pads are cut gems, the brow ridge carries the transport, and the mixer is a spine of crystal segments.',
  rows=[('Orbs (eyes)', 'Jog ring with gem studs, light-beam waveform through both orbs'), ('Brow ridge', 'Play / cue / sync and key lock / quantise / slip / reverse'), ('Forehead', 'Info, overview'), ('Temples', 'Tempo + range'),
        ('Cheekbones', 'FX slots (3 columns)'), ('Nasal cavity / bridge', 'Master section; mic'), ('Jaw', '8 hot-cue pads + pad modes per deck'), ('Upper teeth', 'Loop in/out/size, key shift, range'), ('Lower teeth', '16 sampler pads'),
        ('Crystal spine', 'Trim / EQ / filter rows (A left, B right), crossfader, faders, meters, cue'), ('Floating', 'Shard browser, REC + app buttons')],
  anim='Internal glow follows master level; facets flash on pad hits; beam brightness follows amplitude; orb highlight drifts.',
  float='Crystal-shard browser, REC and app buttons tray.', shapes='Browser: irregular quadrilateral shard. Tray: gem-cut buttons.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'clip_wave', 'shape_btn', 'wave_styles', 'glow', 'meter_bind'], extra='Needs additive blending so the beam reads as light.',
  hardu='Harder: translucent skin changes contrast with the wallpaper; the beam hides cue flags in loud sections. Easier: spine rows make the EQ matrix obvious.'),
 dict(n='08', d='08-tiki-mask', name='Tiki Mask', style='Carved wood, inlay beads, feathers', win='Mask + hanging planks + totem browser', dens='Dense', hard='Mouth as mixer, mirrored lanes', use=(3, 'wood is dark but mouth mixer is far from eyes'),
  idea='A tall carved mask: the eyes are rings of coloured beads (the jog) and a single inlaid band runs through both with deck A growing upward and deck B downward from a shared centre line. The huge mouth is the mixer, framed by two rows of teeth.',
  rows=[('Eyes', 'Bead-ring jog, shared-centreline waveform A up / B down'), ('Forehead plaque', 'Info, overview, master section'), ('Feather crown', '8 sampler pads'), ('Nose bridge', 'Transport (vertical), mic, REC'), ('Cheeks', 'Toggles, loop, key shift, pad modes'),
        ('Ear discs', 'Tempo + range'), ('Hanging planks', 'FX rows'), ('Upper teeth (mouth)', '16 hot-cue pads'), ('Mouth cavity', 'Two channel rows, crossfader'), ('Lower teeth (mouth)', 'Loop in/out/size, key shift, range'), ('Floating', 'Totem-base browser, app buttons')],
  anim='Feathers sway on the beat; bead ring marker travels; mouth cavity glow follows level.',
  float='Totem-base browser, hanging FX planks, app buttons.', shapes='Browser: carved totem trapezoid. Planks: wooden tags on cords.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'wave_styles', 'group_anim'], extra='Mirrored waveform halves are two decks drawn on one centre line.',
  hardu='Harder: mirrored lanes make it unclear which half belongs to which deck at a glance. Easier: the mouth mixer is large and central.'),
 dict(n='09', d='09-wireframe', name='Wireframe Diagram', style='Single-line drawing on paper, almost a technical diagram', win='Skull + 2 deck panels + master panel + app panel + browser', dens='Light, maximum clarity', hard='Curved jaw slider', use=(5, 'high contrast, everything labelled, nothing hidden'),
  idea='The skull is only an outline: eyes are jog rings with a thin line-drawn waveform (one continuous line crossing the bridge), teeth are pads, and the crossfader rides the jaw curve. Every other control lives on clean paper panels that float beside it, so the drawing stays calm and usability stays maximal.',
  rows=[('Eyes', 'Jog ring, line-drawn waveform with playhead'), ('Upper teeth', '16 hot-cue pads'), ('Lower teeth', '16 sampler pads'), ('Jaw', 'Crossfader on a curved rail'), ('Deck panels A / B (floating)', 'Info, overview, transport, key shift, toggles, loop, pad modes, FX, tempo'),
        ('Master panel (floating)', 'Master, headphone, meter, mic, REC'), ('Under the chin', 'Two channel rows'), ('Floating', 'Browser panel, app buttons panel')],
  anim='Line waveform scrolls; teeth flip to colour on pad hit; jaw rail cap follows crossfader; nothing else moves.',
  float='Two deck panels, master/mic panel, app panel, browser.', shapes='Panels: rounded paper tiles with grip. Browser: same.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'clip_wave', 'shape_btn', 'curve_slider', 'wave_styles'], extra='Line-mode waveform (outline only) is the main new drawing mode.',
  hardu='Easiest of the ten: same density as the default skin, big text, high contrast. Hardest part is that six windows must be arranged by the user.'),
 dict(n='10', d='10-xray-open-jaw', name='X-Ray Open Jaw', style='Radiograph skull, caricature sockets, jaw dropped, cervical spine', win='Skull + X-ray film browser (only the browser floats)', dens='Highest, performance-first', hard='Ring jog with text/waveform inside', use=(4, 'largest targets, everything on one surface'),
  owner=True,
  idea='Owner\'s request: caricature-large eye sockets, the jaw dropped wide open and the full cervical spine on show. The sockets hold the whole deck read-out (title, big waveform, BPM, time, overview) inside a giant jog ring; the open mouth holds the mixer; seven vertebrae carry FX, master, faders, mic and app buttons, so only the browser floats.',
  rows=[('Giant sockets', 'Jog ring (464 px), 380x124 waveform with cues and playhead, title, artist, BPM, key, time, remaining, overview'), ('Brow', 'Play / cue / sync, key shift, key lock / quantise / slip / reverse, loop in/out/size'), ('Temples', 'Tempo + range'), ('Cheekbones', 'Pad modes'),
        ('Upper teeth', '16 hot-cue pads'), ('Open mouth', 'Two channel rows (trim, EQ, filter, fader, meters, cue)'), ('Lower teeth', 'Loop in/out/size, key shift, range'), ('Mandible', '8 sampler pads, crossfader along the jawbone'),
        ('Vertebrae C1-C3', 'FX slot 1-3, deck A left wing, deck B right wing'), ('C4', 'Master, headphone level / mix, master meter'), ('C5', 'Channel faders, meters, cue'), ('C6', 'Mic on / talk / level, REC'), ('C7', 'Settings, skin, language'), ('Floating', 'X-ray film browser (only floating module)')],
  anim='Jaw drops a few px on every kick; sockets glow with channel level; vertebrae light as their control is touched; jog ring marker turns with the platter.',
  float='Only the track browser floats (X-ray film sheet with grip, slightly tilted) plus the status pill.', shapes='Browser: X-ray film with rounded corners. Status: pill.',
  needs=['shaped_win', 'float_win', 'drag', 'ring_jog', 'clip_wave', 'shape_btn', 'glow', 'meter_bind', 'group_anim'], extra='Text and waveform must clip to the round socket interior.',
  onsurface='On-surface comparison: waveform area 380x124 per eye (47k px2) against 244x100 in #1 (about 1.9x); jog diameter 464 px against 356 px (1.7x area); deck read-out (title, BPM, time, overview) moves into the sockets, which frees the forehead; mixer, sampler, FX, master, mic, REC and app buttons are all on the main window (about 30 of 33 inventory blocks, only the browser floats; #5 and #9 float 4-6 modules, #6 floats 3). The window is also the tallest (1890 px) because of the spine.',
  hardu='Harder: tall window (1890 px), the spine and the mouth mixer need scrolling or a large display; effects are far from the decks. Easier: biggest waveforms and jogs, no module hunting, everything on one surface.'),
]

def rows_md(c):
    return '\n'.join('| %s | %s |' % r for r in c['rows'])

def needs_md(c):
    out = []
    for k in c['needs']:
        out.append('- %s' % NEED[k])
    out.append('- Concept-specific: ' + c['extra'])
    return '\n'.join(out)

for c in C:
    t = '# %s%s\n\n**Idea.** %s\n\n**Style:** %s. **Window plan:** %s.\n\n## Control map\n| Anatomy | Controls |\n|---|---|\n%s\n\n**Animates.** %s\n\n## Windows\n%s Shapes: %s\nAll empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).\n\n## Engine needs (today it lacks)\n%s\n\n## Usability\n%s\n' % (
        c['name'], ' (OWNER\'S REQUEST: huge sockets, open jaw, visible spine)' if c.get('owner') else '', c['idea'], c['style'], c['win'], rows_md(c), c['anim'], c['float'], c['shapes'], needs_md(c), c['hardu'])
    if c.get('onsurface'):
        t += '\n## On-surface count\n' + c['onsurface'] + '\n'
    open(os.path.join(here, c['d'], 'notes.md'), 'w').write(t)

# counts
cnt = {}
for c in C:
    for k in c['needs']:
        cnt[k] = cnt.get(k, 0) + 1
order = sorted(cnt, key=lambda k: (-cnt[k], list(NEED).index(k)))
order = ALL + [k for k in order if k not in ALL]
needs_list = '\n'.join('%d. %s - %d of 10 concepts' % (i + 1, NEED[k], cnt[k]) for i, k in enumerate(order))

top3 = [('10', 'X-Ray Open Jaw', 'It is the owner request and the strongest performance skin: everything except the browser is on one surface, with the biggest waveforms and jogs, and it needs only features the other concepts also need.'),
        ('09', 'Wireframe Diagram', 'The most usable by far and the cheapest engine-wise (outline plus panels); it proves the skull idea works even stripped to lines, and is a good first shipping skull.'),
        ('01', 'Anatomical Ivory', 'The most literal answer to the brief: clear anatomy-to-control mapping, one continuous waveform through both eyes, and a shipping-quality look without exotic effects.')]

R = ['# DeckHand skull skins: ten concepts\n\nAll artwork is original and drawn by script (`NN-name/make.py`, shared helpers in `lib.py`, standard library only; rendered with headless Chrome). Every mockup is a skull silhouette on a **transparent canvas** (`mockup-transparent.png`, `mockup.svg`); `mockup.png` shows it over a desktop-like wallpaper so the transparency reads. The track browser (and other large modules where it helps) are separate floating windows with their own shapes and a grip handle. `contact-sheet.png` has all ten; `index.html` is the gallery with the comparison table.\n']
for c in C:
    R.append('## %s %s%s\n%s Control families: %s. Floating: %s Usability %d/5 (%s).\n' % (c['n'], c['name'], ' (owner\'s request)' if c.get('owner') else '', c['idea'], '; '.join('%s: %s' % (a, b.split(',')[0]) for a, b in c['rows'][:5]), c['float'], c['use'][0], c['use'][1]))
R.append('## Recommendation: the three strongest\n' + '\n'.join('%d. **%s %s** - %s' % (i + 1, a, b, w) for i, (a, b, w) in enumerate(top3)) + '\n')
R.append('## Consolidated engine needs, ordered by how many concepts need them\n' + needs_list + '\n')
open(os.path.join(here, 'README.md'), 'w').write('\n'.join(R))

# gallery
def tr(c):
    return '<tr><td>%s %s%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%d/5 %s</td></tr>' % (c['n'], html.escape(c['name']), ' <span class="own">owner request</span>' if c.get('owner') else '', html.escape(c['style']), html.escape(c['win']), c['dens'], html.escape(c['hard']), c['use'][0], html.escape(c['use'][1]))
secs = ''
for c in C:
    secs += '<section id="c%s"><h2>%s %s%s</h2><img src="%s/mockup.png" alt="%s"><p>%s</p><div class="cols"><div><h3>Control map</h3><table class="m"><tr><th>Anatomy</th><th>Controls</th></tr>%s</table></div><div><h3>Floating windows</h3><p>%s</p><h3>Engine needs</h3><ul>%s</ul><h3>Usability</h3><p>%s</p>%s<p><a href="%s/mockup-transparent.png">transparent PNG</a> &middot; <a href="%s/notes.md">notes</a></p></div></div></section>' % (
        c['n'], c['n'], html.escape(c['name']), ' <span class="own">owner request</span>' if c.get('owner') else '', c['d'], html.escape(c['name']), html.escape(c['idea']),
        ''.join('<tr><td>%s</td><td>%s</td></tr>' % (html.escape(a), html.escape(b)) for a, b in c['rows']), html.escape(c['float'] + ' ' + c['shapes']),
        ''.join('<li>%s</li>' % html.escape(NEED[k]) for k in c['needs']) + '<li>%s</li>' % html.escape(c['extra']), html.escape(c['hardu']),
        ('<p><b>On-surface:</b> %s</p>' % html.escape(c['onsurface'])) if c.get('onsurface') else '', c['d'], c['d'])
page = '''<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DeckHand skull skins</title><style>
:root{--bg:#f4f4f6;--fg:#15161a;--card:#fff;--line:#d5d6dc;--acc:#0a6cff}
@media(prefers-color-scheme:dark){:root{--bg:#14161b;--fg:#e8e9ee;--card:#1d2027;--line:#343842;--acc:#5aa2ff}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 -apple-system,Helvetica,Arial,sans-serif}
main{max-width:1280px;margin:0 auto;padding:24px 16px}
h1{margin:0 0 6px}table{border-collapse:collapse;width:100%;background:var(--card)}th,td{border:1px solid var(--line);padding:6px 8px;text-align:left;vertical-align:top;font-size:13px}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;margin:28px 0;padding:16px}
section img{width:100%;max-width:900px;display:block;margin:0 auto;border-radius:8px}
.cols{display:grid;grid-template-columns:1fr 1fr;gap:20px}@media(max-width:900px){.cols{grid-template-columns:1fr}}
.own{background:var(--acc);color:#fff;border-radius:10px;padding:1px 8px;font-size:12px;font-weight:600}
.m td:first-child{white-space:nowrap;font-weight:600}h3{margin:12px 0 4px;font-size:14px}a{color:var(--acc)}
</style></head><body><main><h1>DeckHand skull skins</h1><p>Ten directions for the skull skin. Each skull is shown over a wallpaper; the real window is transparent and shaped, and the browser and other big modules float separately. <a href="contact-sheet.png">Contact sheet</a>.</p>
<h2>Comparison</h2><table><tr><th>Concept</th><th>Style</th><th>Windows</th><th>Density</th><th>Hardest engine requirement</th><th>Usability</th></tr>''' + ''.join(tr(c) for c in C) + '''</table>
<p><b>Every concept needs, first:</b> ''' + html.escape(NEED['shaped_win']) + '; ' + html.escape(NEED['float_win']) + '; ' + html.escape(NEED['drag']) + '.</p>' + secs + '<h2>Consolidated engine needs</h2><ol>' + ''.join('<li>%s (%d of 10)</li>' % (html.escape(NEED[k]), cnt[k]) for k in order) + '</ol></main></body></html>'
open(os.path.join(here, 'index.html'), 'w').write(page)
print('docs ok')
