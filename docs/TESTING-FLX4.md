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
- Beat FX FX SELECT steps the effect (SHIFT + press steps back); BEAT left / right shorten /
  lengthen the time; ON/OFF toggles it on the assigned channel; LEVEL/DEPTH turns the wet amount.
  DeckHand starts with the beat FX unit on the master bus until the switch is moved once.
- **Beat FX channel switch (to confirm on hardware).** The list we have does not say which note is which
  position. The mapping assumes 94 10 = CH1 (deck 1), 94 11 = CH2 (deck 2), 95 10 and 95 11 = CH1&CH2 (master).
  Move the switch through 1, 2, 1&2 with an effect on: the effect should stop on the old channel and
  the new channel should get the same effect, wet and time (switched off; press ON again). Report which
  position moves the effect to which channel and which notes the log shows (wrong guesses: swap the
  `fx_assign` lines in the file). ON/OFF must work in all three positions (position 2 sends 95 47).
- MASTER CUE: press, the headphones should switch to the master mix (head mix full master); press again
  restores the previous head mix. The LED note is assumed to be 96 63 too: report if it stays dark.
  Moving the headphone mix knob afterwards takes over. SHIFT + MASTER CUE and MIC LEVEL are not mapped.
- **LEVEL/DEPTH** is the one control whose published status byte and channel column disagree. If the
  knob does nothing, look at the log for the control change it sends (B4 or B5, number 02/22) and report it.
- SMART CFX button: steps the colour effect the CFX knobs drive (both decks); SHIFT steps back. Report
  what the CFX knob then does. Its LED (assumed 96 00, lit while a non-default colour effect is on deck 1)
  is unconfirmed. Not mapped: Smart Fader, MIC level (the engine has no mic input yet), mono/stereo switch.

## 6. LEDs
PLAY and CUE, SYNC, headphone CUE, hot cue pads, beat loop and beat jump pads, mode buttons, pad FX pads
(while an effect is on), sampler pads (while playing), keyboard / key shift pads, IN / OUT, level
meters, track-loaded lights, MASTER CUE and SMART CFX (both guessed notes). Not driven: FX ON/OFF (the
list describes blink / lit only in a note), SMART FADER. Report each LED that stays dark, stays lit, or is the wrong colour.

## Please report
The log file (`deckhand.log`), and for anything wrong: what you did, what happened, what you expected.
