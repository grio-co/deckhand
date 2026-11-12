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

Other directives: `defaults pickup=1 deadzone=0.03` (soft takeover and centre dead zone for every
absolute control, see below). Annotated examples that use every directive:
`controllers/generic-2deck.dhmap.example` and `controllers/shift-layer.dhmap.example` (illustrative
numbers, not real devices).

## Actions
Transport and mixer: `play cue sync tempo_range seek_start jog_touch jog tempo loop_in loop_out
loop_toggle reloop cue_call beatloop beatjump hotcue hotcue_clear trim eq_high eq_mid eq_low filter
fader crossfader head_mix head_level master_level pfl browse browse_press load pad_mode ignore`.

| Action | Input | What it does |
|---|---|---|
| `key_lock` `quantize` `slip` `reverse` | button | toggle; `value=1\|0` sets; `hold=1` = on while held |
| `key_shift` | button / abs / rel | `semitones=X` sets; `dir=1\|-1` steps one semitone (-12..12); abs sweeps `lo=`..`hi=` (default -12..12, rounded); rel moves `scale=` semitones per tick |
| `loop_move` | button / rel | moves the loop by `beats=` (signed); rel: `scale=` beats per tick |
| `loop_exit` `loop_halve` `loop_double` | button | leave / halve / double the loop |
| `auto_gain` | abs / button | abs: `lo=`..`hi=` dB (default -24..12); button: `value=` dB |
| `color_fx` | button | `effect=filter\|noise\|dub_echo\|crush\|pitch\|space` sets, or `dir=1\|-1` cycles |
| `fx_select` | button / rel | `effect=<name>` sets, `dir=1\|-1` (default next) or an encoder cycles the effects |
| `fx_on` | button | toggle (`value=1\|0` sets, `hold=1` momentary) |
| `fx_wet` `fx_param` | abs / rel | wet/dry and the effect's second parameter; rel: 1 % x `scale=` per tick |
| `fx_time` | button / rel | `dir=1\|-1` steps through 1/16 1/8 1/4 1/2 1 2 4 8 16 beats; `beats=X` sets |
| `sampler` | button | trigger slot `slot=N` (+ `offset=`); with a modifier named `shift` held: stop it |
| `sampler_stop` | button | stop slot `slot=N`, or every slot when no slot is given |
| `sampler_level` | abs | sampler bus level |
| `master_bpm` | button / abs / rel | `value=X` sets (0 = follow the decks); abs sweeps `lo=`..`hi=` (default 60..180); rel: `scale=` BPM per tick |
| `modifier` | button | holds the modifier `name=<name>` (global, or only for `deck=N`) |
| `keyboard` `keyshift` `padfx` | pad button | see pad modes |

Effect slots: `slot=1..3`, and `unit=deck` (default, the rule's `deck=`) or `unit=master` (the master
bus). Effect names: `none echo ping_pong spiral reverb flanger phaser low_cut_echo filter_sweep roll
transform pitch crusher vinyl_brake backspin`.

## Arguments
`deck=N`, `slot=N`, `beats=X`, `dir=-1|1`, `scale=X`, `surface=top|side` (jog), `invert=1`,
`mode=hotcue|padfx1|beatjump|sampler|keyboard|padfx2|beatloop|keyshift`, `semitones=X` / `value=X`,
`lo=X hi=X`, `effect=<name>`, `unit=deck|master`, `hold=1`, `offset=N` (sampler slot offset),
`pad=N` (pad number, for `padfx` defaults), `name=<modifier>`, `when=<modifier>` or
`when=!<modifier>`, `pickup=0|1`, `deadzone=X` (0..0.45).

