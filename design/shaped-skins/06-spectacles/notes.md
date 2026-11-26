# Spectacles

**Idea.** Twin round lenses are the platters (rim = jog, glass = waveform and read-out) and the tall bridge between them is the mixer. Bifocal tabs below each lens hold pads, loops and transport; the temple arms are vertical tempo sliders.

**Silhouette:** Two round lenses joined by a tall bridge plate, vertical temple arms and bifocal tabs. **Style:** Tortoiseshell frames and gold rims, retro optics. **Windows:** Glasses + case browser + lens-cloth tray.

## Control map
| Part | Controls |
|---|---|
| Lenses | Jog rim, waveform with cues, title, artist, BPM, key, time, remaining |
| Bridge | Two channel strips, master, headphone, meter, crossfader, mic |
| Bifocal tabs | 8 hot-cue pads, loop in/out/size, key shift, transport |
| Temple arms | Tempo + range |
| Floating case | Track browser |
| Floating lens-cloth tray | Sampler, REC, settings / skin / language |

**Animates.** Lens glints sweep on beat, rims rotate with the platter, bridge LEDs follow channel level.

## Windows
Glasses case (browser) and a wavy-edged cloth tray. Shapes: Browser: hard case with strap. Tray: wavy-edged cloth.
Everything outside the silhouette is transparent (`mockup-transparent.png`; `mockup.png` shows it over a wallpaper).

## How it would be built with the skin engine
One main skin window with a `shape` outline (alpha/outline hit-testing), art as sprites in an atlas, widgets (`jog`, `waveform`, `pads`, `knob`, `fader`, `meter`, `browser`) placed with `free` anchors; each floating module is its own `window`. Buildability today: 4/5 (5 = easiest).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (alpha-based hit = alpha, cut-out holes such as handle gaps and reel spokes; docs/SKINS.md has the outline polygon, limit 256 points, but the shell still shows a rectangle)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other (browser, sampler, effects etc. are separate skin windows)
- Drag-region declaration per window (move by the body or the grip motif; no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform or text inside
- Waveform, text and widget clipping to a circle or polygon (eye, lens, porthole) instead of a rectangle
- Concept-specific: Lens shapes are two circles plus rectangles.

## Usability
Easier: huge round waveform with read-out inside. Harder: mixer is a tall narrow strip in the middle.
