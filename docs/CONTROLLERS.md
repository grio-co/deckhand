# Controllers

DeckHand supports controllers through mapping files, not code. Adding a controller means adding
one `.dhmap` text file; nothing is compiled.

- Built-in mappings: `controllers/` (bundled into the app).
- Your own: `controllers/` inside DeckHand's data folder. A mapping with the same `name`
  replaces the built-in one.
- A MIDI device is matched by its MIDI name; a USB HID device by vendor/product id (see HID controllers). Unmatched MIDI devices are listed in the log.

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
Key sync and stems: `key_sync key_match key_reset stem_mute stem_solo stem_isolate stem_preset stem_enable` (see below).

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
| `fx_assign` | button | `unit=N target=deck1\|deck2\|deck3\|deck4\|master`: unit N now addresses that channel (press only) |
| `master_cue` | button | headphones monitor the master, see Master cue; `value=1\|0` sets, `hold=1` momentary |
| `mic_on` `mic_talkover` | button | microphone on/off and talkover (ducking) on/off: toggle; `value=1\|0` sets; `hold=1` = on while held |
| `mic_level` `mic_eq_low` `mic_eq_high` | abs | microphone level and its two EQ bands (0..1; 0.5 = flat) |
| `modifier` | button | holds the modifier `name=<name>` (global, or only for `deck=N`) |
| `keyboard` `keyshift` `padfx` | pad button | see pad modes |
| `key_sync` | button | key sync follow on/off (the deck's key is kept matching the other deck as tempos change; sets the key shift and engages key lock); toggle, `value=1\|0` sets, `hold=1` = on while held |
| `key_match` | button | key sync once: shift the key now to fit the other deck |
| `key_reset` | button | key shift back to 0 and key sync off |
| `stem_mute` `stem_solo` | pad / button | `stem=vocals\|drums\|bass\|other` (default: pad number mod 4): toggle; `value=1\|0` sets; `hold=1` momentary |
| `stem_isolate` | pad / button | same, momentary (on while held) unless `value=` is given |
| `stem_preset` | pad / button | `preset=reset\|acapella\|instrumental\|drums\|no_drums` |
| `stem_enable` | button | play the stems / the original mix (toggle, `value=1\|0` sets) |

Key sync (`key_sync`, `key_match`) uses the deck's tempo-synced partner (else the other loaded deck) as the reference. The
match mode, the limit (default +-2 semitones) and fine (cents) landing are engine settings (`deckN.key_sync` controls and
`IEngine::key_sync_config`), not mapping arguments. Stem actions press the stems on first when they are attached but off.

Effect slots: `slot=1..3`, and `unit=deck` (default, the rule's `deck=`), `unit=master` (the master bus),
or an effect unit that follows an assignment (see Effect units): `unit=assigned` (= unit 1) or `unit=N`. Effect names: `none echo ping_pong spiral reverb flanger phaser low_cut_echo filter_sweep roll
transform pitch crusher vinyl_brake backspin`.

## Arguments
`deck=N`, `slot=N`, `beats=X`, `dir=-1|1`, `scale=X`, `surface=top|side` (jog), `invert=1`,
`mode=hotcue|padfx1|beatjump|sampler|keyboard|padfx2|beatloop|keyshift|stems` (`mode=!name` = every mode but that one), `alt=<mode>` (pad_mode rules: the mode a second press selects), `stem=`, `preset=`, `alt_on=XX alt_blink=0|1` (pad_mode LEDs), `semitones=X` / `value=X`,
`lo=X hi=X`, `effect=<name>`, `unit=deck|master|assigned|1..4`, `target=deck1..deck4|master` (fx_assign), `hold=1`, `offset=N` (sampler slot offset),
`pad=N` (pad number, for `padfx` defaults), `name=<modifier>`, `when=<modifier>` or
`when=!<modifier>`, `pickup=0|1`, `deadzone=X` (0..0.45).

Output states: `playing cue sync pfl loop loop_in hotcue loaded pad_mode level key_lock quantize slip
reverse fx_on sampler_playing loop_beats key_shift modifier fx_assign master_cue color_fx mic_on mic_talkover key_sync stem`, with `blink=1`, `on=XX off=XX`, and for
`level` meters `min=XX max=XX`.
- `fx_on slot=N [unit=master] [effect=<name>]`: lit while that slot is on (and runs that effect).
- `sampler_playing slot=N [offset=N]`: lit while the sampler slot plays (needs the sampler views, see
  `ControllerHub::tick` below; the sampler is shared, so `deck=` only selects which deck's pad mode gates it).
- `loop_beats beats=X`: lit while a loop of that many beats (within 2 %) is active.
- `key_shift semitones=X`: lit while the deck's key shift rounds to X; without `semitones=` it is the pad
  number minus 1 (keyboard mode) or minus 5 (keyshift mode).
- `modifier name=<name>`: lit while the modifier is held.
- `fx_assign unit=N target=deckX|master`: lit while effect unit N is assigned to it.
- `master_cue`: lit while master cue is on.
- `mic_on`, `mic_talkover`: lit while the hub's last mic command left them on (the hub's own state, like master cue).
- `color_fx deck=N [effect=<name>]`: lit while the deck's colour effect is that one (without `effect=`: any
  but the default filter).

## Effect units
A hub-level assignment says which channel effect unit N (1..4, all start on master) addresses. A rule
with `unit=assigned` (unit 1) or `unit=N` on `fx_select fx_on fx_wet fx_param fx_time padfx` (and an `out ... fx_on`)
acts on whatever that unit is assigned to now. `fx_assign unit=N target=...` changes it; a controller with
a channel-select switch maps one rule per position (it fires on a press / non-zero value only). On a
change, every effect that is on is switched off on the old channel first, then the selected effect, wet,
parameter and time of each slot that has an effect are copied to the new channel (left off). Slots with no
effect selected are not copied. Assignment is not saved and starts at master.
`out ... fx_assign unit=N target=deckX` is lit while unit N is on that target.

## Master cue
`master_cue` (the engine has no dedicated master-cue switch, so this uses head mix, 0 cue .. 1 master):
the first press remembers the current head mix and sets it to full master; the second press restores
the remembered value. Turning a `head_mix` control in between ends it (the knob wins, no restore).
`out ... master_cue` is lit while it is on. The state is the hub's, not the engine's.

## Pad modes
`pad_mode` buttons (`mode=` is required) select a mode per deck (default `hotcue`); `out ... pad_mode`
lights the selected mode's button. A rule with `mode=` fires only while its deck is in that mode, so one
set of pad notes can do eight things; devices that send other notes per mode need no `mode=`. A pad
released after a mode change still sends its release to the rule that got the press. Pad N is `slot=N`.

| Mode | Action on the pad | Per-pad arguments (defaults) |
|---|---|---|
| `hotcue` | `hotcue` / `hotcue_clear` | `slot=` (1..16; a deck has 16 hot cue slots, a mapping uses as many pads as it has) |
| `beatloop` | `beatloop` | `beats=` (0.25, 0.5, 1, 2, 4, 8, 16, 32 by pad) |
| `beatjump` | `beatjump` | `beats=` signed (-1, +1, -2, +2, -4, +4, -8, +8 by pad) |
| `sampler` | `sampler` | `slot=` + `offset=` (a different bank per deck); shift held = stop |
| `keyboard` | `keyboard` | `semitones=` (pad number - 1): sets the key shift, then plays the hot cue pressed last; release lets go of it |
| `keyshift` | `keyshift` | `semitones=` (pad number - 5): sets the key shift |
| `padfx1` `padfx2` | `padfx` | `effect=` (by pad: echo ping_pong roll transform flanger phaser vinyl_brake backspin), `beats=` (the effect's default), `slot=` (effect slot, default 1), `unit=`, `pad=` |

### Pad-mode pairs and the stems mode
A `pad_mode` button can carry a second mode: `in button 90 22 pad_mode deck=1 mode=sampler alt=stems`. The first press
selects `sampler`; pressed again while it is active it selects `stems`; again, back to `sampler`. Pressing it from any
other mode starts at the main mode. The LED rule takes the same `alt=`: `out 90 22 pad_mode deck=1 mode=sampler alt=stems`
is lit in the main mode and **blinks** in the alternate (or shows level `alt_on=XX` steadily). A controller that has a
separate SHIFT message for the mode button needs no extension: a second `pad_mode ... mode=stems when=shift` rule.
`mode=!stems` on a rule means "in every pad mode except stems", so a rule that always worked (no `mode=`) can step aside
for the alternate mode that reuses its notes. Mappings without `alt=` / `mode=!` are unchanged.

`stems` mode (pad N is `slot=N`; the stem defaults to the pad number modulo 4):

| Pads | Action |
|---|---|
| 1-4 | `stem_mute` vocals, drums, bass, other (toggle) |
| 5-8 | `stem_isolate` the same four (while held) |
| SHIFT + 1..5 | `stem_preset` reset, acapella, instrumental, drums only, no drums |

LED `stem stem=<name>` (`mode=stems`): lit while the stem is audible (stems attached and playing, not muted, and solo / isolate
respected); dark when the track has no stems. The shipped DDJ-FLX4 mapping puts stems on the SAMPLER button's second press
and the sampler's own pad notes (docs/TESTING-FLX4.md section 7); no note or CC number was added.

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
`learn_choices(decks = 2)` returns `{label, action, group}` entries ("Play" / `play deck=1` / "Deck 1") for a
picker; pass `action` to `learn_begin`. Labels are English, groups are translated by the app.
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

## HID controllers
Many controllers (jog-wheel units, some mixers and pads) speak USB HID instead of MIDI. They use the same
`.dhmap` files, the same actions, arguments, LED states, pad modes, modifiers and soft takeover; only the
addressing of a control differs: a field of a HID report instead of a MIDI status/data pair. A mapping may
have a MIDI side (`match`), a HID side (`hid`) or both (modifiers are then separate per transport).
`controllers/generic-hid.dhmap.example` is an annotated example with illustrative numbers.

    hid vid=0x.... pid=0x.... [usage_page=0x.... usage=0x.... interface=N]   # which device (vid and pid required)
    hid_init feature|output report=<id> <hex bytes>      # sent once when the device connects (repeatable)
    hid_out report=<id> size=<n> [init=<hex bytes>] [interval=<ms>]   # template of an output report
    in  hid report=<id> byte=<n> [bit=<b> | bits=<lo>-<hi> | size=<bytes> endian=le|be] [signed=1]
        <button|touch|abs|rel|delta> <action> [key=value ...]
    out hid report=<id> byte=<n> [bit=<b> | bits=<lo>-<hi> | size=<bytes> endian=le|be]
        <state> [key=value ...]

Addressing: `report` is the report id (0 for devices without numbered reports). `byte` counts from the
first byte after the report id; the id itself is never part of the data. A field is `size` bytes (1..4,
default 1, little endian unless `endian=be`); `bit=b` or `bits=lo-hi` (bit 0 = least significant) picks bits
out of the assembled value, so a 12-bit control across two bytes is `byte=6 bits=0-11` and a button is
`byte=2 bit=3`. `signed=1` reads two's complement.

Input kinds:
- `button` / `touch` (the same; `touch` reads better for jog touch sensors): non-zero = pressed. `invert=1` for
  active-low bits.
- `abs`: the raw value is normalised to 0..1 over the full range of the field's width (or the signed range),
  or over `min=<raw> max=<raw>` (decimal or 0x hex; `max` may be less than `min` to reverse). Everything
  an absolute MIDI control can do works: `lo=`/`hi=`, `invert=`, `pickup=`, `deadzone=`.
- `rel`: a running position that wraps at its width (a jog wheel reporting an N-bit counter): each report
  gives the delta with wrap handling (250 to 3 on 8 bits is +9, 3 to 250 is -9). Per rule `ticks_per_rev=N`
  (counts per revolution; default: the mapping's `jog` directive); `scale=` and `invert=` as for MIDI.
- `delta`: the field itself is a per-report signed delta (acts on every report while non-zero).

Reports repeat every field, so a rule acts only when its own field changed since the previous report. The
first report after connecting is a baseline: nothing moves just because a device was plugged in. A report too
short for a rule is ignored by that rule.

Output reports: `hid_out` declares the template of an output report (payload size, the starting bytes, and
the minimum interval between sends, default 20 ms). `out hid` rules write their LED/meter state into
fields of it (`on=`/`off=` are hex values of the field width; the default `on` is every bit set; `level`
meters use `on` as maximum and `off` as minimum lit value). The hub composes the whole report from the template and all
rules and sends it only when it changed and the interval has passed (a held-back change goes out on a later
`tick`, so call `tick` regularly); bytes no rule writes keep the template's `init` values. Without a
`hid_out` line a template is made from the rules (zeros, as large as the rules reach). Fields are not
limited to single bits, which leaves room for later text or segment displays (a display is a field with `size=` bytes) -
no display actions exist yet. `hid_init` is how a device is switched into a special mode at connect.

### Safety
- Only devices a mapping names by vendor/product id are enumerated at all (the platform layers filter by
  vid/pid before looking at anything else); unmapped devices are never opened.
- Keyboards, keypads, mice and pointers (usage page 0x01 usages 0x01, 0x02, 0x06, 0x07, and page 0x07) are
  refused: a `hid` line naming them is a parse error, and the hub and every platform `open` refuse them even
  when a wide match (`hid vid=.. pid=..` without `usage_page=`) would otherwise reach them. Name the
  `usage_page=`/`usage=` (or `interface=`) of the controller's own interface when a device also exposes a keyboard.
- Devices are opened with ordinary shared access, never exclusively (macOS: no `kIOHIDOptionsTypeSeizeDevice`).
- HID is dormant until the app creates a `HidIO` (`create_hid_io`) and hands it to `ControllerHub::set_hid_io`.

### HID learn (devices without a mapping)
By default no unmapped HID device is even listed. To learn one, the app opts in with
`hub.hid_learn_allow(vid, pid)` (a device the user picked) or `hub.hid_learn_gamepads(true)` (also lists
joysticks, game pads and multi-axis devices). Those devices get an empty generated mapping, and
`learn_begin(action)` / `learn_result()` / `learn_commit()` work as for MIDI: the next control moved on any
device is captured and not dispatched. The hub watches every report: it keeps the last report per device and
which bytes change all the time (counters, sensors: ignored once they have changed in most recent reports).
- A button action is captured at once from the first report where exactly one bit changed (`... bit=5 button`,
  with `invert=1` when it went 1 to 0) or one byte went to or from zero; several controls changing at once
  are ignored until a clear press.
- Other actions observe up to 12 changing reports (0.4 s after the last, or `learn_result(true)`) and classify
  the byte that changed most: two adjacent bytes become one 16-bit field (`size=2 endian=le|be`) when the high byte
  only steps by one as the low byte wraps; a value that returns to zero several times is `delta signed=1`;
  a value that moves one way in small steps, wrapping at its width, is `rel`; otherwise faders and knobs
  are `abs` (jog and browse actions default to `rel`; other actions are `abs` when the values spread over a
  quarter of the range). So sweep a fader fully, or turn a jog a few clicks.
- A fader or encoder action given a lone toggling bit fails with `learn_error()`.
- `learn_header()` (read before `learn_commit()`, which resets the learn state) is the `hid vid=0x.... pid=0x....`
  line to pass as the last argument of `save_user_mapping(device, lines, folder, &error, header)`; it
  writes it instead of the `match` line of a new file. `learn_choices()` is unchanged.

### Platforms
- macOS: IOKit HIDManager (`platform/apple/hid_apple.cpp`; the manager is never opened, devices are opened
  one by one). iOS has no IOHID access: `create_hid_io` returns null there.
- Linux: `/dev/hidraw*`, found by rescanning `/sys/class/hidraw` about once a second (no libudev). The
  node needs permission: install a per-device udev rule, see `platform/linux/packaging/70-deckhand-hid.rules.example`
  (copy it to `/etc/udev/rules.d/`, put the controller's `lsusb` ids in, run `udevadm control --reload` and
  `udevadm trigger`). Without it DeckHand logs "cannot open /dev/hidrawN: Permission denied".
- Windows: SetupAPI + hid.dll, overlapped reads on a thread per opened device.

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
- A MIDI/HID learn UI, a controller-state readback at connect (use `pickup=1` to avoid jumps), and text or
  segment displays on HID devices. The FLX4 mapping lacks KEY LOCK; its beat FX channel switch and MASTER
  CUE note assignments are guesses (docs/TESTING-FLX4.md).
- No HID controller mapping is shipped: report layouts are device facts that have to be verified on the
  hardware (or from the maker's published documentation), so none are written from memory. The HID path is tested
  with simulated reports only.
- The mic actions send engine commands only in a build whose `engine.hpp` defines `DECKHAND_ENGINE_HAS_MIC`;
  without it they parse and do nothing.

## Per-controller settings and the Controller panel
Click the controller's name in the status row (or press `Cmd+Shift+C` / `Ctrl+Shift+C`, control `ui.controller`) to open
the panel of that device: connection, the mapping file in use and the decks it controls, pad modes (a button that has an
alternate mode, such as sampler / stems, is listed), the audio interface it asks for with a shortcut that makes it the
output device, "Learn a control...", "Reload mapping" (re-reads the `.dhmap` files and re-matches connected devices) and, when
several mappings fit one device, a chooser. Settings are stored per controller (library settings
`controller.<name>.jog_scale`, `.ticks_per_rev`, `.pickup`, `.leds`, `.mapping`; `GET /v1/controllers`) and handed to the
hub, live, also when it runs in the engine process:

* jog sensitivity: every jog turn is multiplied (0.25 to 4);
* jog ticks per turn: replaces the mapping's `ticks_per_rev`; "Measure a full turn" shows the ticks of the last
  touch-and-turn so one exact revolution calibrates it;
* soft takeover for knobs and faders: as the mapping says, on, or off;
* LED feedback: off keeps the controller's LEDs dark.

Not available in the panel (the hub has no such setting): a deck side switch (1/2 versus 3/4), jog touch behaviour,
per-knob behaviour of master / headphone level. A mapping file cannot be revealed in the file manager from the core (no
platform hook): its path is shown.

## Key sync, stems pad mode (application side)
`deckN.key_sync` (follow), `deckN.key_match` (once), `deckN.key_reset`, `deckN.key_sync_mode` (0 match, 1 any compatible key)
and the shortcuts `Cmd+Alt+A/S/F` (deck 1) and `Cmd+Alt+;/L/J` (deck 2; add Shift for decks 3 and 4) drive the engine's key
sync; the track's key reaches the engine when the track loads (`LoadedTrack::key_index`). The default skin shows a KEY SYNC
and a MATCH button with a status line under each deck. The stems pad mode is the `stems` pad mode of a mapping (see above):
the FLX4 mapping puts it behind a second press of the sampler mode button.
