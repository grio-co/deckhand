import UIKit

/// Owns the C core handle and the platform callbacks. The only file that talks to the C API.
final class CoreBridge {
    private(set) var app: OpaquePointer!

    init(dataDir: String, resourceDir: String) {
        var platform = dh_platform(user: Unmanaged.passUnretained(self).toOpaque()) { _, utf8, px, weight, out in
            guard let utf8, let out else { return false }
            return TextRasteriser.rasterise(String(cString: utf8), pixelSize: CGFloat(px), bold: weight != 0, into: out)
        }
        app = dataDir.withCString { d in
            resourceDir.withCString { r in
                // The shell owns the AVAudioSession (manage_audio false); the core opens the route.
                var cfg = dh_config(data_dir: d, resource_dir: r, log_level: 1, touch: true, manage_audio: false)
                return dh_app_create(&platform, &cfg)
            }
        }
    }
    deinit { dh_app_destroy(app) }

    func setDisplay(size: CGSize, scale: CGFloat, insets: UIEdgeInsets) {
        let s = Float(scale)
        dh_app_set_display(app, Float(size.width) * s, Float(size.height) * s, s,
                           Float(insets.left) * s, Float(insets.top) * s, Float(insets.right) * s, Float(insets.bottom) * s)
    }
    func pointer(_ k: dh_ptr_kind, id: UInt32, x: Float, y: Float, clicks: Int32 = 1) { dh_app_pointer(app, k, id, x, y, clicks) }
    func scroll(x: Float, y: Float, dx: Float, dy: Float) { dh_app_scroll(app, x, y, dx, dy) }
    func key(_ k: dh_key, mods: UInt32 = 0) { dh_app_key(app, k, mods) }
    func text(_ s: String) { dh_app_text(app, s) }
    func audioRouteChanged() { dh_app_audio_route(app) }
    func openFile(_ path: String) { dh_app_open_file(app, path, -1, -1) }
    func frame(dt: Double) -> UnsafePointer<dh_frame>? { dh_app_frame(app, dt) }
}
