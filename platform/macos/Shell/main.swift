// DeckHand macOS shell: native windows, menu and lifecycle. All behaviour lives in the C++ core.
//
// The core asks for a set of windows (dh_app_windows): the main window and, depending on the skin, detached modules
// (browser, sampler ...) and panels. Each is a borderless or titled NSWindow whose content view is layer-backed by a
// CAMetalLayer, non-opaque for transparent skins. One Renderer (one texture namespace) draws every window; the core
// answers "what is under this pixel" (dh_app_hit_test) so clicks on transparent parts fall through, and drives window
// dragging itself so snapping and group moves work (dh_app_window_drag). See docs/SHELL-WINDOWS.md.
import AppKit
import MetalKit
import QuartzCore
import CDeckHand

// MARK: - coordinates

/// Desktop points: origin top-left of the primary display, y down (what the core uses). Cocoa is bottom-left.
private var primaryHeight: CGFloat { NSScreen.screens.first?.frame.height ?? 0 }
private func desktopPoint(_ p: NSPoint) -> (x: Float, y: Float) { (Float(p.x), Float(primaryHeight - p.y)) }

private func string(_ tuple: UnsafeRawPointer) -> String { String(cString: tuple.assumingMemoryBound(to: CChar.self)) }
private func infoName(_ i: dh_window_info) -> String { var c = i; return withUnsafePointer(to: &c.name) { string(UnsafeRawPointer($0)) } }
private func infoRole(_ i: dh_window_info) -> String { var c = i; return withUnsafePointer(to: &c.role) { string(UnsafeRawPointer($0)) } }
private func infoTitle(_ i: dh_window_info) -> String { var c = i; return withUnsafePointer(to: &c.title) { string(UnsafeRawPointer($0)) } }

private func has(_ flags: UInt32, _ bit: Int) -> Bool { flags & UInt32(bit) != 0 }

// MARK: - window and view

final class ShellWindow: NSWindow {
  var canBeKey = true
  var canBeMain = false
  override var canBecomeKey: Bool { canBeKey }
  override var canBecomeMain: Bool { canBeMain }
}

final class ShellView: NSView {
  let windowID: UInt32
  unowned let shell: Shell
  let metal: CAMetalLayer
  private var dragging = false

