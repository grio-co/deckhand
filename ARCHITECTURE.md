# DeckHand architecture

No scripting languages in the product. macOS, Windows, Linux. MIT licence.

## Languages
- **Shared C++20 core** (everything under `src/`) builds on all three OSes: engine, library,
  import, export, analysis, skins, rendering, API. The CDJ export writer and skin renderer
  exist exactly once.
- **macOS shell in Swift** (`platform/macos`): app lifecycle, windowing, CoreAudio/CoreMIDI
  glue, packaging, signing. **Windows/Linux shells in C++** (`platform/windows`, `platform/linux`).
- Build: CMake. Platform code stays thin; logic lives in the core.
- **macOS first**, then Windows, then Linux. Core stays platform-neutral from day one.

## Processes
- `deckhand-engine`: owns audio and controller I/O and serves the API. Independent of the UI,
  so a UI hang or crash never interrupts playback; the UI reconnects.
- `deckhand`: skin-driven GPU UI; a client of the API like any other.
- `deckhandctl`: import, analyse, export without the UI.

## Modules (`src/`, dependencies point downward)
    engine, ui, cli
    library, import, export, analysis, audio, midi, broadcast, sync, api, skin, gfx, i18n
    log, core

- `core`: model + command/event vocabulary. No I/O.  `log`: see docs/LOGGING.md.
- `audio`: realtime-safe; no locks, allocation, logging or disk in the callback.
- `import`: read-only, works on copies of source libraries.
- `export`: builds CDJ USB output from the public format documentation.
- `broadcast`: OBS WebSocket + now-playing files; own thread, bounded drop-on-full queue.
- Rule: a module may depend only on modules below it.

## Portable mode
- If a `portable` marker file sits beside the executable, ALL state (library, settings,
  skins, language packs, caches, logs, recordings) lives in a data folder next to it.
  Otherwise per-user directories. One exFAT USB stick holds mac/win/linux builds sharing
  one data folder. Track paths are stored relative to a library root; a relink tool
  repairs moved drives.

## Library ownership
- DeckHand owns its library. Other DJ libraries are read-only sources, readable live on
  demand; DeckHand never writes to them.

## Default look and skins
- Default skin: familiar arrangement (library below, two decks above, waveforms, mixer
  between) with original artwork, icons, fonts, colours, names and wording. No third-party
  branding or assets. It is itself a normal skin. See docs/SKINS.md.

## Other docs
- docs/API.md (REST + WebSocket, ACL scopes, TLS, bearer tokens)
- docs/LOGGING.md, docs/I18N.md, docs/SKINS.md

## Recording, history, sync
- Recording: master (and optionally stream bus) to WAV/FLAC on a disk thread with its own
  buffer; cannot glitch playback. History log: every play with timestamps and deck; exportable.
- Sync: opt-in, user-chosen store (folder/S3/WebDAV), encrypted, conflict-aware. No DeckHand server.

## Rules
- Clean-room: no third-party branding, assets or copied material; no decompiling.
- Check every dependency's licence before adding it (MIT project). Avoid GPL; JUCE is out.
  AAC decoding uses OS codecs (AudioToolbox, Media Foundation) to avoid codec licensing.
- Source libraries are never modified; exports are rebuilt from scratch.
