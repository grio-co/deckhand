import UIKit
import MetalKit

final class DeckView: MTKView, UIKeyInput {
    var onText: ((String) -> Void)?
    var onKey: ((dh_key) -> Void)?
    var wantsKeyboard = false
    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }
    func insertText(_ text: String) { text == "\n" ? onKey?(DH_KEY_ENTER) : onText?(text) }
    func deleteBackward() { onKey?(DH_KEY_BACKSPACE) }
    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var rest = presses
        for p in presses {
            guard let k = p.key else { continue }
            let map: [UIKeyboardHIDUsage: dh_key] = [.keyboardUpArrow: DH_KEY_UP, .keyboardDownArrow: DH_KEY_DOWN,
                .keyboardLeftArrow: DH_KEY_LEFT, .keyboardRightArrow: DH_KEY_RIGHT, .keyboardEscape: DH_KEY_ESCAPE,
                .keyboardTab: DH_KEY_TAB]
            if let m = map[k.keyCode] { onKey?(m); rest.remove(p) }
        }
        super.pressesBegan(rest, with: event)
    }
    var keyboardType: UIKeyboardType = .default
    var autocorrectionType: UITextAutocorrectionType = .no
}

final class DeckViewController: UIViewController, MTKViewDelegate {
    private var core: CoreBridge!
    private var renderer: Renderer!
    private var deck: DeckView { view as! DeckView }
    private let audio = AudioSessionManager()
    private let files = FileAccess()
    private var touchIDs: [UITouch: UInt32] = [:]
    private var lastSize = CGSize.zero, lastInsets = UIEdgeInsets.zero

    override func loadView() {
        let v = DeckView(frame: .zero)
        v.isMultipleTouchEnabled = true
        v.preferredFramesPerSecond = 60
        view = v
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        guard let r = Renderer(view: deck) else { fatalError("Metal unavailable") }
        renderer = r
        deck.delegate = self
        // Audio session first: the core opens its output as soon as it is created.
        audio.start()
        core = CoreBridge(dataDir: FileAccess.documentsURL.path, resourceDir: Bundle.main.resourcePath ?? "")
        deck.onText = { [weak self] in self?.core.text($0) }
        deck.onKey = { [weak self] in self?.core.key($0) }
        audio.onRouteChange = { [weak self] in self?.core.audioRouteChanged() }
        files.restoreBookmarks()
        files.onFile = { [weak self] url in self?.core.openFile(url.path) }
        files.presenter = self
        NotificationCenter.default.addObserver(forName: .deckOpenFile, object: nil, queue: .main) { [weak self] _ in
            self?.files.presentPicker()
        }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        let size = view.bounds.size, insets = view.safeAreaInsets
        if size != lastSize || insets != lastInsets {
            lastSize = size; lastInsets = insets
            core.setDisplay(size: size, scale: view.contentScaleFactor, insets: insets)
        }
        guard let f = core.frame(dt: 1.0 / 60) else { return }
        renderer.draw(frame: f.pointee, in: deck)
        if f.pointee.wants_text_input != deck.isFirstResponder {
            if f.pointee.wants_text_input { deck.becomeFirstResponder() } else { deck.resignFirstResponder() }
        }
    }

    // MARK: touch -> pointer events (pixels)
    private func send(_ touches: Set<UITouch>, _ kind: dh_ptr_kind) {
        let s = deck.contentScaleFactor
        for t in touches {
            var id = touchIDs[t]
            if id == nil { id = UInt32((0..<10).first { n in !touchIDs.values.contains(UInt32(n)) } ?? 9); touchIDs[t] = id }
            let p = t.location(in: deck)
            core.pointer(kind, id: id!, x: Float(p.x * s), y: Float(p.y * s))
            if kind == DH_PTR_UP || kind == DH_PTR_CANCEL { touchIDs[t] = nil }
        }
    }
    override func touchesBegan(_ t: Set<UITouch>, with e: UIEvent?) { send(t, DH_PTR_DOWN) }
    override func touchesMoved(_ t: Set<UITouch>, with e: UIEvent?) { send(t, DH_PTR_MOVE) }
    override func touchesEnded(_ t: Set<UITouch>, with e: UIEvent?) { send(t, DH_PTR_UP) }
    override func touchesCancelled(_ t: Set<UITouch>, with e: UIEvent?) { send(t, DH_PTR_CANCEL) }
    override var prefersStatusBarHidden: Bool { true }
}
extension Notification.Name { static let deckOpenFile = Notification.Name("deckOpenFile") }
