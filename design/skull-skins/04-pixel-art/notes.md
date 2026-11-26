# Pixel Art

**Idea.** A chunky pixel skull on a 20 px grid whose sockets are small arcade screens: waveform lane above, a stepped-disc platter below. The jaw is long so the mixer fits as two vertical strips, like a classic channel pair.

**Style:** 8/16-bit pixels, chunky, playful. **Window plan:** Skull + save-file browser + arcade corner.

## Control map
| Anatomy | Controls |
|---|---|
| Eye sockets | Waveform lane + jog platter (pixel disc, square marker) |
| Forehead | Info, overview, key lock / quantise / slip / reverse |
| Cheekbones | Transport, key shift, loop, pad modes |
| Ears | FX rows, tempo + range |
| Nose | Master section, mic |
| Upper teeth | 16 hot-cue pads |
| Lower teeth | 16 sampler pads |
| Long jaw | Two vertical channel strips, crossfader at the chin |
| Floating | Save-file browser; REC, app buttons, title banner |

**Animates.** Platter turns in 30 degree steps; pads blink at 2 fps; eyes blink every 8 beats; teeth flash on hits.

## Windows
Save-file browser and an arcade corner (REC, settings / skin / language). Shapes: Browser: pixel-notched window. Corner: loose pixel buttons.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Nearest-neighbour integer scaling and a bitmap font (no smoothing) for the pixel look
- Animations on containers (transform/opacity of a group including children): jaw drop/sway, eye pulse, vertebra bounce
- Concept-specific: Skin must keep to integer scale factors so pixels stay square.

## Usability
Easier: huge flat targets, no thin lines. Harder: stepped jog gives coarse visual feedback; waveform lane is small.
