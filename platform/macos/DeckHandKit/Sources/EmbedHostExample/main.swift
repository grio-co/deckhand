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
  }
  func applicationWillTerminate(_ n: Notification) { session?.shutdown() }
}
let app = NSApplication.shared
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
