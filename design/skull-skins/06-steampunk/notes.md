# Steampunk

**Idea.** Aged bone in brass goggles whose gear rims are the jog rings; each goggle shows low, mid and high bands as three stacked lanes. Brass trumpets for ears carry the effects and tempo, and the mixer is a separate riveted console with a vertebra stack for master and headphones.

**Style:** Aged bone, brass, rivets, needle gauges. **Window plan:** Skull + brass console + ledger browser + sampler/app tray.

## Control map
| Anatomy | Controls |
|---|---|
| Goggles (eyes) | Gear-ring jog, 3-band stacked waveform lanes |
| Forehead | Info, overview, L/R needle gauges (master level) |
| Cheekbones | Transport, pad modes, key lock / quantise / slip / reverse |
| Trumpet ears | FX rows, tempo + range |
| Nose | Mic, REC |
| Upper teeth | 16 hot-cue pads |
| Lower teeth | Loop in/out/size, key shift, range |
| Console (floating) | Two channel strips, crossfader; three vertebrae = master, headphone level, headphone mix |
| Floating | Ledger browser, sampler + app tray |

**Animates.** Gear rims rotate with the jog; gauge needles follow the master meter; steam-vent glow on the beat.

## Windows
Brass console, ledger browser, tray with sampler and app buttons. Shapes: Console: riveted brass plate. Browser: brass-bound ledger. Tray: loose brass buttons.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Waveform (and text) widgets clipped to a circle/polygon (eye socket, visor, lens) instead of a rectangle
- Per-widget shape outlines for buttons/pads (tooth, chamfer, gem, petal) with per-pixel hit testing
- Waveform layouts: stacked lanes (3-band), mirrored shared-centreline halves, outline/line mode, glow beam
- Value-driven rotation (needle gauges bound to meters)
- Animations on containers (transform/opacity of a group including children): jaw drop/sway, eye pulse, vertebra bounce
- Style properties (glow, opacity, scale, rotate) bindable to level meters and beat phase (eye glow follows level, teeth light as VU)
- Concept-specific: Gear-toothed rim is just a sprite that rotates with position.

## Usability
Harder: rivet detail is visual noise; stacked band lanes are only 34 px tall. Easier: gauges give master level at a glance, console is conventional.
