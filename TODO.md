# TODO

Living list; milestones match [ROADMAP.md](ROADMAP.md). Tick items as they land and mirror
them in [CHANGELOG.md](CHANGELOG.md).

## Now: first hardware test
- [ ] Run docs/TESTING-FLX4.md on a real DDJ-FLX4; calibrate jog `ticks_per_rev`
- [ ] Confirm master / headphone level knob behaviour and output channel order
- [ ] Read controls' initial positions at connect (needs a documented method)

## Done
- [x] Library search through an FTS5 trigram index (schema 3, triggers keep it current; LIKE scan below 3 characters or without FTS5): 100k tracks ~200 ms -> ~17 ms
- [x] Track table keeps ids and fetches only the visible rows: 100k-track refresh 0.2-0.3 s -> 10-50 ms (tools/bench_library.cpp)
- [x] Repository, CMake skeleton, module layout, licence, schema
- [x] M1 Library storage and read-only importer, `deckhandctl`
- [x] Audio engine (in-process), decoding, waveforms
- [x] Generic controller mappings + DDJ-FLX4 mapping (untested on hardware)
- [x] macOS shell, core-drawn UI, signed app bundle, portable mode
- [x] Playlist editing, column sorting, tag editing (API), relinking (library), play history
- [x] Analysis: variable tempo, downbeat, key, loudness / auto-gain
- [x] Key lock, key shift, quantise, slip, reverse; effect slots, colour effects, sampler (engine)
- [x] Recording to WAV
- [x] REST + WebSocket API: tokens, scopes, TLS, scheduled commands, live events
- [x] Language packs (en, de, fr, es, ja) and runtime switching
- [x] Skin format: parser, validator, layout solver, default and example skins
- [x] USB export writer and read-back parser (offline-verified only)
- [x] Streaming: now-playing files, browser overlay, OBS WebSocket client and rules (mock-tested only)
- [x] Controllers: pad modes, modifiers, soft takeover, actions for every engine function, learn (library)
- [x] Effect slot controls on the deck panels
- [x] UI drawn from skins (default two- and four-deck layouts, example skin), live skin chooser
- [x] Settings, sampler, MIDI learn and track details panels; key shift, reverse, tempo fader
- [x] Sample-accurate scheduled commands (clock, deck position, beat) through the API
- [x] DDJ-FLX4 mapping completed from the published MIDI list (untested on hardware)
- [x] OpenAPI document
- [x] Engine in its own process on macOS: audio, recording and controllers survive a UI crash;
      the UI re-attaches
- [x] Library sync through any replicated folder or USB stick (no server)
- [x] Windows port: Win32 + Direct3D 11 shell, DirectWrite text, Media Foundation decoder, WinMM
      MIDI, Windows IPC layer (cross-compiled; tests pass under Wine; not yet run on Windows)
- [x] Linux on the current tree with the engine helper, audio fallback and a portable tarball
- [x] macOS: Bluetooth MIDI pairing window, sync settings, skin images and rotation
- [x] Linux port: build, FFmpeg decoder, ALSA MIDI, SDL3 + OpenGL shell (container-tested only)
- [x] Controllers: effect unit assignment, master cue, learn choices by name

## Next
- [ ] Embedding (docs/EMBEDDING.md): test input and audio inside Rig; Rig side wires `DeckHandKit` (Rig session)
- [ ] Linux: test on a real machine (audio, MIDI, Wayland, GPU drivers)
- [ ] Windows on the real PC: WASAPI audio (incl. the Remote Desktop device), MIDI, HID, sleep/resume,
      DPI change while running; no UI-level single-instance guard yet
- [x] Sync: passphrase encryption, tombstone clean-up (long-deleted details dropped at compaction)
- [x] Sync: encrypted sync checked between a Mac and a Windows PC with test data (docs/SYNC.md)
- [ ] Sync: passphrase encryption on iOS (no OpenSSL there);
      removing tombstones altogether needs per-device acknowledgements