  init(id: UInt32, shell: Shell, device: MTLDevice) {
    self.windowID = id
    self.shell = shell
    metal = CAMetalLayer()
    metal.device = device
    metal.pixelFormat = .bgra8Unorm
    metal.framebufferOnly = true
    metal.isOpaque = false            // the desktop shows through where the core drew nothing
    metal.backgroundColor = nil
    metal.presentsWithTransaction = false
    super.init(frame: .zero)
    layer = metal
    wantsLayer = true
    registerForDraggedTypes([.fileURL])
    addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self, userInfo: nil))
  }
  required init?(coder: NSCoder) { fatalError("not used") }

  override var isFlipped: Bool { true }
  override var acceptsFirstResponder: Bool { true }
  override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }  // the first click on an inactive app is a real click
  override var mouseDownCanMoveWindow: Bool { false }                      // the core moves windows (snapping, groups)

  var backing: CGFloat { window?.backingScaleFactor ?? 1 }

  /// Pixel size of the drawable, kept in step with the view and the screen it is on.
  func syncDrawable() {
    let b = backing
    metal.contentsScale = b
    let size = CGSize(width: max(1, (bounds.width * b).rounded()), height: max(1, (bounds.height * b).rounded()))
    if metal.drawableSize != size { metal.drawableSize = size }
  }
  override func viewDidChangeBackingProperties() { syncDrawable() }
  override func setFrameSize(_ newSize: NSSize) { super.setFrameSize(newSize); syncDrawable() }

  // pointer ----------------------------------------------------------------------------------------------
  private func pixels(_ event: NSEvent) -> (Float, Float) {
    let p = convert(event.locationInWindow, from: nil)
    return (Float(p.x * backing), Float(p.y * backing))
  }
  private func sendModifiers(_ event: NSEvent) { dh_app_modifiers(shell.core, shell.modifiers(event)) }

  override func mouseDown(with event: NSEvent) {
    sendModifiers(event)
    let (x, y) = pixels(event)
    let hit = dh_app_hit_test(shell.core, windowID, x, y)
    if hit == DH_HIT_DRAG || hit == DH_HIT_RESIZE {
      dragging = true
      shell.dragKind = hit == DH_HIT_RESIZE ? DH_DRAG_RESIZE : DH_DRAG_MOVE
      let g = desktopPoint(NSEvent.mouseLocation)
      dh_app_window_drag(shell.core, windowID, shell.dragKind, DH_DRAG_BEGIN, g.x, g.y, shell.modifiers(event))
    } else if hit != DH_HIT_SOLID {
      dh_app_window_pointer(shell.core, windowID, DH_PTR_DOWN, 0, x, y, Int32(event.clickCount))
    }
  }
  override func mouseDragged(with event: NSEvent) {
    sendModifiers(event)
    if dragging {
      let g = desktopPoint(NSEvent.mouseLocation)
      dh_app_window_drag(shell.core, windowID, shell.dragKind, DH_DRAG_UPDATE, g.x, g.y, shell.modifiers(event))
      shell.applyWindowFrames()  // follow the pointer without waiting for the next display tick
    } else {
      let (x, y) = pixels(event)
      dh_app_window_pointer(shell.core, windowID, DH_PTR_MOVE, 0, x, y, 0)
    }
  }
  override func mouseUp(with event: NSEvent) {
    sendModifiers(event)
    if dragging {
      dragging = false
      let g = desktopPoint(NSEvent.mouseLocation)
      dh_app_window_drag(shell.core, windowID, shell.dragKind, DH_DRAG_END, g.x, g.y, shell.modifiers(event))
      shell.applyWindowFrames()
    } else {
      let (x, y) = pixels(event)
      dh_app_window_pointer(shell.core, windowID, DH_PTR_UP, 0, x, y, 0)
    }
    shell.refreshClickThrough()
  }
  override func mouseMoved(with event: NSEvent) {
    sendModifiers(event)
    let (x, y) = pixels(event)
    dh_app_window_pointer(shell.core, windowID, DH_PTR_MOVE, 0, x, y, 0)
    shell.refreshClickThrough()
  }
  override func magnify(with event: NSEvent) {
    let (x, y) = pixels(event)
    dh_app_window_zoom(shell.core, windowID, x, y, Float(event.magnification))
  }
  override func scrollWheel(with event: NSEvent) {
    let (x, y) = pixels(event)
    let unit: CGFloat = event.hasPreciseScrollingDeltas ? 1 : 12
    let b = Float(backing)
    dh_app_window_scroll(shell.core, windowID, x, y, Float(event.scrollingDeltaX * unit) * b, Float(event.scrollingDeltaY * unit) * b)
  }

  // keyboard ---------------------------------------------------------------------------------------------
  override func flagsChanged(with event: NSEvent) { sendModifiers(event) }
  /// macOS virtual key codes -> USB HID keyboard usages (the physical key the core's shortcuts are bound to).
  private static let hidFromMac: [UInt16: UInt32] = [
    0: 4, 11: 5, 8: 6, 2: 7, 14: 8, 3: 9, 5: 10, 4: 11, 34: 12, 38: 13, 40: 14, 37: 15, 46: 16, 45: 17, 31: 18, 35: 19,
    12: 20, 15: 21, 1: 22, 17: 23, 32: 24, 9: 25, 13: 26, 7: 27, 16: 28, 6: 29,
    18: 30, 19: 31, 20: 32, 21: 33, 23: 34, 22: 35, 26: 36, 28: 37, 25: 38, 29: 39,
    36: 40, 53: 41, 51: 42, 48: 43, 49: 44, 27: 45, 24: 46, 33: 47, 30: 48, 42: 49, 41: 51, 39: 52, 50: 53, 43: 54, 47: 55, 44: 56,
    122: 58, 120: 59, 99: 60, 118: 61, 96: 62, 97: 63, 98: 64, 100: 65, 101: 66, 109: 67, 103: 68, 111: 69,
    105: 104, 107: 105, 113: 106,
    114: 73, 115: 74, 116: 75, 117: 76, 119: 77, 121: 78, 124: 79, 123: 80, 125: 81, 126: 82,
    71: 83, 75: 84, 67: 85, 78: 86, 69: 87, 76: 88, 83: 89, 84: 90, 85: 91, 86: 92, 87: 93, 88: 94, 89: 95, 91: 96, 92: 97, 82: 98, 65: 99,
  ]

  /// The shortcuts see every key (including releases, for held cues). A key a shortcut takes is not typed or navigated.
  private func sendKey(_ event: NSEvent, down: Bool) -> Bool {
    let mods = shell.modifiers(event)
    dh_app_modifiers(shell.core, mods)
    let character = event.charactersIgnoringModifiers?.unicodeScalars.first?.value ?? 0
    return dh_app_key_event(shell.core, Self.hidFromMac[event.keyCode] ?? 0, character < 0xF700 ? character : 0, mods, down, down && event.isARepeat)
  }
  override func keyUp(with event: NSEvent) {
    if !sendKey(event, down: false) { super.keyUp(with: event) }
  }
  override func keyDown(with event: NSEvent) {
    if sendKey(event, down: true) { return }
    let mods = shell.modifiers(event)
    let keys: [UInt16: dh_key] = [126: DH_KEY_UP, 125: DH_KEY_DOWN, 123: DH_KEY_LEFT, 124: DH_KEY_RIGHT,
                                  36: DH_KEY_ENTER, 76: DH_KEY_ENTER, 53: DH_KEY_ESCAPE, 51: DH_KEY_BACKSPACE, 48: DH_KEY_TAB]
    dh_app_modifiers(shell.core, mods)
    if let key = keys[event.keyCode] {
      dh_app_key(shell.core, key, mods)
    } else if !event.modifierFlags.contains(.command), !event.modifierFlags.contains(.control),
              let text = event.characters, !text.isEmpty {
      text.withCString { dh_app_text(shell.core, $0) }
    } else {
      super.keyDown(with: event)
    }
  }

  // files ----------------------------------------------------------------------------------------------------
  override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { .copy }
  override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
    let p = convert(sender.draggingLocation, from: nil)
    let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
    for url in urls { url.path.withCString { dh_app_window_open_file(shell.core, windowID, $0, Float(p.x * backing), Float(p.y * backing)) } }
    return !urls.isEmpty
  }
}

