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
- [x] Audio engine (in-process), decoding, waveforms, tempo detection
- [x] Generic controller mappings + DDJ-FLX4 mapping (untested on hardware)
- [x] macOS shell, core-drawn UI, signed app bundle, portable mode

## Next
- [ ] Engine in its own process (survives a UI crash)
- [ ] Key lock / time-stretch; quantise; slip mode
- [ ] Pad FX, sampler, keyboard and key-shift pad modes; beat FX section
- [ ] Playlist editing; relink missing tracks; tag editing; sort by column
- [ ] Audio device chooser and latency setting in the UI; four-deck layout
- [ ] Skin engine and built-in skin (replaces the layout drawn in code)
- [ ] Language packs (`.dhlang`)
- [ ] REST + WebSocket API with scheduled commands
- [ ] Recording, history log
- [ ] OBS integration
- [ ] Full analysis: variable tempo, downbeats, key
- [ ] CDJ USB export (offline-verified), then the CDJ-3000 test with a reference export
- [ ] Windows and Linux shells; CI on all three; vendored dependencies for portable builds
- [ ] iOS app (in progress in `platform/ios`)
- [ ] Cloud sync

## Known limits of the current build
- Tracks are decoded fully into memory; tempo changes pitch.
- Tempo detection assumes a steady tempo and does not find the downbeat.
- HID controllers and MIDI learn are not supported yet.
- Built against the host's Homebrew SQLCipher, so the app runs on this macOS version and newer.
