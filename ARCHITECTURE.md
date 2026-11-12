# DeckHand architecture

No scripting languages in the product. macOS first, then Windows and Linux; iOS in tandem. MIT.

## Languages
- **Shared C++20 core** (`src/`) on every platform: engine, library, import, controllers, drawing, app
  logic. Anything that must behave identically everywhere exists exactly once.
- **Thin platform shells**: Swift on macOS (`platform/macos`) and iOS (`platform/ios`); C++ on Windows
  and Linux (later). A shell only opens a window, renders the core's draw list, forwards input and
  rasterises text. It talks to the core through one C header, `src/app/include/deckhand/app/capi.h`.
- `platform/apple`: C++ glue over C system APIs shared by macOS and iOS (audio file decoding, MIDI).
- Build: CMake for the core; `scripts/build-macos.sh` builds, bundles and signs the Mac app.
- The core stays platform-neutral: no platform headers outside `platform/`.

## Modules (`src/`, a module may depend only on modules below it)
    app                         UI, track loading, import, controller routing, C interface
    midi  import  gfx           controller mappings; source-library importer; draw list + text atlas
    audio  library              real-time engine; SQLite library
    log  core                   logging; shared types and the lock-free queue
    (api, broadcast, export, analysis, skin, sync, i18n: planned, see ROADMAP.md)

## Audio engine (`src/audio`)
- The callback takes no locks, allocates nothing, does no I/O and never logs. Commands arrive on a
  wait-free queue; state is published as snapshots; retired tracks are freed on another thread.
- Four decks, per-deck trim / three-band EQ / filter / fader, crossfader, master, and a separate
  headphone (cue) bus on a second stereo pair when the device has one.
- Tracks are decoded fully into memory. Tempo changes pitch (no key lock yet).

## Engine process (`src/audio/ipc*`, `remote_engine.*`, `src/enginehost`)
The headline promise: music keeps playing if the UI crashes or hangs, and a restarted UI re-attaches to the set.
- `deckhand-engine` (beside the app executable) owns the output device, the real `Engine`, the recorder, the stream
  tap and the controller hub (MIDI I/O and mapping files). The app talks to it through `IEngine`, implemented by the
  in-process `Engine` and by `RemoteEngine`. Setting `engine.process` = `separate` (default on desktop macOS) |
  `inprocess` (iOS, platforms without the IPC layer, or when the helper cannot start; `DECKHAND_ENGINE_PROCESS`
  overrides). Offline `render()` is in-process only.
- Per data folder, one engine (instance lock in `/tmp/dh-<uid>/`, names hashed from the folder so portable copies
  do not collide and macOS name limits hold). The app attaches to a running engine or starts one.
- Main shared-memory segment, no pointers: multi-producer command ring UI to engine; seqlock snapshots (decks, mixer,
  sampler, recorder, device, controllers, per-deck track id and path); event ring and stream-tap ring engine to UI.
- Track audio: the UI's loader decodes and analyses, then copies PCM into one shared-memory block per track. The
  engine maps it read-only, validates every size, offset, count and number (the UI is untrusted input), unlinks the
  name at once and keeps the mapping until the track is swapped out. Stale names are collected when an engine or UI
  starts. Decoder crashes therefore cannot touch the audio.
- Control socket (Unix domain) for non-real-time requests: output device, recording, sampler loads, scheduling,
  controller learn, attach/quit. Platform code is isolated in `ipc_posix.cpp`; a Windows port adds the same classes.
- If the UI dies the engine plays on (a UI heartbeat only decides whether browse requests are forwarded to the UI).
  A normal quit (window close, SIGTERM) fades out and stops the engine; `engine.keep_alive_on_quit` keeps it while a
  deck plays. If the engine dies the app restarts it with back-off (decks come back empty) and gives up after repeated
  quick failures. SIGTERM finalises an open recording.
- Known limits: a UI that truncates a shared block after handing it over can still fault the engine (shm cannot be
  sealed on macOS); sampler names and the recorder's UI-side status are not part of the re-attach state.

## Controllers (`src/midi`, `controllers/*.dhmap`)
- General first: a controller is a declarative mapping file (no code). Any connected MIDI device
  whose name matches a mapping is driven by it; several can be active at once. See docs/CONTROLLERS.md.
- A mapping can also name the controller's audio interface and where its master and headphone
  pairs start; the engine switches to it when it appears and back when it goes.

## UI (`src/gfx`, `src/app`)
- The core draws everything into a list of textured triangles plus one text atlas; shells render it
  with a single pipeline. Text is rasterised by the platform so all scripts work.
- **Current shortcut:** the default layout is drawn in code. The skin engine (docs/SKINS.md) will
  replace it, with this layout becoming the built-in skin.

## Library (`src/library`, `src/import`)
- DeckHand owns its library (SQLite, schema in `src/library/schema.sql`). Other DJ libraries are
  read-only sources: the importer copies the source database to a scratch folder and reads the copy.
  Re-importing updates in place; cues and grids edited in DeckHand are kept.
- Track paths are stored as root + relative path so libraries can move (portable mode).

## Portable mode
- A file named `portable` beside the app keeps all data in `DeckHandData/` next to it; otherwise
  the per-user data folder is used.

## Planned (see docs/)
- docs/API.md (REST + WebSocket, ACL scopes, TLS, bearer tokens), docs/SKINS.md, docs/I18N.md,
  docs/LOGGING.md; recording, history log, OBS integration, CDJ export, cloud sync.

## Rules
- Clean-room: no third-party branding, assets or copied material; no decompiling. Device support
  is written from manufacturers' published documentation.
- Check every dependency's licence before adding it (MIT project; no GPL).
- Source libraries are never modified; exports are rebuilt from scratch.