/// One native window the core asked for.
final class ShellEntry: NSObject, NSWindowDelegate {
  let id: UInt32
  let window: ShellWindow
  let view: ShellView
  unowned let shell: Shell
  var info: dh_window_info
  var lastSeq: UInt32 = 0
  var shown = false
  var lastPx = CGSize.zero
  var lastBacking: CGFloat = 0
  var shadowTick = 0
  var applying = false
  var gotFrameOnce = false

  init(info: dh_window_info, shell: Shell, device: MTLDevice) {
    self.id = info.id
    self.info = info
    self.shell = shell
    let flags = info.flags
    var style: NSWindow.StyleMask = []
    if has(flags, DH_WIN_BORDERLESS) || has(flags, DH_WIN_POPUP) {
      style = [.borderless, .miniaturizable]
    } else {
      style = [.titled, .closable, .miniaturizable]
      if has(flags, DH_WIN_RESIZABLE) { style.insert(.resizable) }
    }
    window = ShellWindow(contentRect: NSRect(x: 0, y: 0, width: CGFloat(info.width), height: CGFloat(info.height)),
                         styleMask: style, backing: .buffered, defer: false)
    view = ShellView(id: info.id, shell: shell, device: device)
    super.init()
    window.contentView = view
    window.delegate = self
    window.isReleasedWhenClosed = false
    window.acceptsMouseMovedEvents = true
    window.appearance = NSAppearance(named: .darkAqua)
    window.canBeMain = has(flags, DH_WIN_MAIN)
    window.hidesOnDeactivate = false
    configure(info)
  }

