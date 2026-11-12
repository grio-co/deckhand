# DeckHand

A stable, skinnable, cross-platform DJ application with CDJ-compatible USB export and a
full control API.

> **Status: working pre-alpha on macOS.** Decks, mixer, effects, library, USB export, API and
> recording work and are covered by automated tests. It has not yet been tested with controller
> hardware or on a club player. See [ROADMAP.md](ROADMAP.md) and [TODO.md](TODO.md).

## What works today (macOS)
- Library: DeckHand's own SQLite library; read-only import from another DJ library (tracks,
  playlists, hot cues, beat grids); drag and drop; search; sort by column; playlists and folders
  (create, rename, delete, reorder); play history per session.
- Analysis: tempo with variable-tempo grids, downbeat, musical key and loudness (auto-gain), run
  once per track on first load.
- Decks: MP3, AAC, FLAC, WAV, AIFF; scrolling three-band waveforms with beat grid; cue, hot cues,
  loops, beat jump, tempo, beat sync, scratch and nudge; key lock and key shift; quantise; slip;
  reverse.
- Mixer and effects: trim, three-band EQ, filter and five other colour effects, faders,
  crossfader, master, separate headphone cue bus; three effect slots per channel and on the
  master (14 beat effects); 16-slot sampler.
- Recording of the master output to 24-bit WAV (the file stays playable if the app is killed).
- USB export for club players: database, analysis files and audio, from the app, the command
  line or the API. Written from public format documentation and verified by a read-back parser;
  **not yet tested on a player** (see [docs/EXPORT.md](docs/EXPORT.md)).
- API: REST + WebSocket with bearer tokens, per-scope access, TLS and scheduled commands
  ([docs/API.md](docs/API.md)).
- Streaming: now-playing files, a browser overlay and an OBS WebSocket connection
  ([docs/STREAMING.md](docs/STREAMING.md)); tested against a mock, not yet a real OBS.
- Languages: English, German, French, Spanish and Japanese packs; switch from the top bar.
- Controllers: declarative mapping files, any matching MIDI device; DDJ-FLX4 mapping included.
- Portable mode: put a file named `portable` beside the app and all data stays next to it.

Built but not yet connected to the UI: the skin format with its parser and layout solver
([docs/SKINS.md](docs/SKINS.md)); the app still draws its default layout in code.

## Goals
- **Stable.** The audio path never blocks; the engine is designed to run as its own process.
- **Skinnable.** Winamp-style themes, from familiar to exotic. A skin is declarative data, never code.
- **Portable.** Run it from a USB stick.
- **Compatible.** Export playlists and analysis to CDJ-3000 USB drives.
- **Scriptable.** Everything available through a REST + WebSocket API, with sample-accurate scheduling.
- **International.** Language packs install as modules, including help docs.
- **Streamer-friendly.** OBS integration for now-playing and scene control.
- **Cross-platform.** macOS first, then Windows and Linux; iOS in tandem.

## Building (macOS)
Requires Xcode, CMake, SQLCipher and OpenSSL (`brew install sqlcipher openssl`).

    scripts/build-macos.sh          # builds, bundles and signs build/DeckHand.app
    ctest --test-dir build          # core tests

The script needs a code-signing identity: set `DECKHAND_SIGN_IDENTITY` or put its name in
`.signing.local` (not committed).

## Documentation
[ARCHITECTURE.md](ARCHITECTURE.md) · [ROADMAP.md](ROADMAP.md) · [TODO.md](TODO.md) ·
[CHANGELOG.md](CHANGELOG.md) · [docs/](docs/) ([controllers](docs/CONTROLLERS.md),
[first hardware test](docs/TESTING-FLX4.md), [API](docs/API.md), [USB export](docs/EXPORT.md),
[skins](docs/SKINS.md), [languages](docs/I18N.md))

## Licence
MIT. See [LICENSE](LICENSE). Bundled third-party code: miniaudio (public domain / MIT-0).
DeckHand is an independent project, not affiliated with or endorsed by any DJ hardware or
software vendor. Product names are used only to state compatibility.
