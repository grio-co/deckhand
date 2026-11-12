# First hardware test: DDJ-FLX4 on macOS

Nothing here has been run on the hardware yet. This is what to try and what to report.

## Start
1. `scripts/build-macos.sh`
2. Plug in the controller, then run with debug logging so every MIDI message is recorded:

       DECKHAND_LOG=debug build/DeckHand.app/Contents/MacOS/DeckHand

3. The top bar should show the controller's name in green and its audio device with 4 channels.
   The log is `deckhand.log` in the data folder (`~/Library/Application Support/DeckHand`).

## Before playing anything
Controls take effect when first moved. Move every fader and knob once (channel faders,
crossfader, trim, EQ, filter, tempo, headphone mix) so DeckHand knows where they are.

## Checklist
- Browse knob scrolls the track list; pressing it switches between playlists and tracks;
  LOAD puts the selected track on that deck (refused while that deck is playing).
- PLAY/PAUSE, CUE (hold to preview, release to return), SHIFT+CUE to the start.
- Master output on the main outputs; headphone CUE buttons and the headphone MIX knob.
- Channel faders, crossfader, trim, EQ, filter knob. The on-screen mixer should follow.
- Tempo slider: toward "+" should speed up. BEAT SYNC matches tempo and beat to the other deck.
- Jog: touching the top holds and scratches; the side nudges. Hot cue pads set and jump;
  SHIFT+pad deletes. Loop IN / OUT, 4 BEAT/EXIT, CUE/LOOP CALL halves and doubles a loop.
- Pad modes: HOT CUE, BEAT JUMP, BEAT LOOP (SHIFT + BEAT JUMP).
- LEDs: play and cue, sync, headphone cue, hot cue pads, level meters.

## Things most likely to need adjusting (all in `controllers/ddj-flx4.dhmap`)
- **Jog feel.** `ticks_per_rev` is a guess. Touch the platter, turn exactly one revolution,
  release: the debug log prints the tick count. Put that number in the mapping.
- **Master level / headphone level knobs.** Left unmapped on the assumption that the unit applies
  them itself. If they do nothing, uncomment the two lines near the end of the mapping.
- **Tempo direction**, if it turns out reversed: add `invert=1` to the two `tempo` lines.
- **Outputs swapped or silent headphones:** check the "audio:" line in the log and adjust
  `audio ... master=1 phones=3`.

## Please report
The log file, and for anything wrong: what you did, what happened, what you expected.
