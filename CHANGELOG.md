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

### Changed
- Core language is C++20 with thin platform shells (was a single-language plan).
