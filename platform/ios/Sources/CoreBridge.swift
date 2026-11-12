import Foundation

/// Owns the C core handle and the platform callbacks. The only file that talks to the C API.
final class CoreBridge {
    private(set) var app: OpaquePointer!

    init(dataDir: String) {
        var platform = dh_platform(user: Unmanaged.passUnretained(self).toOpaque()) { _, utf8, px, out in
            guard let utf8, let out else { return false }
            return TextRasteriser.rasterise(String(cString: utf8), pixelSize: CGFloat(px), into: out)
        }
        app = dh_app_create(&platform, dataDir)
    }
    deinit { dh_app_destroy(app) }

    func setDisplay(size: CGSize, scale: CGFloat, insets: UIEdgeInsets) {
        let s = Float(scale)
        dh_app_set_display(app, Float(size.width) * s, Float(size.height) * s, s,
                           Float(insets.left) * s, Float(insets.top) * s, Float(insets.right) * s, Float(insets.bottom) * s)
    }
    func pointer(_ k: dh_ptr_kind, id: UInt32, x: Float, y: Float) { dh_app_pointer(app, k, id, x, y) }
    func text(_ s: String) { dh_app_text(app, s) }
    func backspace() { dh_app_backspace(app) }
    func midi(_ bytes: [UInt8]) { dh_app_midi(app, bytes, UInt32(bytes.count)) }
    func audioRoute(channels: Int, master: Int, cue: Int) { dh_app_audio_route(app, UInt32(channels), Int32(master), Int32(cue)) }
    func openFile(_ path: String) { dh_app_open_file(app, path) }
    func frame(dt: Double) -> UnsafePointer<dh_frame>? { dh_app_frame(app, dt) }
}
import UIKit