  /// Properties that follow the skin (a skin switch can change them under a living window).
  func configure(_ i: dh_window_info) {
    info = i
    let flags = i.flags
    let transparent = has(flags, DH_WIN_TRANSPARENT)
    window.isOpaque = !transparent
    window.backgroundColor = transparent ? .clear : NSColor(calibratedWhite: 0.09, alpha: 1)
    window.hasShadow = !has(flags, DH_WIN_NO_SHADOW)
    window.title = infoTitle(i)
    window.level = has(flags, DH_WIN_FLOATING) ? .floating : (has(flags, DH_WIN_POPUP) ? .popUpMenu : .normal)
    // Spaces and full-screen apps: modules ride along with the main window; shaped windows never go full screen.
    var behavior: NSWindow.CollectionBehavior = [.managed]
    behavior.insert(has(flags, DH_WIN_MAIN) ? .fullScreenNone : .fullScreenAuxiliary)
    if has(flags, DH_WIN_FLOATING) { behavior.insert(.fullScreenAuxiliary) }
    window.collectionBehavior = behavior
    window.isExcludedFromWindowsMenu = has(flags, DH_WIN_POPUP)
    if i.min_w > 0 || i.min_h > 0 { window.contentMinSize = NSSize(width: CGFloat(i.min_w), height: CGFloat(i.min_h)) }
    if i.max_w > 0 && i.max_h > 0 { window.contentMaxSize = NSSize(width: CGFloat(i.max_w), height: CGFloat(i.max_h)) }
    else { window.contentMaxSize = NSSize(width: 100000, height: 100000) }
    if has(flags, DH_WIN_ASPECT_LOCK), i.height > 0 { window.contentAspectRatio = NSSize(width: CGFloat(i.width), height: CGFloat(i.height)) }
    else { window.resizeIncrements = NSSize(width: 1, height: 1) }
  }

  /// Applies the position and size the core wants, keeping the window on a screen that exists.
  func applyFrame(_ i: dh_window_info) {
    applying = true
    defer { applying = false }
    let size = NSSize(width: CGFloat(i.width), height: CGFloat(i.height))
    var content = NSRect(origin: .zero, size: size)
    if has(i.flags, DH_WIN_POS_VALID) {
      content.origin = NSPoint(x: CGFloat(i.x), y: primaryHeight - CGFloat(i.y) - size.height)
    } else if let screen = NSScreen.main {
      let vf = screen.visibleFrame
      content.origin = NSPoint(x: vf.midX - size.width / 2, y: vf.midY - size.height / 2)
    }
    var frame = window.frameRect(forContentRect: content)
    frame = Shell.onScreen(frame)
    window.setFrame(frame, display: false)
    report()
  }

  /// Tells the core where the window really is (after clamping, or after the user moved it).
  func report() {
    let c = window.contentRect(forFrameRect: window.frame)
    dh_app_window_event(shell.core, id, DH_WE_MOVED, Float(c.origin.x), Float(primaryHeight - c.origin.y - c.height))
    dh_app_window_event(shell.core, id, DH_WE_RESIZED, Float(c.width), Float(c.height))
  }

  func windowDidMove(_ notification: Notification) { if !applying { report() } }
  func windowDidResize(_ notification: Notification) {
    view.syncDrawable()
    if !applying { report() }
    shadowTick = 0
  }
  func windowDidChangeScreen(_ notification: Notification) { view.syncDrawable() }
  func windowDidChangeBackingProperties(_ notification: Notification) { view.syncDrawable() }
  func windowDidBecomeKey(_ notification: Notification) { dh_app_window_event(shell.core, id, DH_WE_FOCUS, 1, 0) }
  func windowDidResignKey(_ notification: Notification) {
    dh_app_key_release_all(shell.core)  // a key still held (cue) must not stay down
    dh_app_window_event(shell.core, id, DH_WE_FOCUS, 0, 0)
  }
  func windowDidMiniaturize(_ notification: Notification) { dh_app_window_event(shell.core, id, DH_WE_MINIMIZED, 1, 0) }
  func windowDidDeminiaturize(_ notification: Notification) { dh_app_window_event(shell.core, id, DH_WE_MINIMIZED, 0, 0) }
  func windowShouldClose(_ sender: NSWindow) -> Bool {
    if has(info.flags, DH_WIN_MAIN) {
      NSApp.terminate(nil)
    } else {
      dh_app_window_event(shell.core, id, DH_WE_CLOSE, 0, 0)  // the core hides the module / dismisses the panel
    }
    return false
  }
}

// MARK: - the shell

final class Shell: NSObject {
  var core: OpaquePointer!
  let device: MTLDevice
  let renderer: Renderer
  private var entries: [UInt32: ShellEntry] = [:]
  private var order: [UInt32] = []
  private var infos: [dh_window_info] = []
  private var link: CADisplayLink?
  private var lastTick = CACurrentMediaTime()
  private var inTick = false
  private var monitors: [Any] = []
  var dragKind = DH_DRAG_MOVE
  private var pendingDecks: [(String, Float)] = []
  private var fpsFrames = 0
  private var fpsSince = CACurrentMediaTime()
  private let fpsLog = ProcessInfo.processInfo.environment["DECKHAND_FPS"] != nil
  private var mainID: UInt32 = 0

