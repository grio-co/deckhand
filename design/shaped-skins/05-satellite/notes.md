# Satellite

**Idea.** A satellite: the two solar wings are the decks (jog, pads, loops, transport, tempo) and the hub between them is the mixer with the shared waveform display on top. A detached solar array is the browser and a probe pod holds the sampler.

**Silhouette:** Central hub with antenna and two chamfered solar wings on truss arms. **Style:** Gold foil, silver hub, blue solar cells, space. **Windows:** Satellite + solar-array browser + probe-pod sampler.

## Control map
| Part | Controls |
|---|---|
| Left / right solar wing | Jog + read-out, 8 pads, loop in/out/size, key shift, transport, tempo + range |
| Hub display | Two stacked waveforms |
| Hub body | Two channel strips, master, headphone, master meter, crossfader |
| Under the hub | Mic, REC |
| Floating solar array | Track browser |
| Floating probe pod | Sampler, settings / skin / language |

**Animates.** Wings tilt a few degrees toward the louder deck, antenna LED blinks on beat, cell grid shimmers.

## Windows
Solar-array browser on hinge arms and a rounded probe pod. Shapes: Browser: tilted solar panel. Sampler: rounded-square pod with porthole.
Everything outside the silhouette is transparent (`mockup-transparent.png`; `mockup.png` shows it over a wallpaper).

## How it would be built with the skin engine
One main skin window with a `shape` outline (alpha/outline hit-testing), art as sprites in an atlas, widgets (`jog`, `waveform`, `pads`, `knob`, `fader`, `meter`, `browser`) placed with `free` anchors; each floating module is its own `window`. Buildability today: 5/5 (5 = easiest).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (alpha-based hit = alpha, cut-out holes such as handle gaps and reel spokes; docs/SKINS.md has the outline polygon, limit 256 points, but the shell still shows a rectangle)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other (browser, sampler, effects etc. are separate skin windows)
- Drag-region declaration per window (move by the body or the grip motif; no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform or text inside
- Waveform, text and widget clipping to a circle or polygon (eye, lens, porthole) instead of a rectangle
- Concept-specific: Mostly rectangles with chamfered corners: closest to the default skin.

## Usability
Easier: every block has plenty of room. Harder: wide (1480 px) spread.
