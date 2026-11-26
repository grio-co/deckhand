# Crystal Glass

**Idea.** A faceted crystal skull lit from inside: the eyes are glass orbs and the waveform is a beam of light passing through both orbs and the bridge. Knobs and pads are cut gems, the brow ridge carries the transport, and the mixer is a spine of crystal segments.

**Style:** Translucent crystal, internal light, gem controls. **Window plan:** Skull + shard browser + app tray.

## Control map
| Anatomy | Controls |
|---|---|
| Orbs (eyes) | Jog ring with gem studs, light-beam waveform through both orbs |
| Brow ridge | Play / cue / sync and key lock / quantise / slip / reverse |
| Forehead | Info, overview |
| Temples | Tempo + range |
| Cheekbones | FX slots (3 columns) |
| Nasal cavity / bridge | Master section; mic |
| Jaw | 8 hot-cue pads + pad modes per deck |
| Upper teeth | Loop in/out/size, key shift, range |
| Lower teeth | 16 sampler pads |
| Crystal spine | Trim / EQ / filter rows (A left, B right), crossfader, faders, meters, cue |
| Floating | Shard browser, REC + app buttons |

**Animates.** Internal glow follows master level; facets flash on pad hits; beam brightness follows amplitude; orb highlight drifts.

## Windows
Crystal-shard browser, REC and app buttons tray. Shapes: Browser: irregular quadrilateral shard. Tray: gem-cut buttons.
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
- Concept-specific: Needs additive blending so the beam reads as light.

## Usability
Harder: translucent skin changes contrast with the wallpaper; the beam hides cue flags in loud sections. Easier: spine rows make the EQ matrix obvious.
