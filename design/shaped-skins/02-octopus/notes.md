# Octopus

**Idea.** The two eyes are the decks (jog ring around a read-out), a brow-plate carries both waveforms in stacked lanes, and the tentacles are the controls: the outer pair are tempo sliders, the next pair carry the EQ knobs strung along them, the third pair are curved channel faders, and the inner pair hold loop buttons like suckers.

**Silhouette:** Mantle dome with two eyes and eight tapering tentacles curling downward. **Style:** Bioluminescent deep-sea purple, glowing eyes. **Windows:** Octopus + clam browser + bubble sampler.

## Control map
| Part | Controls |
|---|---|
| Eyes | Jog ring and deck read-out (title, BPM, key, time) |
| Brow | Stacked A / B waveforms with cue flags and playhead |
| Under the eyes | Hot-cue pads, play / cue / sync |
| Funnel (centre) | Master, headphone level, headphone mix, master meter |
| Tentacles 1 / 8 | Tempo path sliders + range |
| Tentacles 2 / 7 | Trim, high, mid, low, filter knobs along the path |
| Tentacles 3 / 6 | Channel fader (path), meter, cue |
| Tentacles 4 / 5 | Loop in/out/size buttons |
| Beak smile | Crossfader on a curved path |
| Floating clam | Track browser |
| Floating bubbles | Sampler, settings / skin / language |

**Animates.** Tentacles sway on a slow sine, tips curl on the beat, eyes pulse with channel level, bubbles drift up.

## Windows
Clam-shell browser and a bubble cluster with the sampler. Shapes: Browser: scalloped clam. Sampler: three translucent bubbles.
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
- Concept-specific: Tapered tentacle art can be sprites; sliders need path hit-testing.

## Usability
Harder: faders curve and EQ knobs are spread out; pads are far from the transport. Easier: eyes are huge targets.
