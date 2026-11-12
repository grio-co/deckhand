# Hardware test: DDJ-FLX4 on macOS

Nothing here has been run on the hardware yet. Work through the list in order and note anything
that differs. Mapping file: `controllers/ddj-flx4.dhmap`.

## Start
1. `scripts/build-macos.sh`
2. Plug in the controller, then run with debug logging so every MIDI message is recorded:

       DECKHAND_LOG=debug build/DeckHand.app/Contents/MacOS/DeckHand

3. Top bar: controller name in green, audio device with 4 channels. Log: `deckhand.log` in
   `~/Library/Application Support/DeckHand`.
4. Load a track on each deck. Move every fader and knob once (channel faders, crossfader, trim, EQ,
   CFX knobs, tempo, headphone mix) so DeckHand knows where they are.

## 1. Calibration (do first; each is a one-line change in the mapping)
- **Jog ticks per revolution.** Touch the platter, turn exactly one revolution, release; the log prints
  the tick count. Report it (mapping has `jog ticks_per_rev=720`, a guess).
- **Tempo fader direction.** Toward "+" should speed up. If reversed, report it (fix: `invert=1`).
- **Output channel order.** Master on the main outputs, headphones (CUE) on the phones. If swapped or
  silent, report the log's "audio:" line (mapping has `master=1 phones=3`).
- **Master level and headphone level knobs.** Currently unmapped because the unit may apply them itself.
  Turn each: does the volume change with no software mapping? Report yes/no per knob.

## 2. Basics
- Browse knob scrolls the list, SHIFT + turn scrolls fast; pressing it switches panes; LOAD loads
  (refused while that deck plays).
- PLAY/PAUSE; CUE (hold previews, release returns); SHIFT+CUE to start; BEAT SYNC; SHIFT+SYNC changes
  the tempo range.
- Channel faders, crossfader, trim, EQ, CFX knob (filter). The on-screen mixer follows.
- Jog: touch top = scratch, side = nudge, SHIFT + platter = fast search. Loop IN / OUT, 4 BEAT/EXIT,
  SHIFT+4 BEAT/EXIT reloops, CUE/LOOP CALL halves and doubles.

## 3. Pad modes (try both decks; select a mode with its button, SHIFT + button for the lower row)
Expected for each: pads act as described, and the mode button lights. Report which pad did not.
1. HOT CUE: pad sets / jumps to a cue; SHIFT + pad deletes it. Lit pads show set cues.
2. BEAT JUMP: pads jump -1 +1 -2 +2 -4 +4 -8 +8 beats (pad 1..8).
3. BEAT LOOP (SHIFT + BEAT JUMP): pads start loops of 1/4 1/2 1 2 4 8 16 32 beats; the pad of the
   active loop is lit.
4. PAD FX 1: hold a pad: echo, ping pong, reverb, spiral, low cut echo, filter sweep, flanger, phaser
   (pad 1..8). The effect starts on press, stops on release, the pad lights while on. Report if
   the effect is silent or never stops.
5. PAD FX 2 (SHIFT + PAD FX 1): roll, transform, pitch, crusher, vinyl brake, backspin, echo and
   ping pong at 1/4 beat. Vinyl brake and backspin should act while held.
6. SAMPLER: pads play sampler slots 1-8 (deck 1) and 9-16 (deck 2); SHIFT + pad stops that slot;
   playing slots light.
7. KEYBOARD (SHIFT + HOT CUE): pad 1..8 sets the key shift 0..+7 semitones and plays the last-pressed
   hot cue; release lets go. The pad of the current shift lights.
8. KEY SHIFT (SHIFT + SAMPLER): pad 1..8 sets -4..+3 semitones;
   pad 5 is 0. Report if the labelled mode buttons do not match the order above.

## 4. Shift layer
- Hold SHIFT and press pads in modes other than HOT CUE: nothing should happen (except SAMPLER stop).
- Report any SHIFT combination that does something unexpected.

## 5. Effects section
- Beat FX FX SELECT steps the master effect (SHIFT + press steps back); BEAT left / right shorten /
  lengthen the time; ON/OFF toggles it on the master output; LEVEL/DEPTH turns the wet amount.
- Move the FX channel switch (1, 2, 1&2): ON/OFF must work in all three positions (position 2 sends on
  a different channel). Report if it only works in some. The switch itself does not change routing.
- **LEVEL/DEPTH** is the one control whose published status byte and channel column disagree. If the
  knob does nothing, look at the log for the control change it sends (B4 or B5, number 02/22) and report it.
- SMART CFX button: steps the colour effect the CFX knobs drive (both decks); SHIFT steps back. Report
  what the CFX knob then does. Not mapped: Smart Fader, MIC level, MASTER CUE, mono/stereo switch.

## 6. LEDs
PLAY and CUE, SYNC, headphone CUE, hot cue pads, beat loop and beat jump pads, mode buttons, pad FX pads
(while an effect is on), sampler pads (while playing), keyboard / key shift pads, IN / OUT, level
meters, track-loaded lights. Not driven: FX ON/OFF (the list describes blink / lit only in a note),
SMART CFX, SMART FADER. Report each LED that stays dark, stays lit, or is the wrong colour.

## Please report
The log file (`deckhand.log`), and for anything wrong: what you did, what happened, what you expected.