- [x] Sync grid locks, smart playlists and the browser settings
- [x] Reorder columns by dragging headers; pinch zoom on the waveform (macOS; not yet tried on a trackpad)
- [x] Preview fades out when another replaces it; tests for the USB export glue
- [ ] Preparation follow-ups: pinch zoom on Linux, check it on a Windows touchpad
- [ ] Bluetooth MIDI pairing: try with a real device
- [ ] Skins: port shaped and multi-window skins to the Windows and Linux shells (docs/SHELL-WINDOWS.md);
      drag tracks between windows; Window menu and shortcuts are untested; the `skull` skin exists (try it on
      screen; default scale and module positions need a look; loop teeth need loop in/out bindings)
- [ ] Microphone: try a real input device (macOS permission prompt from the helper process is untested)
- [ ] HID: try a real HID controller; Windows HID code is compile-checked only; no HID mappings shipped yet
- [ ] Listening test of key lock on real mixes; transient handling if needed
- [ ] Test against a real OBS
- [ ] USB export: run `compare_exports` against a reference stick (docs/EXPORT.md says how to make one),
      then the CDJ-3000 test; 11 format details are still undocumented guesses
- [ ] Vendored dependencies for portable builds; CI does not yet run the shell screenshot checks
- [ ] iOS app: feature parity with the desktop build is the goal (owner, 2026-11-26); pull request
      open; merge main into the iOS branch, touch layout, `design/icon/ios/AppIcon.appiconset`

## Target feature set (owner's list, 2026-11-26: parity with the leading club-standard prep and
## performance software, and more). `[x]` exists, `[~]` partly or in progress, `[ ]` not started.

Library and discovery
- [x] Analysis: BPM, variable beat grid, key, loudness, three-band waveforms
- [x] First-sound load cue (user's main cue remembered), grid drift fix, downbeat tests (synthetic audio only)
- [~] Phrase analysis shown on the waveform (tuned on synthetic tracks; unseen on screen; builds at
      150 BPM and above can read as drops)
- [~] Vocal regions on the main waveform from cached stems (synthetic tests only; not on the overview)
- [~] Cue suggestions from phrases and the user's habits (free slots only, undoable; suggested cues
      currently feed back into the habit summary)
- [~] Track suggestions from the library by tempo, key, genre, energy and play history (unseen on screen)
- [~] Two-player audition for set preparation: preparation mode on decks 3/4, headphones only (unheard)
- [~] Browser: column view, filters, smart playlists, layout presets, a second pane, recursive folder
      view, relocation of missing files (all unseen on screen; no playlist trash)
