import CoreText
import CoreGraphics
import UIKit

/// Platform text callback: UTF-8 string + pixel size -> 8-bit alpha bitmap (CoreText: all scripts, RTL, fallback fonts).
enum TextRasteriser {
    static func rasterise(_ s: String, pixelSize: CGFloat, into out: UnsafeMutablePointer<dh_bitmap>) -> Bool {
        let font = CTFontCreateWithName("SFProText-Regular" as CFString, pixelSize, nil)
        let attrs: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorFromContextAttributeName as String): true,
        ]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attrs))
        var asc: CGFloat = 0, desc: CGFloat = 0, lead: CGFloat = 0
        let width = CTLineGetTypographicBounds(line, &asc, &desc, &lead)
        let w = max(1, Int(ceil(width)) + 2), h = max(1, Int(ceil(asc + desc)) + 2)
        guard let buf = malloc(w * h) else { return false }
        memset(buf, 0, w * h)
        guard let ctx = CGContext(data: buf, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                                  space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.alphaOnly.rawValue) else {
            free(buf); return false
        }
        ctx.setFillColor(gray: 1, alpha: 1)
        ctx.textPosition = CGPoint(x: 1, y: desc + 1)   // bitmap row 0 is the top
        CTLineDraw(line, ctx)
        let p = buf.assumingMemoryBound(to: UInt8.self)   // CG bitmap row 0 is already the top
        out.pointee = dh_bitmap(w: UInt32(w), h: UInt32(h), ascent: Float(asc), alpha: p)
        return true
    }
}
