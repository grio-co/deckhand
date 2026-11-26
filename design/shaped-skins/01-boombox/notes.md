# Boombox

**Idea.** A ghetto-blaster whose two speaker cones are the platters: the rubber surround is the jog ring, the dust cap shows deck letter and BPM. A tape-style display in the middle carries both waveforms, the mixer sits between the cones, and the browser is a floating cassette.

**Silhouette:** Wide rounded body with an arched carry handle (see-through gap) and a telescopic antenna. **Style:** Chunky chrome and graphite, carry handle, speaker cones. **Windows:** Boombox + cassette browser + tape-door sampler.

## Control map
| Part | Controls |
|---|---|
| Left / right cone | Jog (surround ring with marker and progress arc), dust cap read-out |
| Display (centre top) | Two stacked waveform lanes with cues and a shared playhead |
| Top corners | Play / cue / sync, title, artist, BPM, key, time |
| Cone sides | Tempo fader + range |
| Under cones | Loop in/out/size, 8 hot-cue pads in one row |
| Centre column | Two channel strips (trim, EQ, filter, fader, meter, cue), master, headphone, master meter |
| Base strip | Crossfader |
| Floating cassette | Track browser |
| Floating tape door | Sampler, REC, settings / skin / language |

**Animates.** Cones pump with the kick (scale 1.0 to 1.02 on beat); surround marker turns with the platter; antenna ball blinks on beat.

## Windows
Cassette (browser) and tape-door (sampler, REC, app buttons) float below; status pill. Shapes: Browser: cassette with screws and a trapezoid base. Sampler: cassette-door plate.
Everything outside the silhouette is transparent (`mockup-transparent.png`; `mockup.png` shows it over a wallpaper).

## How it would be built with the skin engine
One main skin window with a `shape` outline (alpha/outline hit-testing), art as sprites in an atlas, widgets (`jog`, `waveform`, `pads`, `knob`, `fader`, `meter`, `browser`) placed with `free` anchors; each floating module is its own `window`. Buildability today: 4/5 (5 = easiest).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (alpha-based hit = alpha, cut-out holes such as handle gaps and reel spokes; docs/SKINS.md has the outline polygon, limit 256 points, but the shell still shows a rectangle)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other (browser, sampler, effects etc. are separate skin windows)
- Drag-region declaration per window (move by the body or the grip motif; no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform or text inside
- Waveform, text and widget clipping to a circle or polygon (eye, lens, porthole) instead of a rectangle
- Concept-specific: Handle gap needs a hole in the window shape.

## Usability
Easier: large familiar layout, everything labelled. Harder: wide (1460 px) body.
