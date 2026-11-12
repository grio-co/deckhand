# DeckHand

A stable, skinnable, cross-platform DJ application with CDJ-compatible USB export and a
full control API.

> **Status: early development.** The project structure and design are in place; features are
> being built milestone by milestone (see [ROADMAP.md](ROADMAP.md)). Nothing is usable yet.

## Goals
- **Stable.** The audio engine runs in its own process, so a UI hang never stops the music.
- **Skinnable.** Winamp-style themes, from familiar to exotic. A skin is declarative data, never code.
- **Portable.** Run it from a USB stick; all state lives beside the app.
- **Compatible.** Export playlists and analysis to CDJ-3000 USB drives.
- **Scriptable.** Everything is available through a REST + WebSocket API, with sample-accurate scheduling.
- **International.** Language packs install as modules, including help docs.
- **Streamer-friendly.** OBS integration for now-playing and scene control.
- **Cross-platform.** macOS first, then Windows and Linux.

## Building (macOS)
    cmake -S . -B build && cmake --build build -j && ctest --test-dir build

## Documentation
[ARCHITECTURE.md](ARCHITECTURE.md) · [ROADMAP.md](ROADMAP.md) · [TODO.md](TODO.md) ·
[CHANGELOG.md](CHANGELOG.md) · [docs/](docs/)

## Licence
MIT. See [LICENSE](LICENSE). DeckHand is an independent project, not affiliated with or
endorsed by any DJ hardware or software vendor.
