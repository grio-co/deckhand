# Keyboard shortcuts

DeckHand can be played and prepared from the keyboard alone, and every shortcut can be changed.

- Open **Settings > Keyboard shortcuts** (default key F1 opens it directly). Search an action, press **Set** and then the new key.
  If the key is taken you are asked whether to replace it. **Add** gives an action a second key, **Clear** removes its keys,
  **Reset** puts one action back to the default and **Reset all** puts everything back. **Save cheat sheet** writes
  `shortcuts.html` (print it) and `shortcuts.txt` into the data folder.
- Typing in the search box, a rename or any text field never triggers a shortcut.
- Keys are matched by physical position (the name is the key's name on a US keyboard), so the layout under your hands stays the
  same on other keyboard layouts. Letters that no shortcut uses still start a search, as before.

## Files

The shipped defaults are `resources/keymaps/default.dhkeys` (inside the app). Your changes are saved to `keymap.dhkeys` in
DeckHand's data folder and applied on top, so updates can add shortcuts without losing yours. The file only lists differences.
You can also edit it by hand:

    # one directive per line, '#' starts a comment
    bind   <chord> <control> [value] [mode=press|hold|toggle|repeat] [context=global|browser|text] [desc="text"]
    unbind <chord> [context=...]

    bind   mod+shift+k deck1.key_lock                  # a new shortcut
    bind   a deck1.cue mode=hold                       # replaces what A did
    bind   mod+9 ui.rate 3 desc="Rate 3 stars"
    unbind f5                                          # removes a default

- **chord**: modifiers and one key joined by `+`: `shift`, `alt`, `ctrl`, `cmd` and `mod` (Command on macOS, Control elsewhere),
  then a key: `a`..`z`, `0`..`9`, `f1`..`f24`, `space`, `enter`, `escape`, `tab`, `backspace`, `delete`, `home`, `end`,
  `pageup`, `pagedown`, `up`, `down`, `left`, `right`, `kp0`..`kp9`, and punctuation by name or as the character:
  `minus equals lbracket rbracket backslash semicolon quote grave comma period slash`.
- **control**: any name from the control table (`GET /v1/controls`, for example `deck1.play`, `deck2.hotcue.3`, `mixer.crossfader`)
  or an application action named `ui.*` (listed below). The optional **value** is passed to the control.
- **mode**: `press` (default) acts once when the key goes down; `hold` sends the value (1) on key down and 0 on key up (cue and
  hot cue pads: the pad plays while held); `toggle` acts once without a value; `repeat` acts again for every auto-repeat
  (browsing, nudging).
- **context**: `global` (default) works whenever no text field has the keyboard, also with a panel open; `browser` only in the
  main window with no panel open; `text` only while a text field has the keyboard.
- Two shortcuts on one chord in the same context: the later line wins (a warning is logged for the shipped file). Lines with an
  unknown control or key are skipped with a warning in the log; they never stop the rest from loading.
- Chords the operating system keeps for itself (Cmd+Q, W, H, M, Tab, Space on macOS, Alt+F4, ...) are bound if you insist but
  logged as reserved.

## Layout of the defaults

Left hand = deck 1, right hand = deck 2, mirrored; decks 3 and 4 use the same keys with **Alt** held.

    pads 1-4     1 2 3 4        7 8 9 0         pads 5-8   Q W E R      U I O P     (Shift + pad deletes the hot cue)
    cue/play/sync/load   A S D F       ; L K J          (Shift+A sets the cue point, Shift+S headphone cue,
                                                         Shift+D key lock, Shift+F tempo to zero, G memory cue)
    loops        Z in, X out, C halve, V double, B loop      / in, . out, , halve, M double, N loop
    beat jump    5 / T (4 beats, Shift 1)                     6 / Y
    bend         Shift+Z / Shift+X                            Shift+, / Shift+.
    waveform zoom  - / =                                      [ / ]

Loading: `F` and `J` load the selected track into deck 1 and 2 (Alt for 3 and 4), `Shift+Left` / `Shift+Right` do the same
from the browser, `Enter` loads into the free deck. `Up`/`Down`/`PageUp`/`PageDown`/`Home`/`End` browse, `Cmd/Ctrl+Up/Down`
switch playlist, `Cmd/Ctrl+F` jumps to search, `Space` pre-listens to the selected track in the headphones.

## The `ui.*` actions

Besides the engine's control table the keymap can call these application actions; they have no REST route of their own.
`ui.load.N`, `ui.load_free`, `ui.browse_*`, `ui.playlist_next/prev`, `ui.focus_search`, `ui.clear_search`, `ui.preview_*`,
`ui.add_to_prepare` (adds the selection to the playlist named *Prepare*), `ui.add_to`, `ui.rate` (0..5), `ui.rate_up/down`,
`ui.undo_edit` (undoes the last tag edit), `ui.edit_track`, `ui.compatible`, `ui.settings`, `ui.shortcuts`, `ui.sampler`,
`ui.learn`, `ui.columns`, `ui.close_panel`, `ui.window.browser/sampler`, `ui.layout`, `ui.skin_next`, `ui.cheat_sheet`,
`ui.nudge.N` (pitch bend, bind with `mode=repeat`), `ui.tempo_step.N`.

## Default shortcuts

`Mod` is Command on macOS and Control on Windows and Linux. This table is generated from the default file;
`test_keymap` fails when it is out of date (regenerate with `build/test_keymap --write-docs` from the repository root).

<!-- BEGIN GENERATED -->

### Application

| Shortcut | What it does | Control |
|---|---|---|
| `Mod+E` | Add the selected tracks to the end of the play queue | `ui.queue_add` |
| `Mod+Shift+E` | Play the selected tracks next | `ui.queue_play_next` |
| `Mod+Shift+C` | Open or close the connected controller's panel (mapping, jog and LED settings) | `ui.controller` |
| `Mod+Shift+D` | Switch a controller with switchable deck sides to its other pair of decks | `ui.controller.switch_decks` |
| `Mod+Shift+L` | Link the playhead of deck 1 to the playhead of deck 2 (a mix point between the two tracks) | `ui.link.1.2` |
| `Mod+Shift+B` | Link the playhead of deck 2 to the playhead of deck 1 | `ui.link.2.1` |
| `F9` | What goes well after the track on air | `ui.suggestions` |
| `Mod+Shift+Y` | Play the top suggestion next | `ui.queue_suggestion` |
| `F10` | Preparation mode: two headphone players on decks 3 and 4 | `ui.prep` |
| `Mod+,` | Open or close the settings | `ui.settings` |
| `F1` | Open or close the keyboard shortcuts | `ui.shortcuts` |
| `F2` | Open or close the sampler | `ui.sampler` |
| `F3` | Open or close MIDI learn | `ui.learn` |
| `F4` | Open or close the browser columns | `ui.columns` |
| `Mod+Shift+H` | Trash and edit history | `ui.trash` |
| `Mod+Alt+Shift+C` | Effect chain editor | `ui.chain` |
| `Mod+Shift+O` | Browser layouts (saved column sets) | `ui.layouts` |
| `Mod+Shift+.` | Next browser layout | `ui.layout_next` |
| `Mod+Shift+,` | Previous browser layout | `ui.layout_prev` |
| `Mod+Shift+U` | Find moved or renamed files | `ui.locate` |
| `Mod+Alt+Shift+V` | Record from an analogue input and split into tracks | `ui.capture` |
| `Mod+Shift+M` | Split the browser: side by side, stacked, off | `ui.split` |
| `F6` | Switch between the two- and four-deck layouts | `ui.layout` |
| `F7` | Switch to the next skin | `ui.skin_next` |
| `F8` | Write the shortcut cheat sheet (HTML) into the data folder | `ui.cheat_sheet` |
| `Mod+B` | Show or hide the browser window | `ui.window.browser` |
| `Escape` | Close the open panel, or clear the search | `ui.close_panel` |

### Browser

| Shortcut | What it does | Control |
|---|---|---|
| `F` | Load the selected track into deck 1 | `ui.load.1` |
| `J` | Load the selected track into deck 2 | `ui.load.2` |
| `Up` | Select the previous track (or playlist while the tree has focus) | `ui.browse_up (repeat)` |
| `Down` | Select the next track (or playlist while the tree has focus) | `ui.browse_down (repeat)` |
| `Page Up` | Move the selection up one page | `ui.browse_page_up (repeat)` |
| `Page Down` | Move the selection down one page | `ui.browse_page_down (repeat)` |
| `Home` | Select the first track | `ui.browse_top` |
| `End` | Select the last track | `ui.browse_bottom` |
| `Tab` | Move the keyboard between the playlist tree and the track list | `ui.browse_switch` |
| `Enter` | Load the selected track into deck 1, or into deck 2 while deck 1 plays | `ui.load_free` |
| `Shift+Left` | Load the selected track into deck 1 | `ui.load.1` |
| `Shift+Right` | Load the selected track into deck 2 | `ui.load.2` |
| `Mod+Up` | Show the previous playlist | `ui.playlist_prev` |
| `Mod+Down` | Show the next playlist | `ui.playlist_next` |
| `Mod+F` | Start typing in the search box | `ui.focus_search` |
| `Mod+Shift+F` | Clear the search box | `ui.clear_search` |
| `Mod+0` | Clear the rating | `ui.rate 0` |
| `Mod+1` | Rate 1 star | `ui.rate 1` |
| `Mod+2` | Rate 2 stars | `ui.rate 2` |
| `Mod+3` | Rate 3 stars | `ui.rate 3` |
| `Mod+4` | Rate 4 stars | `ui.rate 4` |
| `Mod+5` | Rate 5 stars | `ui.rate 5` |
| `Mod+=` | Give the selected track one more star | `ui.rate_up` |
| `Mod+-` | Give the selected track one star less | `ui.rate_down` |
| `Mod+P` | Add the selected tracks to the playlist named Prepare (if there is one) | `ui.add_to_prepare` |
| `Mod+Shift+P` | Open the add-to-playlist menu for the selected tracks | `ui.add_to` |
| `Mod+I` | Edit the selected track's tags | `ui.edit_track` |
| `Mod+Z` | Undo the last tag edit (title, artist, rating ...) | `ui.undo_edit` |
| `Mod+Shift+I` | Keyboard focus to the other track table | `ui.focus_pane` |
| `F5` | Show only tracks that mix with the loaded one | `ui.compatible` |

### Deck 1

| Shortcut | What it does | Control |
|---|---|---|
| `1` | Hot cue 1: set it, or jump to it while held | `deck1.hotcue.1 (hold)` |
| `2` | Hot cue 2: set it, or jump to it while held | `deck1.hotcue.2 (hold)` |
| `3` | Hot cue 3: set it, or jump to it while held | `deck1.hotcue.3 (hold)` |
| `4` | Hot cue 4: set it, or jump to it while held | `deck1.hotcue.4 (hold)` |
| `Q` | Hot cue 5: set it, or jump to it while held | `deck1.hotcue.5 (hold)` |
| `W` | Hot cue 6: set it, or jump to it while held | `deck1.hotcue.6 (hold)` |
| `E` | Hot cue 7: set it, or jump to it while held | `deck1.hotcue.7 (hold)` |
| `R` | Hot cue 8: set it, or jump to it while held | `deck1.hotcue.8 (hold)` |
| `Shift+1` | Delete hot cue 1 | `deck1.hotcue_clear.1` |
| `Shift+2` | Delete hot cue 2 | `deck1.hotcue_clear.2` |
| `Shift+3` | Delete hot cue 3 | `deck1.hotcue_clear.3` |
| `Shift+4` | Delete hot cue 4 | `deck1.hotcue_clear.4` |
| `Shift+Q` | Delete hot cue 5 | `deck1.hotcue_clear.5` |
| `Shift+W` | Delete hot cue 6 | `deck1.hotcue_clear.6` |
| `Shift+E` | Delete hot cue 7 | `deck1.hotcue_clear.7` |
| `Shift+R` | Delete hot cue 8 | `deck1.hotcue_clear.8` |
| `A` | CUE button (set / return / preview while held) | `deck1.cue (hold)` |
| `S` | Start or pause playback | `deck1.play` |
| `D` | Beat sync to the other deck | `deck1.sync` |
| `G` | Add a memory cue (body: text = name, color = 0xRRGGBB as a number) | `deck1.memory_cue_set` |
| `Shift+A` | Set the main cue point at the playhead | `deck1.cue_set` |
| `Shift+S` | Headphone cue for this channel | `deck1.pfl` |
| `Shift+D` | Keep the pitch when the tempo changes | `deck1.key_lock` |
| `Shift+F` | Reset the tempo fader to zero | `deck1.tempo 0` |
| `Mod+G` | Suggest cues for deck 1 from the phrase analysis (free hot slots only) | `deck1.suggest_cues` |
| `Shift+G` | Delete the memory cue nearest the playhead | `deck1.memory_cue_delete` |
| `Z` | Set the loop start | `deck1.loop_in` |
| `X` | Set the loop end and start looping | `deck1.loop_out` |
| `C` | Halve the loop | `deck1.loop_scale 0.5` |
| `V` | Double the loop | `deck1.loop_scale 2` |
| `B` | Toggle a loop | `deck1.loop` |
| `Shift+B` | Return to the last loop / exit | `deck1.reloop` |
| `5` | Jump back 4 beats | `deck1.beatjump -4` |
| `T` | Jump forward 4 beats | `deck1.beatjump 4` |
| `Shift+5` | Jump back 1 beat | `deck1.beatjump -1` |
| `Shift+T` | Jump forward 1 beat | `deck1.beatjump 1` |
| `Shift+Z` | Pitch bend down (nudge back) | `ui.nudge.1 -0.01 (repeat)` |
| `Shift+X` | Pitch bend up (nudge forward) | `ui.nudge.1 0.01 (repeat)` |
| `-` | Zoom the scrolling waveform out | `deck1.wave_zoom_out` |
| `=` | Zoom the scrolling waveform in | `deck1.wave_zoom_in` |
| `Mod+Alt+1` | Deck 1: mute / unmute the vocals stem | `deck1.stem.vocals.mute` |
| `Mod+Alt+2` | Deck 1: mute / unmute the drums stem | `deck1.stem.drums.mute` |
| `Mod+Alt+3` | Deck 1: mute / unmute the bass stem | `deck1.stem.bass.mute` |
| `Mod+Alt+4` | Deck 1: mute / unmute the other stem | `deck1.stem.other.mute` |
| `Mod+Alt+Q` | Deck 1: solo the vocals while held | `deck1.stem.vocals.isolate (hold)` |
| `Mod+Alt+W` | Deck 1: solo the drums while held | `deck1.stem.drums.isolate (hold)` |
| `Mod+Alt+E` | Deck 1: vocals only | `deck1.stems.acapella` |
| `Mod+Alt+R` | Deck 1: everything except the vocals | `deck1.stems.instrumental` |
| `Mod+Alt+5` | Deck 1: all stems back on | `deck1.stems.reset` |
| `Mod+Alt+A` | Deck 1: key sync on / off (follows the other deck's key as tempos change) | `deck1.key_sync` |
| `Mod+Alt+S` | Deck 1: match the other deck's key once | `deck1.key_match` |
| `Mod+Alt+F` | Deck 1: key shift back to 0, key sync off | `deck1.key_reset` |
| `Mod+Alt+Z` | Deck 1: key down one semitone | `ui.key_shift.1 -1` |
| `Mod+Alt+X` | Deck 1: key up one semitone | `ui.key_shift.1 1` |

### Deck 2

| Shortcut | What it does | Control |
|---|---|---|
| `7` | Hot cue 1: set it, or jump to it while held | `deck2.hotcue.1 (hold)` |
| `8` | Hot cue 2: set it, or jump to it while held | `deck2.hotcue.2 (hold)` |
| `9` | Hot cue 3: set it, or jump to it while held | `deck2.hotcue.3 (hold)` |
| `0` | Hot cue 4: set it, or jump to it while held | `deck2.hotcue.4 (hold)` |
| `U` | Hot cue 5: set it, or jump to it while held | `deck2.hotcue.5 (hold)` |
| `I` | Hot cue 6: set it, or jump to it while held | `deck2.hotcue.6 (hold)` |
| `O` | Hot cue 7: set it, or jump to it while held | `deck2.hotcue.7 (hold)` |
| `P` | Hot cue 8: set it, or jump to it while held | `deck2.hotcue.8 (hold)` |
| `Shift+7` | Delete hot cue 1 | `deck2.hotcue_clear.1` |
| `Shift+8` | Delete hot cue 2 | `deck2.hotcue_clear.2` |
| `Shift+9` | Delete hot cue 3 | `deck2.hotcue_clear.3` |
| `Shift+0` | Delete hot cue 4 | `deck2.hotcue_clear.4` |
| `Shift+U` | Delete hot cue 5 | `deck2.hotcue_clear.5` |
| `Shift+I` | Delete hot cue 6 | `deck2.hotcue_clear.6` |
| `Shift+O` | Delete hot cue 7 | `deck2.hotcue_clear.7` |
| `Shift+P` | Delete hot cue 8 | `deck2.hotcue_clear.8` |
| `;` | CUE button (set / return / preview while held) | `deck2.cue (hold)` |
| `L` | Start or pause playback | `deck2.play` |
| `K` | Beat sync to the other deck | `deck2.sync` |
| `H` | Add a memory cue (body: text = name, color = 0xRRGGBB as a number) | `deck2.memory_cue_set` |
| `Shift+;` | Set the main cue point at the playhead | `deck2.cue_set` |
| `Shift+L` | Headphone cue for this channel | `deck2.pfl` |
| `Shift+K` | Keep the pitch when the tempo changes | `deck2.key_lock` |
| `Shift+J` | Reset the tempo fader to zero | `deck2.tempo 0` |
| `Mod+J` | Suggest cues for deck 2 from the phrase analysis (free hot slots only) | `deck2.suggest_cues` |
| `Shift+H` | Delete the memory cue nearest the playhead | `deck2.memory_cue_delete` |
| `/` | Set the loop start | `deck2.loop_in` |
| `.` | Set the loop end and start looping | `deck2.loop_out` |
| `,` | Halve the loop | `deck2.loop_scale 0.5` |
| `M` | Double the loop | `deck2.loop_scale 2` |
| `N` | Toggle a loop | `deck2.loop` |
| `Shift+N` | Return to the last loop / exit | `deck2.reloop` |
| `6` | Jump back 4 beats | `deck2.beatjump -4` |
| `Y` | Jump forward 4 beats | `deck2.beatjump 4` |
| `Shift+6` | Jump back 1 beat | `deck2.beatjump -1` |
| `Shift+Y` | Jump forward 1 beat | `deck2.beatjump 1` |
| `Shift+,` | Pitch bend down (nudge back) | `ui.nudge.2 -0.01 (repeat)` |
| `Shift+.` | Pitch bend up (nudge forward) | `ui.nudge.2 0.01 (repeat)` |
| `[` | Zoom the scrolling waveform out | `deck2.wave_zoom_out` |
| `]` | Zoom the scrolling waveform in | `deck2.wave_zoom_in` |
| `Mod+Alt+7` | Deck 2: mute / unmute the vocals stem | `deck2.stem.vocals.mute` |
| `Mod+Alt+8` | Deck 2: mute / unmute the drums stem | `deck2.stem.drums.mute` |
| `Mod+Alt+9` | Deck 2: mute / unmute the bass stem | `deck2.stem.bass.mute` |
| `Mod+Alt+0` | Deck 2: mute / unmute the other stem | `deck2.stem.other.mute` |
| `Mod+Alt+U` | Deck 2: solo the vocals while held | `deck2.stem.vocals.isolate (hold)` |
| `Mod+Alt+I` | Deck 2: solo the drums while held | `deck2.stem.drums.isolate (hold)` |
| `Mod+Alt+O` | Deck 2: vocals only | `deck2.stems.acapella` |
| `Mod+Alt+P` | Deck 2: everything except the vocals | `deck2.stems.instrumental` |
| `Mod+Alt+6` | Deck 2: all stems back on | `deck2.stems.reset` |
| `Mod+Alt+;` | Deck 2: key sync on / off (follows the other deck's key as tempos change) | `deck2.key_sync` |
| `Mod+Alt+L` | Deck 2: match the other deck's key once | `deck2.key_match` |
| `Mod+Alt+J` | Deck 2: key shift back to 0, key sync off | `deck2.key_reset` |
| `Mod+Alt+,` | Deck 2: key down one semitone | `ui.key_shift.2 -1` |
| `Mod+Alt+.` | Deck 2: key up one semitone | `ui.key_shift.2 1` |

### Mixer

| Shortcut | What it does | Control |
|---|---|---|
| `Mod+Left` | Crossfader fully left | `mixer.crossfader 0` |
| `Mod+Right` | Crossfader fully right | `mixer.crossfader 1` |
| `Mod+Shift+Down` | Crossfader to the centre | `mixer.crossfader 0.5` |

### Other

| Shortcut | What it does | Control |
|---|---|---|
| `Mod+Shift+A` | Start or stop automix | `automix.run` |
| `Mod+Shift+K` | Pause or resume automix | `automix.pause` |
| `Mod+Shift+N` | Automix: start the next transition now, on the next bar | `automix.skip` |
| `Mod+Shift+R` | Start or stop the step sequencer | `seq.run` |
| `Mod+Shift+T` | Sequencer: record sampler hits into the pattern while it runs | `seq.record` |
| `Mod+Shift+X` | Sequencer: empty the pattern being played | `seq.clear` |
| `Mod+Alt+Shift+1` | Sequencer: play pattern 1 from the next bar | `seq.pattern.1.queue` |
| `Mod+Alt+Shift+2` | Sequencer: play pattern 2 from the next bar | `seq.pattern.2.queue` |
| `Mod+Alt+Shift+3` | Sequencer: play pattern 3 from the next bar | `seq.pattern.3.queue` |
| `Mod+Alt+Shift+4` | Sequencer: play pattern 4 from the next bar | `seq.pattern.4.queue` |
| `Mod+Alt+Shift+5` | Sequencer: play pattern 5 from the next bar | `seq.pattern.5.queue` |
| `Mod+Alt+Shift+6` | Sequencer: play pattern 6 from the next bar | `seq.pattern.6.queue` |
| `Mod+Alt+Shift+7` | Sequencer: play pattern 7 from the next bar | `seq.pattern.7.queue` |
| `Mod+Alt+Shift+8` | Sequencer: play pattern 8 from the next bar | `seq.pattern.8.queue` |

### Pre-listen

| Shortcut | What it does | Control |
|---|---|---|
| `Space` | Pre-listen to the selected track (or pause it), in the headphones | `ui.preview_toggle` |
| `Shift+Space` | Stop pre-listening | `ui.preview_stop` |
| `Alt+Left` | Skip back 10 s in the pre-listen | `ui.preview_seek -10 (repeat)` |
| `Alt+Right` | Skip forward 10 s in the pre-listen | `ui.preview_seek 10 (repeat)` |

<!-- END GENERATED -->
