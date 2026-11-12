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
- [x] UI drawn from skins (default two- and four-deck layouts, example skin), live skin chooser
- [x] Settings, sampler, MIDI learn and track details panels; key shift, reverse, tempo fader
- [x] Sample-accurate scheduled commands (clock, deck position, beat) through the API
- [x] DDJ-FLX4 mapping completed from the published MIDI list (untested on hardware)
- [x] OpenAPI document
- [x] Engine in its own process on macOS: audio, recording and controllers survive a UI crash;
      the UI re-attaches
- [x] Library sync through any replicated folder or USB stick (no server)
- [x] Linux port: build, FFmpeg decoder, ALSA MIDI, SDL3 + OpenGL shell (container-tested only)
- [x] Controllers: effect unit assignment, master cue, learn choices by name

## Next
- [ ] Linux: rerun the container build against the skin-driven UI; fall back to no audio device
      cleanly; audio-route change notifications; test on a real machine (audio, MIDI, Wayland)
- [ ] Windows port (after Linux; built and tested on the Windows PC)
- [ ] Engine process: Windows IPC layer; ship the helper in the Linux install; translate its status
      messages; restore sampler slot names on re-attach
- [ ] Sync: settings UI for the folder (today: setting `sync.folder`), passphrase encryption,
      tombstone clean-up
- [ ] Bluetooth MIDI pairing window on macOS
- [ ] Skins: window shapes, images/sprites and rotate animations are parsed but not drawn;
      hidden widgets keep their layout space
- [ ] Controllers: microphone input (engine has none yet); HID controllers
- [ ] Listening test of key lock on real mixes; transient handling if needed
- [ ] Test against a real OBS
- [ ] USB export: check against a reference export, then the CDJ-3000 test; write exportExt.pdb
- [ ] CI on all platforms; vendored dependencies for portable builds
- [ ] iOS app (pull request open; touch layout pending)

## Known limits of the current build
- Tracks are decoded fully into memory.
- Analysis can split a steady track at a long breakdown; such splits are merged when the tempo agrees.
- HID controllers and MIDI learn are not supported yet.
- Built against the host's Homebrew SQLCipher, so the app runs on this macOS version and newer.
