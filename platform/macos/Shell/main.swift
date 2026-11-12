// DeckHand macOS shell: window, menu and lifecycle. All behaviour lives in the C++ core.
import AppKit
import MetalKit

final class AppDelegate: NSObject, NSApplicationDelegate {
  private var window: NSWindow!
  private var view: DeckHandView?

  func applicationDidFinishLaunching(_ notification: Notification) {
    buildMenu()
    let rect = NSRect(x: 0, y: 0, width: 1440, height: 900)
    window = NSWindow(contentRect: rect,
                      styleMask: [.titled, .closable, .miniaturizable, .resizable],
                      backing: .buffered, defer: false)
    window.title = "DeckHand"
    window.minSize = NSSize(width: 1100, height: 700)
    window.appearance = NSAppearance(named: .darkAqua)
    window.acceptsMouseMovedEvents = true

    guard let device = MTLCreateSystemDefaultDevice() else {
      fail("DeckHand needs a Metal-capable graphics device.")
      return
    }
    let view = DeckHandView(frame: rect, device: device)
    guard view.start() else {
      fail("DeckHand could not start. See deckhand.log in its data folder.")
      return
    }
    self.view = view
    window.contentView = view
    window.center()
    window.makeKeyAndOrderFront(nil)
    window.makeFirstResponder(view)
    NSApp.activate(ignoringOtherApps: true)

    // Optional: DeckHand --deck-a <file> --deck-b <file> loads tracks at launch.
    let args = CommandLine.arguments
    for (flag, x) in [("--deck-a", 10.0), ("--deck-b", Double(rect.width) - 10.0)] {
      if let i = args.firstIndex(of: flag), i + 1 < args.count {
        view.openFile(args[i + 1], at: CGPoint(x: x, y: 100))
      }
    }
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

  func applicationWillTerminate(_ notification: Notification) { view?.shutdown() }

  func application(_ sender: NSApplication, openFiles filenames: [String]) {
    for file in filenames { view?.openFile(file, at: nil) }
    sender.reply(toOpenOrPrint: .success)
  }

  private func fail(_ message: String) {
    let alert = NSAlert()
    alert.messageText = message
    alert.runModal()
    NSApp.terminate(nil)
  }

  private func buildMenu() {
    let main = NSMenu()
    let appItem = NSMenuItem()
    main.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.addItem(withTitle: "About DeckHand",
                    action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
    appMenu.addItem(.separator())
    appMenu.addItem(withTitle: "Hide DeckHand", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
    appMenu.addItem(withTitle: "Quit DeckHand", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu

    let windowItem = NSMenuItem()
    main.addItem(windowItem)
    let windowMenu = NSMenu(title: "Window")
    windowMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
    windowMenu.addItem(withTitle: "Enter Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f")
    windowItem.submenu = windowMenu
    NSApp.mainMenu = main
    NSApp.windowsMenu = windowMenu
  }
}

let application = NSApplication.shared
let delegate = AppDelegate()
application.delegate = delegate
application.setActivationPolicy(.regular)
application.run()
