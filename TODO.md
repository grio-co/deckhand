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
- [ ] Sync: passphrase encryption on iOS (no OpenSSL there); encryption never tried across two real computers;
      removing tombstones altogether needs per-device acknowledgements
- [x] Sync grid locks, smart playlists and the browser settings
- [ ] Preparation follow-ups: reorder
      columns by dragging headers; fade when a preview replaces another; persist the main cue point;
      tests for the USB export glue; pinch zoom
- [ ] Bluetooth MIDI pairing: try with a real device
- [ ] Skins: port shaped and multi-window skins to the Windows and Linux shells (docs/SHELL-WINDOWS.md);
      drag tracks between windows; Window menu and shortcuts are untested; build a skull skin from
      the chosen concept in design/skull-skins
- [ ] Microphone: try a real input device (macOS permission prompt from the helper process is untested)
- [ ] HID: try a real HID controller; Windows HID code is compile-checked only; no HID mappings shipped yet
- [ ] Listening test of key lock on real mixes; transient handling if needed
- [ ] Test against a real OBS
- [ ] USB export: run `compare_exports` against a reference stick (docs/EXPORT.md says how to make one),
      then the CDJ-3000 test; 11 format details are still undocumented guesses
- [ ] Vendored dependencies for portable builds; CI does not yet run the shell screenshot checks
- [ ] iOS app (pull request open; touch layout pending; add `design/icon/ios/AppIcon.appiconset`)

## Known limits of the current build
- Tracks are decoded fully into memory.
- Analysis can split a steady track at a long breakdown; such splits are merged when the tempo agrees.
- HID controllers and MIDI learn are not supported yet.
- The database library is vendored and statically linked; the app still links the host's Homebrew OpenSSL (API server, sync), so it runs on this macOS version and newer.
