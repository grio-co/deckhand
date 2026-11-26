# Pebble (compact pocket mode)

**Idea.** An extremely compact two-deck player: two mini platters with play inside, a two-lane waveform, BPMs, crossfader and cue / sync, all at 1x with 28 px minimum targets. Everything else (hot cues, loops, EQ and faders, browser) lives in small docks that appear on demand beside it.

**Silhouette:** A smooth pebble with a thumb dent, small enough for a screen corner. **Style:** Soft stone, two accent colours. **Windows:** Pebble (740 x 330) + hot-cue dock + loop dock + mix dock + picker.

## Control map
| Part | Controls |
|---|---|
| Mini platters | Jog (ring marker) with play |
| Under platters | Cue, sync per deck |
| Centre | Two waveform lanes, both BPMs, crossfader, master and headphone knobs, EXPAND |
| Hot-cue dock (floating) | 4 pads per deck |
| Loop dock (floating) | Loop in/out/size and tempo nudge |
| Mix dock (floating) | Trim, EQ, filter, channel fader, meter, cue per channel |
| Picker (floating) | Search and track list |

**Animates.** Platters tick, thumb dent glows with the beat, docks slide out from the pebble edge.

## Windows
Four docks: hot-cue pill, loop pill, mix dock and track picker. Shapes: All pill-shaped, same stone material, each with grip dots.
Everything outside the silhouette is transparent (`mockup-transparent.png`; `mockup.png` shows it over a wallpaper).

## How it would be built with the skin engine
One main skin window with a `shape` outline (alpha/outline hit-testing), art as sprites in an atlas, widgets (`jog`, `waveform`, `pads`, `knob`, `fader`, `meter`, `browser`) placed with `free` anchors; each floating module is its own `window`. Buildability today: 5/5 (5 = easiest).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (alpha-based hit = alpha, cut-out holes such as handle gaps and reel spokes; docs/SKINS.md has the outline polygon, limit 256 points, but the shell still shows a rectangle)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other (browser, sampler, effects etc. are separate skin windows)
- Drag-region declaration per window (move by the body or the grip motif; no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform or text inside
- Compact/expanded window modes with remembered positions (switch between a tiny shell and the full skin, docks that appear on demand)
- Concept-specific: Mini mode and the full skin share bindings; the docks are normal skin windows.

## Usability
Easier to park in a corner and glance at. Harder: no pads or EQ on the pebble itself, small targets.
