// The single view: forwards input to the core and renders what the core draws.
import AppKit
import MetalKit

final class DeckHandView: MTKView, MTKViewDelegate {
  private var core: OpaquePointer?
  private var renderer: Renderer?
  private var lastFrame = CACurrentMediaTime()

  override var isFlipped: Bool { true }
  override var acceptsFirstResponder: Bool { true }

  override init(frame: CGRect, device: MTLDevice?) {
    super.init(frame: frame, device: device)
    colorPixelFormat = .bgra8Unorm
    preferredFramesPerSecond = 60
    delegate = self
    registerForDraggedTypes([.fileURL])
    addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                   owner: self, userInfo: nil))
  }

  required init(coder: NSCoder) { fatalError("not used") }

  /// Creates the renderer and the core. Returns false if either cannot start.
  func start() -> Bool {
    guard let device, let renderer = Renderer(device: device, pixelFormat: colorPixelFormat) else {
      NSLog("DeckHand: renderer could not be created")
      return false
    }
    self.renderer = renderer

    // Portable mode: a file named "portable" beside the app keeps all data next to it.
    let fm = FileManager.default
    let beside = Bundle.main.bundleURL.deletingLastPathComponent()
    let dataDir: String
    if fm.fileExists(atPath: beside.appendingPathComponent("portable").path) {
      dataDir = beside.appendingPathComponent("DeckHandData").path
    } else {
      let support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
        ?? URL(fileURLWithPath: NSHomeDirectory())
      dataDir = support.appendingPathComponent("DeckHand").path
    }
    let levels = ["error": 0, "warn": 1, "info": 2, "debug": 3, "trace": 4]
    let level = levels[ProcessInfo.processInfo.environment["DECKHAND_LOG"] ?? "info"] ?? 2
    let resources = Bundle.main.resourcePath ?? ""

    // list_export_targets stays NULL: the core's own scan of /Volumes is right on macOS.
    var platform = dh_platform(user: nil, rasterise_text: rasteriseText, show_bluetooth_midi: Platform.showBluetooth,
                               choose_folder: Platform.chooseFolder, list_export_targets: nil)
    core = dataDir.withCString { data in
      resources.withCString { res in
        var config = dh_config(data_dir: data, resource_dir: res, log_level: Int32(level), touch: false,
                               manage_audio: true)
        return dh_app_create(&platform, &config)
      }
    }
    if core == nil { NSLog("DeckHand: core could not be created") }
    return core != nil
  }

  func shutdown() {
    isPaused = true
    if let core { dh_app_destroy(core) }
    core = nil
  }

  func openFile(_ path: String, at point: CGPoint?) {
    guard let core else { return }
    let s = scale
    path.withCString { dh_app_open_file(core, $0, point.map { Float($0.x) * s } ?? -1, point.map { Float($0.y) * s } ?? -1) }
  }

  private var scale: Float { Float(window?.backingScaleFactor ?? 1) }

  // MARK: drawing
  func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

  private var inFrame = false  // a modal panel opened by the core (folder picker) can re-enter draw

  func draw(in view: MTKView) {
    guard !inFrame else { return }
    inFrame = true
    defer { inFrame = false }
    guard let core, let renderer, let drawable = currentDrawable, let pass = currentRenderPassDescriptor else { return }
    let size = drawableSize
    dh_app_set_display(core, Float(size.width), Float(size.height), scale, 0, 0, 0, 0)
    let now = CACurrentMediaTime()
    guard let frame = dh_app_frame(core, now - lastFrame) else { return }
    lastFrame = now
    renderer.render(frame.pointee, drawable: drawable, pass: pass, size: size)
  }

  // MARK: pointer
  private func pointer(_ kind: dh_ptr_kind, _ event: NSEvent, clicks: Int) {
    guard let core else { return }
    let p = convert(event.locationInWindow, from: nil)
    dh_app_pointer(core, kind, 0, Float(p.x) * scale, Float(p.y) * scale, Int32(clicks))
  }
  override func mouseDown(with event: NSEvent) { pointer(DH_PTR_DOWN, event, clicks: event.clickCount) }
  override func mouseUp(with event: NSEvent) { pointer(DH_PTR_UP, event, clicks: 0) }
  override func mouseDragged(with event: NSEvent) { pointer(DH_PTR_MOVE, event, clicks: 0) }
  override func mouseMoved(with event: NSEvent) { pointer(DH_PTR_MOVE, event, clicks: 0) }

  override func scrollWheel(with event: NSEvent) {
    guard let core else { return }
    let p = convert(event.locationInWindow, from: nil)
    let unit: CGFloat = event.hasPreciseScrollingDeltas ? 1 : 12
    dh_app_scroll(core, Float(p.x) * scale, Float(p.y) * scale, Float(event.scrollingDeltaX * unit) * scale,
                  Float(event.scrollingDeltaY * unit) * scale)
  }

  // MARK: keyboard
  override func flagsChanged(with event: NSEvent) {
    guard let core else { return }
    var mods: UInt32 = 0
    if event.modifierFlags.contains(.shift) { mods |= UInt32(DH_MOD_SHIFT) }
    if event.modifierFlags.contains(.command) { mods |= UInt32(DH_MOD_COMMAND) }
    if event.modifierFlags.contains(.option) { mods |= UInt32(DH_MOD_ALT) }
    if event.modifierFlags.contains(.control) { mods |= UInt32(DH_MOD_CONTROL) }
    dh_app_modifiers(core, mods)
  }

  override func keyDown(with event: NSEvent) {
    guard let core else { return }
    var mods: UInt32 = 0
    if event.modifierFlags.contains(.shift) { mods |= UInt32(DH_MOD_SHIFT) }
    if event.modifierFlags.contains(.command) { mods |= UInt32(DH_MOD_COMMAND) }
    if event.modifierFlags.contains(.option) { mods |= UInt32(DH_MOD_ALT) }
    if event.modifierFlags.contains(.control) { mods |= UInt32(DH_MOD_CONTROL) }
    let keys: [UInt16: dh_key] = [126: DH_KEY_UP, 125: DH_KEY_DOWN, 123: DH_KEY_LEFT, 124: DH_KEY_RIGHT,
                                  36: DH_KEY_ENTER, 76: DH_KEY_ENTER, 53: DH_KEY_ESCAPE, 51: DH_KEY_BACKSPACE,
                                  48: DH_KEY_TAB]
    if let key = keys[event.keyCode] {
      dh_app_key(core, key, mods)
    } else if !event.modifierFlags.contains(.command), !event.modifierFlags.contains(.control),
              let text = event.characters, !text.isEmpty {
      text.withCString { dh_app_text(core, $0) }
    } else {
      super.keyDown(with: event)
    }
  }

  // MARK: drag and drop
  override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { .copy }

  override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
    let point = convert(sender.draggingLocation, from: nil)
    let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                     options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
    for url in urls { openFile(url.path, at: point) }
    return !urls.isEmpty
  }
}
