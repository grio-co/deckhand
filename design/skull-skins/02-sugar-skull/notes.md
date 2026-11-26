# Sugar Skull

**Idea.** A sugar skull whose eyes are flower rosettes that spin as the jog while each holds its own waveform lane; every knob is a marigold flower. Teeth are real buttons (both rows), pads move to the cheeks, and a strand of beat beads links the eyes.

**Style:** Dia de los Muertos ornament, flowers, papel picado. **Window plan:** Skull + bunting-framed browser.

## Control map
| Anatomy | Controls |
|---|---|
| Eye rosettes | Jog (petal ring + marker), waveform lane through the socket centre |
| Forehead | Info, overview, master / headphone knobs (flowers) and master meter |
| Crown flowers | 8 sampler pads |
| Ears | FX rows (3 slots) + tempo fader and range |
| Cheekbones | 8 hot-cue pads per deck (rounded), pad modes |
| Upper teeth | Play, cue, sync, key lock, quantise, slip, reverse, range |
| Lower teeth | Loop in/out/size, key shift -/+, eject |
| Nasal bridge | Mic, REC, settings / skin / language, beat garland |
| Jaw | Two channel rows with flower knobs, crossfader |

**Animates.** Rosettes rotate with the jog; flowers pulse with the kick; garland beads light on beats; petals on a channel knob bloom with level.

## Windows
Browser on an ofrenda panel strung with papel-picado bunting; status pill. Shapes: Browser: bunting-topped plaque. Status: pill.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Waveform (and text) widgets clipped to a circle/polygon (eye socket, visor, lens) instead of a rectangle
- Per-widget shape outlines for buttons/pads (tooth, chamfer, gem, petal) with per-pixel hit testing
- Animations on containers (transform/opacity of a group including children): jaw drop/sway, eye pulse, vertebra bounce
- Style properties (glow, opacity, scale, rotate) bindable to level meters and beat phase (eye glow follows level, teeth light as VU)
- Concept-specific: Flower-shaped knobs need a petal sprite that rotates with value.

## Usability
Harder than default: labels on busy ornament, buttons moved onto teeth make the grouping unfamiliar. Easier: huge round targets and strong colour per function.
