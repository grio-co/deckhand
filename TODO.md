# TODO

Living list; milestones match [ROADMAP.md](ROADMAP.md). Tick items as they land and mirror
them in [CHANGELOG.md](CHANGELOG.md).

## Now: first hardware test
- [ ] Run docs/TESTING-FLX4.md on a real DDJ-FLX4; calibrate jog `ticks_per_rev`
- [ ] Confirm master / headphone level knob behaviour and output channel order
- [ ] Read controls' initial positions at connect (needs a documented method)

## Done
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

## Next
- [ ] Draw the UI from the skin (the layout is still drawn in code); skin chooser
- [ ] UI for effect slots, sampler, key shift, reverse; four-deck layout
- [ ] Audio device chooser and latency setting in the UI
- [ ] Relink and tag editing in the UI; settings screen (API, language, export options)
- [ ] Sample-accurate scheduled commands (they currently fire within a few milliseconds)
- [ ] Engine in its own process (survives a UI crash)
- [ ] Settings screen for streaming and OBS (today: settings through the API); test against a real OBS
- [ ] MIDI learn UI (the library support exists); HID controllers
- [ ] DDJ-FLX4: fill in the pad FX, sampler, keyboard, key shift, CFX and beat FX lines from the published MIDI list
- [ ] Long translated labels overflow in the mixer strip (seen in German)
- [ ] Listening test of key lock on real mixes; transient handling if needed
- [ ] USB export: check against a reference export, then the CDJ-3000 test; write exportExt.pdb
- [ ] Windows and Linux shells; CI on all three; vendored dependencies for portable builds
- [ ] iOS app (pull request open; touch layout pending)
- [ ] Cloud sync

## Known limits of the current build
- Tracks are decoded fully into memory.
- Analysis can split a steady track at a long breakdown; such splits are merged when the tempo agrees.
- The API has no OpenAPI document yet; docs/API.md is the reference.
- HID controllers and MIDI learn are not supported yet.
- Built against the host's Homebrew SQLCipher, so the app runs on this macOS version and newer.
