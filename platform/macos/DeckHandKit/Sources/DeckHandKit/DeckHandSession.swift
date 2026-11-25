// One running DeckHand (core + view) that a host app can show inline and pop out into its own window.
// There is a single view: popping out moves it between containers, so nothing restarts and the music keeps playing.
import AppKit
import SwiftUI

public final class DeckHandSession: NSObject, NSWindowDelegate {
  public let view: DeckHandView
  public private(set) var isPoppedOut = false
  /// Called when the pop-out window is closed by the user, after the view has been handed back.
  public var onDock: (() -> Void)?

  private weak var inlineContainer: NSView?
  private var window: NSWindow?

  /// Returns nil if Metal or the core cannot start.
  public init?(configuration: DeckHandView.Configuration = .init(), frame: CGRect = CGRect(x: 0, y: 0, width: 1200, height: 760)) {
    guard let device = MTLCreateSystemDefaultDevice() else { return nil }
    let view = DeckHandView(frame: frame, device: device)
    guard view.start(configuration) else { return nil }
    self.view = view
    super.init()
  }

  /// Show the view inside `container` (an inline panel). If it is popped out, it stays in its window until docked.
  public func attach(to container: NSView) {
    inlineContainer = container
    guard !isPoppedOut else { return }
    place(view, in: container)
  }

  public func popOut(title: String = "DeckHand") {
    guard !isPoppedOut else { window?.makeKeyAndOrderFront(nil); return }
    let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1440, height: 900),
                     styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
    w.title = title
    w.minSize = NSSize(width: 1100, height: 700)
    w.appearance = NSAppearance(named: .darkAqua)
    w.acceptsMouseMovedEvents = true
    w.isReleasedWhenClosed = false
    w.delegate = self
    view.removeFromSuperview()
    w.contentView = view
    w.center()
    w.makeKeyAndOrderFront(nil)
    w.makeFirstResponder(view)
    window = w
    isPoppedOut = true
  }

  public func dock() {
    guard isPoppedOut else { return }
    isPoppedOut = false
    let w = window
    window = nil
    w?.delegate = nil
    w?.contentView = NSView()
    w?.close()
    if let inlineContainer { place(view, in: inlineContainer) }
    onDock?()
  }

  public func windowWillClose(_ notification: Notification) {
    guard isPoppedOut else { return }
    dock()
  }

  /// Stop the core (the engine process keeps its own lifetime rules). Call when the host quits.
  public func shutdown() {
    window?.delegate = nil
    window?.close()
    view.shutdown()
  }

  private func place(_ v: NSView, in container: NSView) {
    v.removeFromSuperview()
    v.frame = container.bounds
    v.autoresizingMask = [.width, .height]
    container.addSubview(v)
    container.window?.makeFirstResponder(v)
  }
}

/// SwiftUI host for the inline panel. Shows a placeholder while the view is popped out.
public struct DeckHandEmbed: NSViewRepresentable {
  public let session: DeckHandSession
  public init(session: DeckHandSession) { self.session = session }
  public func makeNSView(context: Context) -> NSView {
    let container = NSView()
    session.attach(to: container)
    return container
  }
  public func updateNSView(_ nsView: NSView, context: Context) { session.attach(to: nsView) }
}
