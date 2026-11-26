# Scarab (closed / open)

**Idea.** A scarab whose wing cases open to reveal the mixer: open, each elytra is a deck (jog, waveform, pads, loops, transport, tempo) and the abdomen between them is the mixer; closed, the shell shrinks to a compact two-deck player. Both states are drawn in the mockup; the browser is a rolling dung ball.

**Silhouette:** Beetle seen from above with two huge elytra swung open, head, legs; closed form is a single oval. **Style:** Iridescent beetle shell, gold trim. **Windows:** Open beetle + closed compact form + dung-ball browser.

## Control map
| Part | Controls |
|---|---|
| Left elytra / right elytra (open) | Jog, read-out, waveform lane, 8 hot-cue pads, loop in/out/size, transport, tempo + range |
| Pronotum (shield) | Master, headphone level / mix, master meter, mic |
| Abdomen | Two channel strips, crossfader, REC, sampler pads, settings / skin / language |
| Closed shell (floating) | Mini waveform and play / cue for each deck, crossfader, OPEN hinge |
| Dung ball (floating) | Track browser |

**Animates.** Elytra swing from 0 to about 70 degrees about the hinge when OPEN is pressed (container rotate with children); closed shell breathes; wing shine sweeps on beat.

## Windows
Closed compact beetle (alternate state of the main window) and a circular dung-ball browser. Shapes: Browser: round ball. Closed state: single oval with seam.
Everything outside the silhouette is transparent (`mockup-transparent.png`; `mockup.png` shows it over a wallpaper).

## How it would be built with the skin engine
One main skin window with a `shape` outline (alpha/outline hit-testing), art as sprites in an atlas, widgets (`jog`, `waveform`, `pads`, `knob`, `fader`, `meter`, `browser`) placed with `free` anchors; each floating module is its own `window`. Buildability today: 3/5 (5 = easiest).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (alpha-based hit = alpha, cut-out holes such as handle gaps and reel spokes; docs/SKINS.md has the outline polygon, limit 256 points, but the shell still shows a rectangle)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other (browser, sampler, effects etc. are separate skin windows)
- Drag-region declaration per window (move by the body or the grip motif; no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform or text inside
- Waveform, text and widget clipping to a circle or polygon (eye, lens, porthole) instead of a rectangle
- Two-state shell (closed/open) with animated transform of grouped widgets and swapping of window layouts
- Compact/expanded window modes with remembered positions (switch between a tiny shell and the full skin, docks that appear on demand)
- Concept-specific: Two layouts of the same skin window with an animated transition.

## Usability
Easier: very large jogs and pads. Harder: wide when open and the mixer is far from the transport.
