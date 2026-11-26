# Wireframe Diagram

**Idea.** The skull is only an outline: eyes are jog rings with a thin line-drawn waveform (one continuous line crossing the bridge), teeth are pads, and the crossfader rides the jaw curve. Every other control lives on clean paper panels that float beside it, so the drawing stays calm and usability stays maximal.

**Style:** Single-line drawing on paper, almost a technical diagram. **Window plan:** Skull + 2 deck panels + master panel + app panel + browser.

## Control map
| Anatomy | Controls |
|---|---|
| Eyes | Jog ring, line-drawn waveform with playhead |
| Upper teeth | 16 hot-cue pads |
| Lower teeth | 16 sampler pads |
| Jaw | Crossfader on a curved rail |
| Deck panels A / B (floating) | Info, overview, transport, key shift, toggles, loop, pad modes, FX, tempo |
| Master panel (floating) | Master, headphone, meter, mic, REC |
| Under the chin | Two channel rows |
| Floating | Browser panel, app buttons panel |

**Animates.** Line waveform scrolls; teeth flip to colour on pad hit; jaw rail cap follows crossfader; nothing else moves.

## Windows
Two deck panels, master/mic panel, app panel, browser. Shapes: Panels: rounded paper tiles with grip. Browser: same.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Waveform (and text) widgets clipped to a circle/polygon (eye socket, visor, lens) instead of a rectangle
- Per-widget shape outlines for buttons/pads (tooth, chamfer, gem, petal) with per-pixel hit testing
- Slider/fader travelling along a path (arc jaw rail, diagonal bone) with matching hit-testing
- Waveform layouts: stacked lanes (3-band), mirrored shared-centreline halves, outline/line mode, glow beam
- Concept-specific: Line-mode waveform (outline only) is the main new drawing mode.

## Usability
Easiest of the ten: same density as the default skin, big text, high contrast. Hardest part is that six windows must be arranged by the user.