- [~] Shared (collaborative) playlists through the sync folder (API; no menu entry yet)
- [x] Metadata safety: edit history, undo, restorable trash (API, CLI, panels; panels unseen)
- [x] Library import/export through the common DJ library XML and M3U8 (untested against other applications' files)
- [ ] Streaming services and promo pools: each needs a partner agreement and account; pick services
      with the owner before any work
- [x] USB export for club players (offline-verified; hardware test pending)
- [x] Export compatibility check and on-stick 24-bit conversion (rule table unverified against hardware; no panel for the report yet)
- [~] Backup, verify and restore of an exported device library (API and CLI; untested on a real stick); SD card export

Performance
- [x] Four decks, beat sync, key shift/lock, quantise, slip, loops, sampler, effect slots, colour effects
- [~] Key sync and 16 hot cue slots (unheard, unseen; USB export writes the first eight cues and reports the rest)
- [~] Stems: offline separation with a disk cache (in progress); then per-deck stem playback,
      mute/solo/FX per stem, stem pads on controllers
- [~] Linked mix points between tracks, used by automix and suggestions (unseen, unheard; synced between devices)
- [~] Sampler: 16 slots, 8-pattern step sequencer, sample scratch, loop slicing (unheard; steps play whole samples, not slices)
- [~] More effects: Tape Echo, Beat Reverb, Tape Stop added (unheard); still open: build-up/drop "merge" effect, isolator and mixer-style effects
- [ ] Timecode vinyl control (DVS): needs an audio interface with phono inputs to test
- [x] MIDI learn, custom mappings (`.dhmap`), master output from the computer
- [~] Custom keyboard shortcuts: keymap, defaults, panel (never run on screen; Linux change uncompiled; Windows on fixed keys)

Visuals, lighting, recording
- [ ] Video decks: mix video files, stills and camera input in step with the audio
- [~] Lighting: DMX output (Art-Net / sACN / serial) driven by beats and phrases (off by default; no real fixture or node tried)
- [x] Mix recording
- [ ] Record from an analogue input (vinyl) with track splitting; export an edited track

Sync
- [x] Library sync through any replicated folder, passphrase-encrypted
- [~] Sync the audio files themselves (optional, per playlist) and analysis data (macOS test data
      only; grid, key, tempo and gain travel with the track, not the analysis bundle; no clean-up
      of stored audio no playlist uses; a re-analysis elsewhere can still replace a locked grid)
- [ ] Loading from cloud storage directly on club players is a closed vendor service: not possible
      clean-room; the answer here is fast, verified USB/SD export

Gaps in the incumbent that DeckHand should close (owner's list, 2026-11-26)
- [~] Pre-analysed stems: built-in inference engine (no outside runtime) runs the two Demucs models
      offline, verified numerically against a reference on synthetic audio (nobody has listened yet);
      cache, queue, installer and catalogue exist; no model ships (user installs from the authors'
      host). Stem playback on the decks with per-stem mute/solo/level/effects exists (unheard;
      42 MB of memory per minute per deck). The RoFormer vocal model and the RoFormer + Demucs recipe run (slower than
      real time; unheard). A stems pad mode for
      controllers and stems on the USB stick exist (untested on hardware)
- [~] Fluid beat grid: drifting tempo, live drummers and tempo jumps become as many sections as
      needed (within ~10 ms on synthetic tests); untested on real recordings
- [x] Cues follow the grid (API; the in-app confirm is still a status line only)
- [x] Remove analysis selectively across many tracks (API and CLI)
- [~] Automix and play queue with per-track points and "play next" (simulation-tested; never heard;
      untested with the separate engine process; fader/EQ moves are quarter-beat steps)
- [x] Duplicate finder with merge (panel, API, CLI)
- [ ] Opt-in metadata and key fill from an open music database
- [~] Effect chains per deck/master/mic/sampler/stem bus with a chain editor and Audio Unit
      plug-ins on macOS (unseen, unheard; only Apple's built-in units loaded so far; no plug-in
      windows or host tempo); VST3 hosting not started
- [~] Any controller: open `.dhmap` mappings and learn exist; ship mappings for common controllers
      from other makers (each untested until someone has the hardware)
- [x] No paid tiers: recording, two-player preparation, filters and sync are in the one free build
- [x] Sync through any replicated folder, not tied to one cloud provider

Beyond the list
- [x] Skins with shaped, floating windows; REST + WebSocket API; engine in its own process;
      OBS integration; portable mode; no account, no subscription

## Known limits of the current build
- The new REST routes for chains, plug-ins, browser and sync are compiled and checked against
  the OpenAPI document but no test calls them.
- A plug-in's first load holds chain edits for up to 8 s while it is test-loaded.
- If the engine helper misses its start-up limit (60 s), the app falls back to in-process audio
  and the late helper exits by itself; the wait shows only in the status line and the log.
- Controller panel: channel fader and crossfader gain laws, EQ kill depth and trim range need the
  engine; no mappings for other makers' controllers yet (each needs the maker's MIDI document).
- The single-instance lock and the shell messages are compile-checked only on Windows; the Swift
  shell change has not been run.
- Tracks are decoded fully into memory.
- Analysis can split a steady track at a long breakdown; such splits are merged when the tempo agrees.
- HID controllers and MIDI learn are not supported yet.
- The database library is vendored and statically linked; the app still links the host's Homebrew OpenSSL (API server, sync), so it runs on this macOS version and newer.
