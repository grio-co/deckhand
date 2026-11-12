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
- **Current shortcut:** the engine runs inside the app process behind the same command/snapshot
  boundary it will use as a separate process. Splitting it out (so a UI crash cannot stop audio)
  is planned work, not done.

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
