# Predator Owl

**Idea.** A menacing owl mid-strike: heavy angled brow slabs carry the deck read-outs, two enormous glowing eyes are the decks (iris = jog ring, pupil = centre, one waveform band running through both eyes), the hooked beak holds the master knob, the chest is the mixer, the spread wings carry rows of feather-shaped buttons, and each foot is a set of four talon-toes for load, sync, play and cue.

**Silhouette:** Head-on bird of prey with ear tufts, spread wings with fanned primaries, and talons reaching down. **Style:** Night/ember owl (plus an ice palette), glowing eyes, feather buttons. **Windows:** Owl + perch browser + shed-feather FX + pellet sampler.

## Control map
| Part | Controls |
|---|---|
| Eyes | Jog (glowing iris ring with marker and progress arc), BPM, key, time, remaining, waveform band through both eyes with cues and playheads |
| Brow slabs | Title and artist per deck |
| Beak | Master knob; headphone level / mix beside it |
| Upper chest | Crossfader |
| Chest | Two channel strips (trim, EQ, filter, fader, meters, cue) and master meter |
| Primaries (8 per wing) | Hot-cue pads as feather-shaped buttons |
| Coverts / leading edge | Tempo slider along the leading edge, range, loop in/out/size, beat jump, key shift |
| Talons (each foot) | Load, sync, play, cue as four big toes |
| Floating perch | Track browser |
| Floating shed feather | Effects: three slots per deck |
| Floating pellet | Sampler, settings / skin / language |

**Animates.** Eyes glow with channel level and the pupils dilate on the kick; primaries flare slightly on pad hits; claws flex on transport press; ear tufts twitch on beat.

## Windows
Perch branch (browser), shed feather (effects) and a pellet (sampler); status pill. Shapes: Browser: log with end rings. Effects: shed feather. Sampler: pellet oval. Ice palette is in mockup-ice.png.
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
- Concept-specific: Feather and talon buttons are rotated shaped widgets with alpha hit-testing; gaps between flight feathers are transparent.

## Usability
Easier: talon transport and feather pads are huge. Harder: fanned, tilted pads and a wide wingspan; tempo sliders are curved.
