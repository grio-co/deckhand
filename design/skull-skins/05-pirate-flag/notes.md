# Pirate Scraps

**Idea.** A bandana-wearing skull in brass portholes with crossed bones underneath: the bones are the mixer, with EQ knobs strung along each bone and a brass cap that slides diagonally for the channel fader. Deck controls live on four torn sailcloth scraps that float beside the skull.

**Style:** Jolly Roger skull, crossed bones, brass and torn sailcloth. **Window plan:** Skull + 4 cloth scraps + chart browser.

## Control map
| Anatomy | Controls |
|---|---|
| Portholes (eyes) | Jog ring with rivets, waveform lane |
| Bandana | Master, headphone level / mix, master meter |
| Cloth scrap A / B (floating) | Info, overview, transport, key shift, toggles, loop, pad modes |
| Lower scraps (floating) | FX rows, tempo + range |
| Upper teeth | 16 hot-cue pads |
| Cheekbones | Sampler (left), mic + REC (right) |
| Jaw | Crossfader on a curved brass rail |
| Crossed bones | Trim, EQ, filter knobs; channel faders slide along the bones; meters and cue at the bone ends |
| Floating | Treasure-map browser, app buttons |

**Animates.** Bandana tail flutters; portholes glint; knobs on a bone turn as set; scraps sway slightly (container rotate).

## Windows
Four cloth scraps (deck A, deck B, FX A, FX B), treasure-map browser, app buttons. Shapes: Scraps: torn sailcloth with stitched border. Browser: torn parchment map.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Waveform (and text) widgets clipped to a circle/polygon (eye socket, visor, lens) instead of a rectangle
- Per-widget shape outlines for buttons/pads (tooth, chamfer, gem, petal) with per-pixel hit testing
- Slider/fader travelling along a path (arc jaw rail, diagonal bone) with matching hit-testing
- Rotated widgets (fader caps and knob rows on a diagonal)
- Animations on containers (transform/opacity of a group including children): jaw drop/sway, eye pulse, vertebra bounce
- Concept-specific: Channel faders are sliders along a straight but rotated track.

## Usability
Hardest of the set: diagonal faders are awkward and the deck panels are far from the skull. Easiest to read: big brass knobs.
