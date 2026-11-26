# Orrery

**Idea.** A brass orrery: the sun is the master with an arc meter, two planets are the decks, hot-cue pads ride the outer ring turned to follow it, EQ knobs orbit above each planet and the channel faders and tempo sliders are arcs. The crossfader is the lower orbit.

**Silhouette:** A single big brass ring (the only circular main window of the set). **Style:** Brass instrument, graduated ring, starry dome. **Windows:** Circular dome + scroll browser + hex sampler.

## Control map
| Part | Controls |
|---|---|
| Planets | Jog ring, waveform lane, title, BPM, time |
| Orbit above each planet | Trim, high, mid, low, filter knobs |
| Arc on the sun side | Channel fader, meter, cue |
| Arc below the planet | Tempo + range |
| Outer ring | 8 hot-cue pads per deck (rotated along the ring) |
| Sun | Master, arc meter, headphone level / mix |
| Lower orbit | Crossfader on an arc |
| Dome base | Transport, loop, mic, REC |
| Floating scroll | Track browser |
| Floating hex tile | Sampler, settings / skin / language |

**Animates.** Planets rotate slowly with the platter, outer ring ticks scroll with the beat, sun arc follows the master level.

## Windows
Scroll browser with brass rollers and a hexagonal sampler tile. Shapes: Browser: scroll. Sampler: hexagon.
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
- Arc-shaped segmented meters bound to a level (beacon bulb, sun)
- Concept-specific: Pad ring is eight rotated buttons per deck.

## Usability
Harder: sliders are arcs and pads are rotated. Easier: concentric grouping is easy to learn and the dome is one compact shape.
