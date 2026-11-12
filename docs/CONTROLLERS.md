# Controllers

DeckHand supports controllers through mapping files, not code. Adding a controller means adding
one `.dhmap` text file; nothing is compiled.

- Built-in mappings: `controllers/` (bundled into the app).
- Your own: `controllers/` inside DeckHand's data folder. A mapping with the same `name`
  replaces the built-in one.
- A device is matched by its MIDI name. Unmatched devices are listed in the log.

## File format
One directive per line; `#` starts a comment; bytes are hex; decks and slots count from 1.

    name  "My Controller"
    match "part of the MIDI device name"
    audio match=<part of audio device name> master=1 phones=3    # first channel of each stereo pair
    jog   ticks_per_rev=720
    init  90 17 7F                                                # sent when the device connects
    in  <kind> <status> <data1> [<lsb data1>] <action> [key=value ...]
    out <status> <data1> <state> [key=value ...]

Input kinds: `button` (non-zero = press, zero or note-off = release), `abs` (7-bit),
`abs14` (MSB and LSB controller numbers), `rel` (0x40 = no movement), `rel2` (0x01.. clockwise,
0x7F.. counter-clockwise).

Actions: `play cue sync tempo_range seek_start jog_touch jog tempo loop_in loop_out loop_toggle
reloop cue_call beatloop beatjump hotcue hotcue_clear trim eq_high eq_mid eq_low filter fader
crossfader head_mix head_level master_level pfl browse browse_press load pad_mode ignore`.

Arguments: `deck=N`, `slot=N`, `beats=X`, `dir=-1|1`, `scale=X`, `surface=top|side` (jog),
`invert=1`, `mode=hotcue|padfx1|beatjump|sampler|keyboard|padfx2|beatloop|keyshift`.

Output states: `playing cue sync pfl loop loop_in hotcue loaded pad_mode level`, with `blink=1`,
`on=XX off=XX`, and for `level` meters `min=XX max=XX`.

## Supported today
- MIDI controllers on macOS (CoreMIDI). Two decks are shown in the UI; the engine and the mapping
  format already handle four.
- Shipped mapping: DDJ-FLX4 (`controllers/ddj-flx4.dhmap`), written from the manufacturer's
  published MIDI message list. It has been checked against that document and by automated tests,
  but **not yet on the hardware**. See docs/TESTING-FLX4.md.

## Not yet
- HID controllers, MIDI learn, pad FX / sampler / keyboard / key-shift pad modes, beat FX section,
  and reading the controls' initial positions at connect (a control takes effect when first moved).