  init?(device: MTLDevice) {
    guard let renderer = Renderer(device: device, pixelFormat: .bgra8Unorm) else {
      NSLog("DeckHand: renderer could not be created")
      return nil
    }
    self.device = device
    self.renderer = renderer
    super.init()

    // Portable mode: a file named "portable" beside the app keeps all data next to it.
    let fm = FileManager.default
    let beside = Bundle.main.bundleURL.deletingLastPathComponent()
    let dataDir: String
    if fm.fileExists(atPath: beside.appendingPathComponent("portable").path) {
      dataDir = beside.appendingPathComponent("DeckHandData").path
    } else {
      let support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first ?? URL(fileURLWithPath: NSHomeDirectory())
      dataDir = support.appendingPathComponent("DeckHand").path
    }
    let levels = ["error": 0, "warn": 1, "info": 2, "debug": 3, "trace": 4]
    let level = levels[ProcessInfo.processInfo.environment["DECKHAND_LOG"] ?? "info"] ?? 2
    let resources = Bundle.main.resourcePath ?? ""
    var platform = dh_platform(user: nil, rasterise_text: rasteriseText, show_bluetooth_midi: Platform.showBluetooth,
                               choose_folder: Platform.chooseFolder, list_export_targets: nil)
    core = dataDir.withCString { data in
      resources.withCString { res in
        var config = dh_config(data_dir: data, resource_dir: res, log_level: Int32(level), touch: false, manage_audio: true)
        return dh_app_create(&platform, &config)
      }
    }
    if core == nil { return nil }
    dh_app_set_capabilities(core, UInt32(DH_CAP_MULTI_WINDOW | DH_CAP_TRANSPARENT | DH_CAP_CLICK_THROUGH))
  }

  func modifiers(_ event: NSEvent) -> UInt32 {
    var mods: UInt32 = 0
    if event.modifierFlags.contains(.shift) { mods |= UInt32(DH_MOD_SHIFT) }
    if event.modifierFlags.contains(.command) { mods |= UInt32(DH_MOD_COMMAND) }
    if event.modifierFlags.contains(.option) { mods |= UInt32(DH_MOD_ALT) }
    if event.modifierFlags.contains(.control) { mods |= UInt32(DH_MOD_CONTROL) }
    return mods
  }

  /// A screen position for a window frame: if it is not (mostly) on any screen, move it onto the nearest one.
  static func onScreen(_ frame: NSRect) -> NSRect {
    let screens = NSScreen.screens.map { $0.visibleFrame }
    guard !screens.isEmpty else { return frame }
    let grip = NSRect(x: frame.minX, y: frame.maxY - 40, width: min(frame.width, 120), height: 40)  // the top-left part
    if screens.contains(where: { $0.intersects(grip) && $0.intersection(grip).width >= min(80, grip.width) }) { return frame }
    let target = NSScreen.main?.visibleFrame ?? screens[0]
    var f = frame
    f.origin.x = min(max(f.origin.x, target.minX), max(target.minX, target.maxX - f.width))
    f.origin.y = min(max(f.origin.y, target.minY), max(target.minY, target.maxY - f.height))
    return f
  }

  // lifecycle ------------------------------------------------------------------------------------------------
  func start(decks: [(String, Float)]) {
    pendingDecks = decks
    let screen = NSScreen.main ?? NSScreen.screens[0]
    link = screen.displayLink(target: self, selector: #selector(tick(_:)))
    link?.add(to: .main, forMode: .common)
    let moved: (NSEvent) -> Void = { [weak self] _ in self?.refreshClickThrough() }
    if let m = NSEvent.addGlobalMonitorForEvents(matching: [.mouseMoved, .leftMouseDragged, .leftMouseUp], handler: moved) { monitors.append(m) }
    if let m = NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseUp], handler: { [weak self] e in self?.refreshClickThrough(); return e }) { monitors.append(m) }
    NotificationCenter.default.addObserver(self, selector: #selector(screensChanged), name: NSApplication.didChangeScreenParametersNotification, object: nil)
  }

  func shutdown() {
    link?.invalidate()
    link = nil
    for m in monitors { NSEvent.removeMonitor(m) }
    monitors = []
    if let core { dh_app_destroy(core) }
    core = nil
  }

