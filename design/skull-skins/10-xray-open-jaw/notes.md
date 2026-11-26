# X-Ray Open Jaw (OWNER'S REQUEST: huge sockets, open jaw, visible spine)

**Idea.** Owner's request: caricature-large eye sockets, the jaw dropped wide open and the full cervical spine on show. The sockets hold the whole deck read-out (title, big waveform, BPM, time, overview) inside a giant jog ring; the open mouth holds the mixer; seven vertebrae carry FX, master, faders, mic and app buttons, so only the browser floats.

**Style:** Radiograph skull, caricature sockets, jaw dropped, cervical spine. **Window plan:** Skull + X-ray film browser (only the browser floats).

## Control map
| Anatomy | Controls |
|---|---|
| Giant sockets | Jog ring (464 px), 380x124 waveform with cues and playhead, title, artist, BPM, key, time, remaining, overview |
| Brow | Play / cue / sync, key shift, key lock / quantise / slip / reverse, loop in/out/size |
| Temples | Tempo + range |
| Cheekbones | Pad modes |
| Upper teeth | 16 hot-cue pads |
| Open mouth | Two channel rows (trim, EQ, filter, fader, meters, cue) |
| Lower teeth | Loop in/out/size, key shift, range |
| Mandible | 8 sampler pads, crossfader along the jawbone |
| Vertebrae C1-C3 | FX slot 1-3, deck A left wing, deck B right wing |
| C4 | Master, headphone level / mix, master meter |
| C5 | Channel faders, meters, cue |
| C6 | Mic on / talk / level, REC |
| C7 | Settings, skin, language |
| Floating | X-ray film browser (only floating module) |

**Animates.** Jaw drops a few px on every kick; sockets glow with channel level; vertebrae light as their control is touched; jog ring marker turns with the platter.

## Windows
Only the track browser floats (X-ray film sheet with grip, slightly tilted) plus the status pill. Shapes: Browser: X-ray film with rounded corners. Status: pill.
All empty areas (nasal cavity, open mouth gap, spaces between vertebrae, beside the jaw) are transparent; the mockup is on a transparent canvas (`mockup-transparent.png`; `mockup.png` shows it over a desktop-like wallpaper).

## Engine needs (today it lacks)
- Per-window transparency with shaped hit-testing (outline + cut-out holes such as nasal cavity and open mouth; clicks fall through transparent pixels)
- Multiple independent floating windows with remembered positions, optional snapping/docking to each other
- Drag-region / grip declaration per window (move the window by a skull part or frame grip, no title bar)
- Annular/round jog widget: ring hit-test, rotating position marker, progress arc, centre area that passes clicks to the waveform inside
- Waveform (and text) widgets clipped to a circle/polygon (eye socket, visor, lens) instead of a rectangle
- Per-widget shape outlines for buttons/pads (tooth, chamfer, gem, petal) with per-pixel hit testing
- Soft glow / blur and additive blend per widget
- Style properties (glow, opacity, scale, rotate) bindable to level meters and beat phase (eye glow follows level, teeth light as VU)
- Animations on containers (transform/opacity of a group including children): jaw drop/sway, eye pulse, vertebra bounce
- Concept-specific: Text and waveform must clip to the round socket interior.

## Usability
Harder: tall window (1890 px), the spine and the mouth mixer need scrolling or a large display; effects are far from the decks. Easier: biggest waveforms and jogs, no module hunting, everything on one surface.

## On-surface count
On-surface comparison: waveform area 380x124 per eye (47k px2) against 244x100 in #1 (about 1.9x); jog diameter 464 px against 356 px (1.7x area); deck read-out (title, BPM, time, overview) moves into the sockets, which frees the forehead; mixer, sampler, FX, master, mic, REC and app buttons are all on the main window (about 30 of 33 inventory blocks, only the browser floats; #5 and #9 float 4-6 modules, #6 floats 3). The window is also the tallest (1890 px) because of the spine.
