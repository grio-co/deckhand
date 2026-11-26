# Changelog

All notable changes to DeckHand. Format follows [Keep a Changelog](https://keepachangelog.com/);
versions follow [Semantic Versioning](https://semver.org/) once releases begin.

## [Unreleased]

### Added
- Automix and play queue: add to end or "play next", reorder, endless fill from a playlist; per-track
  mix-in/mix-out points, transition length and type, maximum play time; crossfade with bass swap,
  cut, fade and echo-out, started on a bar line with the incoming beat aligned (0.3 samples in the
  offline simulation) and the tempo ramped back to the track's own; controls you touch are handed
  back. Queue tab in the Tools panel. Library schema 7. Nobody has listened to a transition yet.
- Phrase analysis: sections (intro, build, drop, breakdown, outro...) with an energy level, drawn
  on the waveform; cue suggestions written to free slots only as one undoable step, biased toward
  the user's own cue habits; phrase-based mix points for automix. Library schema 8. Tuned on
  synthetic arrangements only.
- Effect chains: up to eight effects in any order on each deck, the master, the microphone and
  the sampler, edited without clicks (tails ring out); new isolator, slicer, build-up and one-knob
  filter; chain presets; controls and REST routes (`/v1/fx/chains/...`). Engine protocol 5.
- Cues follow the grid: committing a grid change offers to move on-beat cues and loops with it, as
  one undoable step (`cues_follow_grid` setting). Off-beat cues stay.
- Selective analysis removal across many tracks (grid, key, loudness, waveform, first sound, stems).
- Duplicate finder (same audio, or same artist/title/length) with merge of cues, playlists and
  history into the kept track; removed entries go to the trash; audio files are never touched.
  Library schema 6.
- "Library tools" panel: stem models, trash, edit history, duplicates. Keymap over the API.
- Stem separation groundwork: seamless chunked separation that sums back to the input, a
  content-keyed disk cache, a background queue that pauses while a deck plays, and a model
  installer that fetches weights from their authors on the user's request after showing the
  licence ([docs/STEMS.md](docs/STEMS.md)). No model ships.
- Stems: a built-in inference engine (no outside runtime; Accelerate on macOS, portable fallback
  elsewhere) runs the two Demucs four-stem models offline; results agree with a reference
  implementation to about 5e-4 on synthetic audio. Roughly 3x real time for the standard model on
  an Apple silicon laptop under load.
- Metadata safety: every tag edit is journalled per field (up to 200 per track) and can be undone
  singly or as a batch; imports no longer overwrite fields edited by hand unless asked; deleted
  tracks go to a trash that keeps cues, grid and playlist places for 30 days. Library schema 5.
  DeckHand never writes to audio files ([docs/BACKUP.md](docs/BACKUP.md)).
- Library interchange: import and export of the whole library (tags, cues, loops, grids, playlist
  tree) through the common DJ library XML, and M3U8 playlists in and out
  ([docs/INTERCHANGE.md](docs/INTERCHANGE.md)). Not yet tried against files from other applications.
- USB export compatibility check: reports before export which files a club player will not read
  (for example 32-bit float WAV), and can write a 24-bit copy to the stick instead. The rule table
  is unverified against hardware ([docs/EXPORT.md](docs/EXPORT.md)).
- Analysis finds the first audible sound and suggests a load cue on the first downbeat; tracks load
  at the user's own main cue (now remembered), else at that suggestion (`load_cue_mode` setting).
- Customisable keyboard shortcuts (`.dhkeys`), two-handed defaults for the whole workflow, a
  shortcuts panel with rebinding and a printable cheat sheet ([docs/SHORTCUTS.md](docs/SHORTCUTS.md)).
  macOS and Linux forward full key events; Windows keeps the fixed keys for now. Untested on screen.
- Sync: grid locks, smart playlists (including rules that name a playlist) and the browser
  column and key-notation settings now carry between devices.
- Free-form skins on macOS: transparent, shaped windows whose empty areas let clicks through
  to the desktop; modules (library, sampler, panels) as separate floating windows with
  remembered positions, snapping and group dragging, uniform scaling; sliders along a path
  and arc meters; the `orbit` example skin; optional pop-out library in the default skin.
  Shells that do not support this keep the single-window layout ([docs/SHELL-WINDOWS.md](docs/SHELL-WINDOWS.md)).
- Application icon: a ship's helm whose rim is a record. macOS .icns, Windows .ico, Linux
  desktop icons and window icon, an iOS icon set, overlay favicon; `tools/make_icons.sh`
  regenerates everything ([docs/BRAND.md](docs/BRAND.md)).
- Skin concepts: ten skull skin mockups in `design/skull-skins/` (designs only, not yet skins).
- Windows: the engine runs as a separate process by default (music survives a UI crash or
  kill; the UI re-attaches); `deckhandctl hid` lists HID devices; engine-check scripts.
- SQLCipher is now built from a pinned, vendored source on every platform with full-text
  search enabled; the Homebrew, distribution and vcpkg database packages are no longer used.
  The macOS app uses the system crypto for it. CI checks a screenshot on Linux and Windows.
- Track preparation: beat-grid editing (shift, first beat and downbeat at the playhead, BPM
  nudge, halve/double, tap tempo, tempo sections, lock, undo) with a headphone-only metronome;
  memory cues, saved loops, named and coloured hot cues, draggable cue markers.
- Waveform zoom and scrubbing; overview with cue and loop markers.
- Pre-listen from the browser on the headphone bus only (never on master unless enabled).
- Browser: configurable columns, ratings, colour tags, multi-select, drag onto playlists and
  reorder, compatible-key filter, missing-file marker; smart playlists (exportable to USB).
- Library schema 4 (copy made before upgrading); engine IPC protocol 4; new API routes and
  controls; `tools/check_openapi.py` keeps the OpenAPI file in step with the routes.
- Continuous integration on GitHub for macOS, Linux and Windows (build and all test suites).
- Microphone channel: input device chooser, level, EQ, noise gate, talkover ducking, optional
  headphone monitoring; included in recordings and the stream output; controls in the API,
  skin and controller mappings. The input is only opened after a device is chosen.
- HID controllers: `.dhmap` rules for HID reports (buttons, absolute values, wrapping jog
  counters, touch, LED output reports, init reports), learn for HID, platform layers for macOS,
  Linux and Windows. Keyboards and mice are always refused.
- USB export: structural validator, comparison tool for a reference export, experimental
  `exportExt.pdb`, atomic staging with rollback; index-page header and colour-preview layout
  corrected against the public documentation.
- Library sync keeps what it had published when a library is restored from an older backup; the source-library
  import no longer starts by itself without its key file
- Library sync: optional passphrase encryption of the sync folder (AES-256-GCM, scrypt; Settings, Library sync);
  logs compact by themselves past 64 files, dropping the details of items deleted over 180 days ago
- Library backups: an automatic copy before any schema upgrade, and `deckhandctl backup | backups | restore` (docs/BACKUP.md)
- Windows: first fixes for the Visual Studio (MSVC) build found on a real PC (atomic store, 64-bit seek, duplicate manifest); not yet rebuilt there
- Library search uses an FTS5 trigram index (schema 3); falls back to the LIKE scan for 1-2 character terms or an SQLite without FTS5
- `Library::track_ids()`; the track table holds ids and loads only visible rows (large libraries refresh in tens of ms)
- Embedding on macOS: `DeckHandKit` Swift package (inline view, pop-out window, helper path and folders set by the host),
  `scripts/build-embed.sh`, `DECKHAND_ENGINE_HELPER` override; the standalone app builds from the same sources (docs/EMBEDDING.md)
- macOS application: Swift shell (window, Metal renderer, CoreText text, input, drag and drop)
  over the C++ core; build, bundle and signing script.
- Audio engine: four decks, varispeed playback with scratch and nudge, cue and hot cues, loops,
  beat jump, tempo ranges, beat sync with phase alignment, trim, three-band EQ, filter, faders,
  crossfader, master, separate headphone cue bus, level meters, soft clipping.
- Decoding of MP3, AAC, FLAC, WAV and AIFF through the system codecs; three-band waveforms;
  tempo and beat-phase detection for tracks without analysis.
- Library storage (SQLite) with playlists, cues and beat grids; repeatable read-only importer
  for another DJ library's database; `deckhandctl` command-line tool.
- Generic controller layer: declarative `.dhmap` mapping files, automatic device matching, LED
  and meter feedback, automatic switch to a controller's audio interface. DDJ-FLX4 mapping.
- UI drawn by the core: scrolling waveforms with beat grid and cue markers, deck panels, mixer,
  playlist tree, searchable track table.
- Portable mode, file and console logging with four levels plus trace.
- Shell interface header (`capi.h`) shared by the macOS and iOS shells.
- Tests: library, engine (offline rendering), controller mapping.
- Design docs: architecture, roadmap, API, skins, internationalisation, logging, controllers.

- Library editing: playlists and folders (create, rename, delete, move, reorder), sort by column,
  tag edits, path relinking, play history, settings store, schema migrations.
- Analysis module: beat tracking with variable tempo, downbeat, key and loudness; results are
  stored on first load and the loudness trim is applied automatically.
- Engine: key lock and key shift, quantise, slip, reverse, loop move; three effect slots per
  channel and master with 14 effects, six colour effects, 16-slot sampler; master recording to
  24-bit WAV; a stream tap for broadcast output; short crossfades at loop wraps and jumps.
- USB export for club players (database, analysis files, audio) with a read-back parser;
  `deckhandctl export`; Export USB in the app. Offline-verified only.
- REST + WebSocket API server with bearer tokens, scopes, TLS, rate limiting, scheduled
  commands and live deck, mixer and event topics; `deckhandctl token`.
- One table of named controls shared by the API (and later skins and scripts).
- Language packs (`.dhlang`) with plural rules; English, German, French, Spanish, Japanese.
- Skin format (`.dhskin`): parser, validator, layout solver, default and example skins.
- Streaming: now-playing text files and tracklist, a browser overlay for OBS, an OBS WebSocket
  client with track-change rules ([docs/STREAMING.md](docs/STREAMING.md)); settings routes in the API.
- Controllers: pad modes (hot cue, beat loop, beat jump, sampler, keyboard, key shift, pad FX),
  modifiers and `when=` conditions, soft takeover, dead zones, mapping actions and LED states for
  every engine function, MIDI learn support, two example mappings.
- Effect slot controls on each deck panel; REC button; key lock, quantise and slip buttons.
- The UI is drawn from skins: default skin with two- and four-deck layouts, an example skin,
  live skin switching; a minimal built-in layout remains as a fallback.
- Panels: settings (language, skin, audio device and buffer, API, streaming), sampler, MIDI
  learn, track details (tags, relink, remove); key shift, reverse, tempo range and tempo fader.
- Scheduled commands execute in the engine on the exact output sample: at a clock time, at a
  deck position, or on a beat or bar of a deck's grid.
- DDJ-FLX4 mapping completed from the published MIDI list: all pad modes, shift layer, beat FX,
  CFX button.
- OpenAPI 3.1 description of the REST API (`docs/openapi.yaml`).
- Linux port: CMake support, FFmpeg decoder, ALSA sequencer MIDI, SDL3 + OpenGL shell with
  Pango text, build scripts and a container build ([docs/LINUX.md](docs/LINUX.md)).
- Controllers: effect units assignable to a deck or the master, master cue, learn choices.
- The audio engine runs in its own signed helper process on macOS (`deckhand-engine`): it owns
  the output device, recorder and controllers, keeps playing if the UI crashes or is killed, and
  a relaunched UI re-attaches to the running decks. In-process mode remains (`engine.process`).
- Library sync without a server: per-device change logs in any replicated folder or USB stick,
  merged field by field with convergent playlist ordering ([docs/SYNC.md](docs/SYNC.md)).
- Windows port: Win32 shell with a Direct3D 11 renderer and DirectWrite text, Media Foundation
  decoding, WinMM MIDI, Winsock and shared-memory layers, cross-build container and a native
  build script ([docs/WINDOWS.md](docs/WINDOWS.md)). Tests pass under Wine; not yet run on Windows.
- Build option `DECKHAND_WITH_API` to leave out the API server where OpenSSL is unavailable.
- Tests for the library, analysis, API, export, effects, scheduling, languages, skins and streaming.

### Fixed
- Tempos within 0.05 of a whole number were rounded, drifting about 20 ms over six minutes
  (174.02 BPM); they now snap only when the drift stays under 3 ms.
- Windows: minimum window size no longer exceeds the screen at high display scaling; the
  `--screenshot` canvas scales with `DECKHAND_SCALE`; quit on session end stops the helper.
- Tests: the analysis speed check is a real-time-factor bound; the export test uses its own temp folder.
- API server: a WebSocket close frame is no longer lost on Windows when the client is still sending.
- `build-windows.ps1`: an existing build folder keeps its CMake generator, so configuring no
  longer fails after a newer Visual Studio is installed.
- Windows paths are no longer stored with a leading slash in the library and sync code.
- The macOS build script no longer bundles a stale core library after a compile error.
- iOS: the engine no longer overrides the audio session the shell configures.

### Changed
- Core language is C++20 with thin platform shells (was a single-language plan).