Output states: `playing cue sync pfl loop loop_in hotcue loaded pad_mode level key_lock quantize slip
reverse fx_on sampler_playing loop_beats key_shift modifier`, with `blink=1`, `on=XX off=XX`, and for
`level` meters `min=XX max=XX`.
- `fx_on slot=N [unit=master] [effect=<name>]`: lit while that slot is on (and runs that effect).
- `sampler_playing slot=N [offset=N]`: lit while the sampler slot plays (needs the sampler views, see
  `ControllerHub::tick` below; the sampler is shared, so `deck=` only selects which deck's pad mode gates it).
- `loop_beats beats=X`: lit while a loop of that many beats (within 2 %) is active.
- `key_shift semitones=X`: lit while the deck's key shift rounds to X; without `semitones=` it is the pad
  number minus 1 (keyboard mode) or minus 5 (keyshift mode).
- `modifier name=<name>`: lit while the modifier is held.

## Pad modes
`pad_mode` buttons (`mode=` is required) select a mode per deck (default `hotcue`); `out ... pad_mode`
lights the selected mode's button. A rule with `mode=` fires only while its deck is in that mode, so one
set of pad notes can do eight things; devices that send other notes per mode need no `mode=`. A pad
released after a mode change still sends its release to the rule that got the press. Pad N is `slot=N`.

| Mode | Action on the pad | Per-pad arguments (defaults) |
|---|---|---|
| `hotcue` | `hotcue` / `hotcue_clear` | `slot=` |
| `beatloop` | `beatloop` | `beats=` (0.25, 0.5, 1, 2, 4, 8, 16, 32 by pad) |
| `beatjump` | `beatjump` | `beats=` signed (-1, +1, -2, +2, -4, +4, -8, +8 by pad) |
| `sampler` | `sampler` | `slot=` + `offset=` (a different bank per deck); shift held = stop |
| `keyboard` | `keyboard` | `semitones=` (pad number - 1): sets the key shift, then plays the hot cue pressed last; release lets go of it |
| `keyshift` | `keyshift` | `semitones=` (pad number - 5): sets the key shift |
| `padfx1` `padfx2` | `padfx` | `effect=` (by pad: echo ping_pong roll transform flanger phaser vinyl_brake backspin), `beats=` (the effect's default), `slot=` (effect slot, default 1), `unit=`, `pad=` |

`padfx` is momentary: on press it selects the effect and its beats on the slot and enables it; on release
it disables it, so release effects (vinyl brake, backspin) act while held. Pad LEDs are normal `out` rules
with `mode=`: a rule belongs to its mode and is dark otherwise.

## Modifiers and layers
`in button <s> <d> modifier name=shift` marks a held button (global; add `deck=N` for a per-deck one).
Any rule may add `when=shift` (only while held) or `when=!shift` (only while not held), so a device that
sends the same message with and without SHIFT can be mapped with two rules. A press is released by the
rule that received it even if the modifier changed meanwhile. Rules for the sampler read the modifier
named exactly `shift`. `out ... modifier name=shift` lights an LED while it is held.

## Soft takeover and dead zone
`pickup=1` on a rule (or `defaults pickup=1` for the whole mapping) makes an absolute control act only
after it crosses, or comes within 2 % of, the engine's current value; until then it is ignored, so a
fader does not make the value jump. It starts over (a) when a modifier used by a `when=` rule changes,
(b) when the pad mode changes for rules with `mode=`, and (c) at connect. It needs the engine's values,
which the hub learns from `tick`; before the first tick values pass straight through. `deadzone=X`
makes a flat zone of +-X around the centre (for centre-detented knobs); the ends stay reachable.
14-bit and relative handling are unchanged.

## MIDI learn (library; no UI yet)
Any connected MIDI device gets an empty mapping when no mapping matches, so learn works on any
controller (it is not listed by `connected()`).
- `ControllerHub::learn_begin("filter deck=2")`: the action as written after the control. Returns false
  (see `learn_error()`) for an unknown action or bad arguments. The next control touched on any device is
  captured and not dispatched.
- `learn_state()`: `Waiting`, `Observing`, `Done`, `Failed`, `Idle`.
- `learn_result(finalize=false)`: the mapping line, or "" while undecided. A note is captured at once, and
  so is a control change for a button action. Other control changes are watched for 12 messages or 0.4 s
  (or `finalize=true`): a controller number n < 32 together with n + 32 = `abs14`; otherwise by action
  type: faders and knobs are `abs`, jog and browse are `rel` (values near 0x40) or `rel2` (near 0x00 and
  0x7F), other actions are `abs` when the values spread by 24 or more. So move a fader through its range
  and turn an encoder a few clicks. A note for a fader, knob or encoder action fails with `learn_error()`.
- `learn_device()`, `learn_cancel()`, `learn_commit()` (adds the rule to that device's live mapping).
- `save_user_mapping(device, lines, folder)`: writes or updates `<folder>/<device>.dhmap` (name and match
  are the device name for a new file). A line replaces any existing line with the same control, or the same
  action and target (deck, slot, unit, mode, when...); everything else is kept.

## Hub API for the app
`tick(decks, count, now)` as before; `tick(decks, count, &mixer, samplers, sampler_count, now)` also
feeds the master effect state, sampler level, master BPM and sampler slot states. All actions are engine
`Command`s sent through `ControlSink::command`; nothing new is required of the app.

## Supported today
- MIDI controllers on macOS (CoreMIDI). Two decks are shown in the UI; the engine and the mapping
  format already handle four.
- Shipped mapping: DDJ-FLX4 (`controllers/ddj-flx4.dhmap`), written from the manufacturer's
  published MIDI message list. It has been checked against that document and by automated tests,
  but **not yet on the hardware**. See docs/TESTING-FLX4.md.

## Not yet
- HID controllers, a MIDI learn UI, and a controller-state readback at connect (use `pickup=1` to avoid
  jumps). The FLX4 mapping lacks the pad FX, sampler, keyboard and key-shift pad notes, KEY LOCK, CFX and
  beat FX controls: their MIDI numbers are not in the list it was written from (TODO lines in the file).
