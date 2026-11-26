# Lighthouse (tall and narrow)

**Idea.** A lighthouse designed to live at a screen edge: the lantern is the master with a beacon-bulb arc meter, then deck A, the mixer, deck B and a door at the base, stacked top to bottom. A sailing boat floats beside it as the browser with the sampler on its sail.

**Silhouette:** Tall narrow tower (about 4.6 wide to 1 high) for the screen edge, with a gallery and a lantern. **Style:** Stripe-painted tower, beacon, harbour. **Windows:** Tower (640 x 1950) + boat browser.

## Control map
| Part | Controls |
|---|---|
| Lantern | Beacon arc meter, master knob, headphone level / mix |
| Gallery | Mic, talk, level |
| Deck A section | Waveform, jog read-out, tempo, transport, 8 pads, loop, key shift |
| Mixer section | Two channel strips, channel meters, crossfader |
| Deck B section | Same as deck A |
| Door | REC, settings / skin / language |
| Floating boat | Track browser |
| Floating sail | Sampler |

**Animates.** Beam sweeps with the beat, bulb ring follows master level, waves lap under the boat.

## Windows
A boat (browser hull, sampler on the sail) and a status pill. Shapes: Browser: hull. Sampler: sail triangle.
Everything outside the silhouette is transparent (`mockup-transparent.png`; `mockup.png` shows it over a wallpaper).

## How it would be built with the skin engine
One main skin window with a `shape` outline (alpha/outline hit-testing), art as sprites in an atlas, widgets (`jog`, `waveform`, `pads`, `knob`, `fader`, `meter`, `browser`) placed with `free` anchors; each floating module is its own `window`. Buildability today: 4/5 (5 = easiest).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (alpha-based hit = alpha, cut-out holes such as handle gaps and reel spokes; docs/SKINS.md has the outline polygon, limit 256 points, but the shell still shows a rectangle)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other (browser, sampler, effects etc. are separate skin windows)
- Drag-region declaration per window (move by the body or the grip motif; no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform or text inside
- Arc-shaped segmented meters bound to a level (beacon bulb, sun)
- Screen-edge docking and auto-hide for a tall narrow window
- Concept-specific: Tower is rectangles; edge docking and auto-hide are the real requirement.

## Usability
Easier: one narrow column, little pointer travel horizontally. Harder: decks stack, so waveforms cannot be compared side by side.
