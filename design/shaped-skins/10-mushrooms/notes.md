# Mushroom Cluster

**Idea.** Two big toadstools are the decks: the cap face holds the platter, title and waveform, and the gills underneath fan out as the eight hot-cue pads. Stems carry transport, loops and tempo; the small purple mushroom in between is the mixer, and the crossfader runs along the roots.

**Silhouette:** Two big caps with a small purple cap between them, flared stems, mossy ground. **Style:** Spotted toadstools on a mossy mound. **Windows:** Three mushrooms + bracket-fungus browser + spore sampler.

## Control map
| Part | Controls |
|---|---|
| Big caps | Jog, BPM, key, time, title, artist, waveform lane |
| Gills | 8 hot-cue pads per deck (fan, rotated) |
| Stems | Transport, key shift, loop, key lock / quantise / slip / reverse, pad modes, tempo + range |
| Small cap | Master, headphone, level meter |
| Small stem | Two channel strips with EQ, faders, meters, cue |
| Roots | Crossfader on a curve |
| Floating bracket fungus | Track browser |
| Floating spore cluster | Sampler, settings / skin / language |

**Animates.** Caps squash on the kick, spores drift from the gills, stem rings glow with channel level.

## Windows
A bracket-fungus shelf (browser) and a spore-bubble cluster (sampler). Shapes: Browser: half-moon bracket fungus. Sampler: translucent spore bubbles.
Everything outside the silhouette is transparent (`mockup-transparent.png`; `mockup.png` shows it over a wallpaper).

## How it would be built with the skin engine
One main skin window with a `shape` outline (alpha/outline hit-testing), art as sprites in an atlas, widgets (`jog`, `waveform`, `pads`, `knob`, `fader`, `meter`, `browser`) placed with `free` anchors; each floating module is its own `window`. Buildability today: 2/5 (5 = easiest).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (alpha-based hit = alpha, cut-out holes such as handle gaps and reel spokes; docs/SKINS.md has the outline polygon, limit 256 points, but the shell still shows a rectangle)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other (browser, sampler, effects etc. are separate skin windows)
- Drag-region declaration per window (move by the body or the grip motif; no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform or text inside
- Waveform, text and widget clipping to a circle or polygon (eye, lens, porthole) instead of a rectangle
- Sliders and knob chains that travel along a bezier/arc path with matching hit-testing (tentacles, orbits, roots, crossfader smile)
- Rotated widgets (pads and buttons following a ring or a fan)
- Concept-specific: Cap domes with a scalloped underside are sprites with alpha hit-testing.

## Usability
Harder: pads are fanned and rotated, controls spread over stems. Easier: giant caps and clear colour coding.
