# DeckHand

A stable, skinnable, cross-platform DJ application with CDJ-compatible USB export and a
full control API.

> **Status: first working build on macOS (pre-alpha).** Two decks, mixer, library import and
> generic controller support work. It has not yet been tested with controller hardware.
> Much of the roadmap is still ahead; see [ROADMAP.md](ROADMAP.md) and [TODO.md](TODO.md).

## What works today (macOS)
- Library: DeckHand's own SQLite library; read-only import from another DJ library (tracks,
  playlists, hot cues, beat grids); drag and drop audio files; search.
- Decks: MP3, AAC, FLAC, WAV, AIFF; scrolling three-band waveforms with beat grid; cue, hot cues,
  loops, beat jump, tempo, beat sync, scratch and nudge; tempo detection for unanalysed tracks.
- Mixer: trim, three-band EQ, filter, faders, crossfader, master, separate headphone cue bus.
- Controllers: declarative mapping files, any matching MIDI device; DDJ-FLX4 mapping included.
- Portable mode: put a file named `portable` beside the app and all data stays next to it.

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
Requires Xcode, CMake and SQLCipher (`brew install sqlcipher`).

    scripts/build-macos.sh          # builds, bundles and signs build/DeckHand.app
    ctest --test-dir build          # core tests

The script needs a code-signing identity: set `DECKHAND_SIGN_IDENTITY` or put its name in
`.signing.local` (not committed).

## Documentation
[ARCHITECTURE.md](ARCHITECTURE.md) · [ROADMAP.md](ROADMAP.md) · [TODO.md](TODO.md) ·
[CHANGELOG.md](CHANGELOG.md) · [docs/](docs/) ([controllers](docs/CONTROLLERS.md),
[first hardware test](docs/TESTING-FLX4.md))

## Licence
MIT. See [LICENSE](LICENSE). Bundled third-party code: miniaudio (public domain / MIT-0).
DeckHand is an independent project, not affiliated with or endorsed by any DJ hardware or
software vendor. Product names are used only to state compatibility.
