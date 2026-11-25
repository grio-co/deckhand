# Embedding DeckHand in another macOS app

`platform/macos/DeckHandKit` is a Swift package with the same view the standalone app uses. A host (for example Rig)
shows DeckHand inline and can pop it out into its own window without restarting anything.

```sh
scripts/build-embed.sh      # builds the core, merges it into build/embed/libdeckhand_core.a, writes Package.swift, builds the package
```

Add `platform/macos/DeckHandKit` as a local package dependency (its `Package.swift` is generated and holds this machine's
build path, so it is not committed). Then:

```swift
import DeckHandKit

let session = DeckHandSession(configuration: .init(
    dataDir: "<host data folder>/DeckHand",        // library, settings, logs
    resourceDir: "<host>/Resources/DeckHand",       // lang/, skins/, controllers/*.dhmap
    engineHelper: "<host>.app/Contents/MacOS/deckhand-engine"))   // nil: beside the host executable
// SwiftUI:  DeckHandEmbed(session: session)       // inline panel
// session.popOut() / session.dock()                // one view moves between panel and window; audio never stops
// session.shutdown()                               // at quit
```

- **One core, one view.** Pop-out moves the same view to a window; closing the window docks it back (`onDock`).
- **Engine process.** The audio engine is the separate `deckhand-engine` helper (built to `build/deckhand-engine`).
  The host ships it, signs it with its own identity (hardened runtime, no entitlements) and passes its path.
  The helper exits when the host does.
- **API port.** The core still starts DeckHand's REST API on 127.0.0.1:7373. If the standalone DeckHand is already
  running, the embedded one logs "Address already in use" and runs without an API; both cannot own the port.
- **Example host:** `swift run EmbedHostExample` in the package folder (env `DECKHAND_DATA`, `DECKHAND_RESOURCES`,
  `DECKHAND_ENGINE_HELPER`).

Verified: package builds; standalone app still builds and signs from the same sources; the example host starts the core
with a host-chosen data folder and launches the helper from the given path; the helper exits with the host.
Not verified: the inline view and pop-out visually (the window did not come to the front in my run), input and audio
inside a real host, signing/notarising a host that bundles the helper.
