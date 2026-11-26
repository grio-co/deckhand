# Neon Chrome

**Idea.** A low-poly chrome skull with slanted holographic visors for eyes: the two decks share one band of stacked lanes (A on top, B below) that scrolls through both visors with a single playhead at the nasal bridge. Jog platters are HUD discs on the cheekbones; the ears are cyber headphone cups full of controls.

**Style:** Faceted chrome skull, neon edge glow, holographic visors. **Window plan:** Skull + terminal browser + app strip.

## Control map
| Anatomy | Controls |
|---|---|
| Visors | Stacked waveform lanes A / B, central playhead |
| Forehead | Info, overview, key lock / quantise / slip / reverse, loop, key shift, master section |
| Cheekbones | Jog HUD disc, transport (vertical), pad modes |
| Headphone ears | FX rows, 8 hot-cue pads, tempo + range |
| Nose | Mic on / talk / level |
| Upper teeth | 16 sampler pads |
| Lower teeth | Master level L/R as 16 lit segments |
| Mandible | Channel rows, crossfader |
| Spine terminal (floating) | Track browser; REC and app buttons beside it |

**Animates.** Edge glow follows master level; lower teeth are a live VU; playhead pulses on beat; HUD arcs turn with the jog.

## Windows
Terminal browser (chamfered window), REC, settings / skin / language strip (small separate window). Shapes: Browser: chamfered rectangle. App strip: chamfered capsule.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Waveform (and text) widgets clipped to a circle/polygon (eye socket, visor, lens) instead of a rectangle
- Per-widget shape outlines for buttons/pads (tooth, chamfer, gem, petal) with per-pixel hit testing
- Waveform layouts: stacked lanes (3-band), mirrored shared-centreline halves, outline/line mode, glow beam
- Soft glow / blur and additive blend per widget
- Style properties (glow, opacity, scale, rotate) bindable to level meters and beat phase (eye glow follows level, teeth light as VU)
- Concept-specific: Shared two-lane waveform with one playhead is one widget with two decks bound.

## Usability
Harder: the shared playhead hides each deck's own position until you learn it. Easier: very high contrast, deck colours obvious, pads and jog well separated.
