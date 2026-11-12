# Changelog

All notable changes to DeckHand. Format follows [Keep a Changelog](https://keepachangelog.com/);
versions follow [Semantic Versioning](https://semver.org/) once releases begin.

## [Unreleased]

### Added
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
- Tests for the library, analysis, API, export, effects, languages, skins and streaming.

### Fixed
- iOS: the engine no longer overrides the audio session the shell configures.

### Changed
- Core language is C++20 with thin platform shells (was a single-language plan).