  @objc private func screensChanged() {
    for e in entries.values where e.shown {  // a monitor went away: bring windows back
      let f = Shell.onScreen(e.window.frame)
      if f != e.window.frame { e.applying = true; e.window.setFrame(f, display: true); e.applying = false; e.report() }
    }
  }

  // click-through ----------------------------------------------------------------------------------------------
  /// Windows with transparent parts ignore the mouse exactly where the core says nothing is there, so the click goes to
  /// whatever is behind. Called on every mouse move, also while the app is inactive (global monitor).
  func refreshClickThrough() {
    guard let core else { return }
    if NSEvent.pressedMouseButtons != 0 { return }  // never flip while a button is down
    let loc = NSEvent.mouseLocation
    for e in entries.values where e.shown && has(e.info.flags, DH_WIN_SHAPED) {
      let w = e.window
      guard w.frame.contains(loc) else { continue }
      let c = w.contentRect(forFrameRect: w.frame)
      let b = e.view.backing
      let x = Float((loc.x - c.minX) * b), y = Float((c.maxY - loc.y) * b)
      let hit = dh_app_hit_test(core, e.id, x, y)
      w.ignoresMouseEvents = hit == DH_HIT_NONE
    }
  }

  // frames ------------------------------------------------------------------------------------------------------
  /// Reads the window list and brings the native windows in line (create, destroy, show, hide, move, resize).
  func applyWindowFrames() {
    guard let core else { return }
    var buffer = [dh_window_info](repeating: dh_window_info(), count: 32)
    let n = Int(min(32, dh_app_windows(core, &buffer, 32)))
    infos = Array(buffer.prefix(n))
    var seen = Set<UInt32>()
    for i in infos {
      seen.insert(i.id)
      let visible = has(i.flags, DH_WIN_VISIBLE)
      var entry = entries[i.id]
      if entry == nil {
        guard visible else { continue }
        entry = ShellEntry(info: i, shell: self, device: device)
        entries[i.id] = entry
        order.append(i.id)
      }
      guard let e = entry else { continue }
      e.configure(i)
      if has(i.flags, DH_WIN_MAIN) { mainID = i.id }
      if e.lastSeq != i.move_seq || !e.shown {
        e.lastSeq = i.move_seq
        e.applyFrame(i)
      }
      if visible && !e.shown {
        e.shown = true
        if has(i.flags, DH_WIN_MAIN) || has(i.flags, DH_WIN_POPUP) || e.window.canBeKey { e.window.makeKeyAndOrderFront(nil) } else { e.window.orderFront(nil) }
        if has(i.flags, DH_WIN_MAIN) { NSApp.activate(ignoringOtherApps: true) }
        e.view.syncDrawable()
      } else if !visible && e.shown && NSEvent.pressedMouseButtons == 0 {
        // Hidden by a click inside the window itself (its own close button): wait for the button to come up, or the
        // window server keeps the window on screen for the rest of that mouse grab.
        e.shown = false
        e.window.orderOut(nil)
        if fpsLog { NSLog("DeckHand hide window %u (%@)", i.id, infoRole(i)) }
      }
    }
    for (id, e) in entries where !seen.contains(id) {  // the core dropped the window (panel closed, skin without that module)
      e.window.delegate = nil
      e.window.orderOut(nil)
      e.window.contentView = NSView()
      entries[id] = nil
      order.removeAll { $0 == id }
    }
  }

