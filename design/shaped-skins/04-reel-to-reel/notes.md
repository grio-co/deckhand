# Reel-to-Reel

**Idea.** Each deck is a reel: the hub is the jog and read-out, the rim carries a position arc, and the visible tape pack shrinks as the track plays. The tower between the reels is the tape head with both waveforms as two tape runs; the plinth holds the mixer and per-deck controls.

**Silhouette:** Two huge reels flanking a head tower over a plinth with feet. **Style:** Champagne metal and black faceplate, analogue tape. **Windows:** Tape machine + tape-box browser + tape-loop cartridge.

## Control map
| Part | Controls |
|---|---|
| Reels | Jog hub with marker, title / BPM / time, rim position arc, tape-pack remaining time |
| Head tower window | Two waveform runs A / B, playhead on the head |
| Tower top | Master, headphone, master meter |
| Plinth left / right | Tempo, 8 hot-cue pads, transport, loop in/out/size |
| Plinth centre | Two channel rows, crossfader, mic, REC |
| Floating tape box | Track browser |
| Floating tape-loop cartridge | Sampler, settings / skin / language |

**Animates.** Reels turn with the platter, the pack radius follows remaining time, VU-lit master meter.

## Windows
Tape-box browser and a stadium-shaped tape-loop cartridge. Shapes: Browser: tape box with flap. Sampler: stadium cartridge with hubs.
Everything outside the silhouette is transparent (`mockup-transparent.png`; `mockup.png` shows it over a wallpaper).

## How it would be built with the skin engine
One main skin window with a `shape` outline (alpha/outline hit-testing), art as sprites in an atlas, widgets (`jog`, `waveform`, `pads`, `knob`, `fader`, `meter`, `browser`) placed with `free` anchors; each floating module is its own `window`. Buildability today: 4/5 (5 = easiest).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (alpha-based hit = alpha, cut-out holes such as handle gaps and reel spokes; docs/SKINS.md has the outline polygon, limit 256 points, but the shell still shows a rectangle)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other (browser, sampler, effects etc. are separate skin windows)
- Drag-region declaration per window (move by the body or the grip motif; no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform or text inside
- Waveform, text and widget clipping to a circle or polygon (eye, lens, porthole) instead of a rectangle
- Concept-specific: Spoke cut-outs are holes in the reel sprite.

## Usability
Easier: plinth is a conventional layout. Harder: the waveform window is small and the reels cover the corners.
