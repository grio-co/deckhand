# Tiki Mask

**Idea.** A tall carved mask: the eyes are rings of coloured beads (the jog) and a single inlaid band runs through both with deck A growing upward and deck B downward from a shared centre line. The huge mouth is the mixer, framed by two rows of teeth.

**Style:** Carved wood, inlay beads, feathers. **Window plan:** Mask + hanging planks + totem browser.

## Control map
| Anatomy | Controls |
|---|---|
| Eyes | Bead-ring jog, shared-centreline waveform A up / B down |
| Forehead plaque | Info, overview, master section |
| Feather crown | 8 sampler pads |
| Nose bridge | Transport (vertical), mic, REC |
| Cheeks | Toggles, loop, key shift, pad modes |
| Ear discs | Tempo + range |
| Hanging planks | FX rows |
| Upper teeth (mouth) | 16 hot-cue pads |
| Mouth cavity | Two channel rows, crossfader |
| Lower teeth (mouth) | Loop in/out/size, key shift, range |
| Floating | Totem-base browser, app buttons |

**Animates.** Feathers sway on the beat; bead ring marker travels; mouth cavity glow follows level.

## Windows
Totem-base browser, hanging FX planks, app buttons. Shapes: Browser: carved totem trapezoid. Planks: wooden tags on cords.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Waveform layouts: stacked lanes (3-band), mirrored shared-centreline halves, outline/line mode, glow beam
- Animations on containers (transform/opacity of a group including children): jaw drop/sway, eye pulse, vertebra bounce
- Concept-specific: Mirrored waveform halves are two decks drawn on one centre line.

## Usability
Harder: mirrored lanes make it unclear which half belongs to which deck at a glance. Easier: the mouth mixer is large and central.