  @objc private func tick(_ link: CADisplayLink) {
    guard !inTick, let core else { return }
    inTick = true
    defer { inTick = false }
    let now = CACurrentMediaTime()
    dh_app_begin_frame(core, now - lastTick)
    lastTick = now
    applyWindowFrames()

    var drawn: [(ShellEntry, UnsafePointer<dh_frame>)] = []
    for id in order {
      guard let e = entries[id], e.shown, e.window.occlusionState.contains(.visible), !e.window.isMiniaturized else { continue }
      let b = e.view.backing
      let px = CGSize(width: max(1, (e.view.bounds.width * b).rounded()), height: max(1, (e.view.bounds.height * b).rounded()))
      if px != e.lastPx || b != e.lastBacking {
        e.lastPx = px
        e.lastBacking = b
        e.view.syncDrawable()
        dh_app_window_display(core, id, Float(px.width), Float(px.height), Float(b))
      }
      if let f = dh_app_window_frame(core, id) { drawn.append((e, f)) }
    }
    if let up = dh_app_end_frame(core) {  // textures first: every window's commands refer to the one shared set
      for i in 0..<Int(up.pointee.upload_count) { renderer.apply(up.pointee.uploads[i]) }
      for i in 0..<Int(up.pointee.release_count) { renderer.textures[up.pointee.releases[i]] = nil }
    }
    for (e, f) in drawn {
      guard let drawable = e.view.metal.nextDrawable() else { continue }
      let pass = MTLRenderPassDescriptor()
      pass.colorAttachments[0].texture = drawable.texture
      pass.colorAttachments[0].storeAction = .store
      renderer.render(f.pointee, drawable: drawable, pass: pass, size: e.view.metal.drawableSize)
      e.gotFrameOnce = true
      e.shadowTick += 1
      if has(e.info.flags, DH_WIN_SHAPED) && !has(e.info.flags, DH_WIN_NO_SHADOW) && (e.shadowTick == 3 || e.shadowTick % 90 == 0) {
        e.window.invalidateShadow()  // the system shadow follows the alpha of what was just drawn
      }
    }
    if !pendingDecks.isEmpty, let main = entries[mainID], main.gotFrameOnce {  // --deck-a / --deck-b
      let b = Float(main.view.backing)
      let size = main.window.contentView?.bounds.size ?? .zero
      for (path, fx) in pendingDecks {
        path.withCString { dh_app_window_open_file(core, mainID, $0, Float(size.width) * fx * b, Float(size.height) * 0.3 * b) }
      }
      pendingDecks = []
    }
    if fpsLog {  // DECKHAND_FPS=1: frames per second and windows drawn, once a second, to the system log
      fpsFrames += 1
      if now - fpsSince >= 1 {
        NSLog("DeckHand fps %.1f windows %d", Double(fpsFrames) / (now - fpsSince), drawn.count)
        fpsFrames = 0
        fpsSince = now
      }
    }
    if drawn.count > 0 && entries.values.contains(where: { has($0.info.flags, DH_WIN_SHAPED) }) { refreshClickThrough() }
  }

  // menu actions -------------------------------------------------------------------------------------------------
  var windowInfos: [dh_window_info] { infos }
  func setVisible(_ id: UInt32, _ visible: Bool) { dh_app_window_set_visible(core, id, visible) }
  func resetArrangement() { dh_app_windows_reset(core) }
  func openFile(_ path: String) { path.withCString { dh_app_open_file(core, $0, -1, -1) } }
}

