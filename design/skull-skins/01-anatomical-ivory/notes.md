# Anatomical Ivory

**Idea.** A restrained, anatomically literal ivory skull: each eye is a jog ring around a dark socket, and one continuous waveform band runs through both sockets and across the nasal bridge with a playhead at each pupil. Controls sit on dark engraved plates so nothing competes with the bone.

**Style:** Clean vector, ivory bone on charcoal plates. **Window plan:** Skull + ribcage browser + status pill.

## Control map
| Anatomy | Controls |
|---|---|
| Eye sockets | Jog ring (marker, progress arc), waveform band with playhead at pupil, deck letter |
| Forehead / cranium | Title, artist, BPM, key, time, remaining, mini overview; app buttons and REC at the crown |
| Cheekbones | Play / cue / sync, key shift, key lock / quantise / slip / reverse, loop in/out/size, pad modes |
| Ears | Tempo fader + range |
| Nasal bridge / cavity | Mic on / talk / level; master, headphone level, headphone mix, master meter |
| Upper teeth | 16 teeth = 8 hot-cue pads A | 8 pads B |
| Lower teeth | 16 sampler pads |
| Mandible | Two channel rows (trim, EQ, filter, fader, meters, cue) and crossfader |
| Vertebrae | 3 vertebrae = 3 FX slots, deck A left wing, deck B right wing |

**Animates.** Jog marker and ring progress turn with the platter; pupil playheads; teeth glow as pads; band amplitude breathes on the beat; jaw nudges 2 px with the kick.

## Windows
Ribcage browser (rib-cage silhouette with grip) and a status pill float below the spine. Shapes: Browser: rib cage. Status: pill.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Waveform (and text) widgets clipped to a circle/polygon (eye socket, visor, lens) instead of a rectangle
- Per-widget shape outlines for buttons/pads (tooth, chamfer, gem, petal) with per-pixel hit testing
- Animations on containers (transform/opacity of a group including children): jaw drop/sway, eye pulse, vertebra bounce
- Concept-specific: Band that crosses from one socket across bone to the other is a single wide widget with two playheads.

## Usability
Hardest to use: 16-tooth rows need 58 px pitch; the FX rack sits far from the decks. Easiest: strict one-job-per-region mapping, large pads, readable text.
