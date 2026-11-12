# DeckHand skins

Goal: Winamp-style total themeability, up to exotic layouts and effects, with no scripting.

## Package (`.dhskin`, zip)
    skin.ron        manifest: name, author, licence, min DeckHand version, windows
    layout/*.ron    windows and control placement
    img/            sprites, masks (window shape), backgrounds
    fonts/          fonts
    shaders/*.wgsl  optional fragment shaders (backgrounds, controls)
    sfx/            optional UI sounds

## Layout
- Skin owns everything: multiple windows, free-form shapes via mask images,
  any position / size / rotation per control, any panel may be omitted.
- Controls (provided by DeckHand): jog, knob, fader, button, meter, label, waveform,
  browser, playlist tree, deck info, effect slot, pad grid, image, shader surface.
- Each control declares sprites/animations per state (idle, hover, pressed, active)
  and per deck.

## Bindings
- Live values: position, bpm, key, level, beat phase, loop state, cue state, ...
- Expressions: arithmetic, comparisons, lookups only. No loops, no user functions.
- Missing behaviour means a new control or binding in DeckHand, never a script.

## Safety
- Untrusted input: size/count limits, shader validation, no file or network access.
- Any failure falls back to the built-in skin. Hot reload for authors.
- Skin runs in the UI process only; it can never affect audio.

## Built-in skin
Original artwork, shipped as a normal skin (dogfooding the format). No third-party
branding or assets.