// MARK: - application

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
  private var shell: Shell?
  private var moduleItems: [NSMenuItem] = []
  private var classicWindow: NSWindow?
  private var classicView: DeckHandView?

  /// DECKHAND_SINGLE_WINDOW=1: the classic one-window shell (DeckHandView, no multi-window capabilities). It is the same
  /// path a shell that does not announce them takes: modules are docked into the one rectangular window.
  private func startClassic() {
    let rect = NSRect(x: 0, y: 0, width: 1440, height: 900)
    let window = NSWindow(contentRect: rect, styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
    window.title = "DeckHand"
    window.minSize = NSSize(width: 1100, height: 700)
    window.appearance = NSAppearance(named: .darkAqua)
    window.acceptsMouseMovedEvents = true
    guard let device = MTLCreateSystemDefaultDevice() else { fail("DeckHand needs a Metal-capable graphics device."); return }
    let view = DeckHandView(frame: rect, device: device)
    guard view.start() else { fail("DeckHand could not start. See deckhand.log in its data folder."); return }
    classicView = view
    classicWindow = window
    window.contentView = view
    window.center()
    window.makeKeyAndOrderFront(nil)
    window.makeFirstResponder(view)
    NSApp.activate(ignoringOtherApps: true)
    let args = CommandLine.arguments
    for (flag, x) in [("--deck-a", 10.0), ("--deck-b", Double(rect.width) - 10.0)] {
      if let i = args.firstIndex(of: flag), i + 1 < args.count { view.openFile(args[i + 1], at: CGPoint(x: x, y: 100)) }
    }
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    buildMenu()
    if ProcessInfo.processInfo.environment["DECKHAND_SINGLE_WINDOW"] != nil {
      startClassic()
      return
    }
    guard let device = MTLCreateSystemDefaultDevice() else {
      fail("DeckHand needs a Metal-capable graphics device.")
      return
    }
    guard let shell = Shell(device: device) else {
      fail("DeckHand could not start. See deckhand.log in its data folder.")
      return
    }
    self.shell = shell
    // Optional: DeckHand --deck-a <file> --deck-b <file> loads tracks at launch (once the first frame is up).
    let args = CommandLine.arguments
    var decks: [(String, Float)] = []
    for (flag, x) in [("--deck-a", Float(0.22)), ("--deck-b", Float(0.78))] {
      if let i = args.firstIndex(of: flag), i + 1 < args.count { decks.append((args[i + 1], x)) }
    }
    shell.start(decks: decks)
    NSApp.activate(ignoringOtherApps: true)
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
  func applicationWillTerminate(_ notification: Notification) {
    shell?.shutdown()
    classicView?.shutdown()
  }

  func application(_ sender: NSApplication, openFiles filenames: [String]) {
    for file in filenames {
      if let shell { shell.openFile(file) } else { classicView?.openFile(file, at: nil) }
    }
    sender.reply(toOpenOrPrint: .success)
  }

  @objc private func showBluetoothMIDI() { Platform.showBluetoothMIDI() }
  @objc private func resetArrangement() { shell?.resetArrangement() }
  @objc private func minimizeKey() { NSApp.keyWindow?.miniaturize(nil) }
  @objc private func closeKey() { NSApp.keyWindow?.performClose(nil) }
  @objc private func toggleModule(_ item: NSMenuItem) {
    guard let shell else { return }
    let id = UInt32(item.tag)
    shell.setVisible(id, item.state != .on)
  }

  private func fail(_ message: String) {
    let alert = NSAlert()
    alert.messageText = message
    alert.runModal()
    NSApp.terminate(nil)
  }

  // The Window menu lists every module the skin declares, with a check mark while it is shown.
  func menuNeedsUpdate(_ menu: NSMenu) {
    for item in moduleItems { menu.removeItem(item) }
    moduleItems = []
    guard let shell else { return }
    let at = menu.items.firstIndex(where: { $0.title == "Reset Window Arrangement" }) ?? menu.items.count
    var index = at
    for i in shell.windowInfos where !has(i.flags, DH_WIN_MAIN) && !has(i.flags, DH_WIN_PANEL) {
      let item = NSMenuItem(title: infoTitleOrName(i), action: #selector(toggleModule(_:)), keyEquivalent: "")
      item.target = self
      item.tag = Int(i.id)
      item.state = has(i.flags, DH_WIN_VISIBLE) ? .on : .off
      menu.insertItem(item, at: index)
      moduleItems.append(item)
      index += 1
    }
    if !moduleItems.isEmpty {
      let sep = NSMenuItem.separator()
      menu.insertItem(sep, at: index)
      moduleItems.append(sep)
    }
  }
  private func infoTitleOrName(_ i: dh_window_info) -> String {
    let role = infoRole(i)
    return role.prefix(1).uppercased() + role.dropFirst().replacingOccurrences(of: "_", with: " ")
  }

  private func buildMenu() {
    let main = NSMenu()
    let appItem = NSMenuItem()
    main.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.addItem(withTitle: "About DeckHand", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
    appMenu.addItem(.separator())
    let bluetooth = appMenu.addItem(withTitle: "Bluetooth MIDI\u{2026}", action: #selector(showBluetoothMIDI), keyEquivalent: "")
    bluetooth.target = self
    appMenu.addItem(.separator())
    appMenu.addItem(withTitle: "Hide DeckHand", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
    appMenu.addItem(withTitle: "Quit DeckHand", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu

    let windowItem = NSMenuItem()
    main.addItem(windowItem)
    let windowMenu = NSMenu(title: "Window")
    windowMenu.delegate = self
    let close = windowMenu.addItem(withTitle: "Close", action: #selector(closeKey), keyEquivalent: "w")
    close.target = self
    let minimize = windowMenu.addItem(withTitle: "Minimize", action: #selector(minimizeKey), keyEquivalent: "m")
    minimize.target = self
    windowMenu.addItem(withTitle: "Enter Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f")
    windowMenu.addItem(.separator())
    let reset = windowMenu.addItem(withTitle: "Reset Window Arrangement", action: #selector(resetArrangement), keyEquivalent: "")
    reset.target = self
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
