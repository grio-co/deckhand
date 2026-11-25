// Minimal host: DeckHand inline in a SwiftUI window with a Pop out / Dock button.
// Run: scripts/build-embed.sh && (cd platform/macos/DeckHandKit && swift run EmbedHostExample)
import AppKit
import DeckHandKit
import SwiftUI

struct Root: View {
  let session: DeckHandSession
  @State private var popped = false
  var body: some View {
    VStack(spacing: 0) {
      HStack {
        Text("Host app").font(.headline)
        Spacer()
        Button(popped ? "Dock" : "Pop out") {
          if popped { session.dock() } else { session.popOut() }
          popped = session.isPoppedOut
        }
      }.padding(8)
      if popped { Text("DeckHand is in its own window").frame(maxWidth: .infinity, maxHeight: .infinity) }
      else { DeckHandEmbed(session: session) }
    }
  }
}

final class App: NSObject, NSApplicationDelegate {
  var window: NSWindow!
  var session: DeckHandSession!
  func applicationDidFinishLaunching(_ n: Notification) {
    let data = ProcessInfo.processInfo.environment["DECKHAND_DATA"]
    let helper = ProcessInfo.processInfo.environment["DECKHAND_ENGINE_HELPER"]
    let res = ProcessInfo.processInfo.environment["DECKHAND_RESOURCES"]
    guard let s = DeckHandSession(configuration: .init(dataDir: data, resourceDir: res, engineHelper: helper)) else {
      print("DeckHand could not start"); NSApp.terminate(nil); return
    }
    session = s
    let root = Root(session: s)
    let host = NSHostingView(rootView: root)
    window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1300, height: 860), styleMask: [.titled, .closable, .resizable],
                      backing: .buffered, defer: false)
    window.title = "Embed host"
    window.contentView = host
    window.center(); window.makeKeyAndOrderFront(nil)
    NSApp.activate(ignoringOtherApps: true)
    print("started")
    // Self-check without touching the desktop: DECKHAND_SNAPSHOT=<dir> saves the host window, pops out, saves, docks, saves.
    if let dir = ProcessInfo.processInfo.environment["DECKHAND_SNAPSHOT"] {
      func shot(_ w: NSWindow?, _ name: String) {
        guard let w else { print("no window for \(name)"); return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
        p.arguments = ["-x", "-o", "-l", String(w.windowNumber), dir + "/" + name + ".png"]  // this window only
        try? p.run(); p.waitUntilExit()
        print("saved \(name) status \(p.terminationStatus)")
      }
      let popWindow = { NSApp.windows.first { $0 !== self.window && $0.contentView === s.view } }
      DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
        shot(self.window, "1-inline")
        s.popOut()
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
          shot(popWindow(), "2-popped")
          shot(self.window, "2-host-while-popped")
          s.dock()
          DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
            shot(self.window, "3-docked")
            NSApp.terminate(nil)
          }
        }
      }
    }
  }
  func applicationWillTerminate(_ n: Notification) { session?.shutdown() }
}
let app = NSApplication.shared
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
