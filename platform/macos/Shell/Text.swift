// Text rasteriser handed to the core: CoreText shapes and draws any script into an 8-bit
// alpha bitmap. The buffer is malloc'd here and freed by the core.
import AppKit
import CoreText

func rasteriseText(_ user: UnsafeMutableRawPointer?, _ utf8: UnsafePointer<CChar>?, _ px: Float,
                   _ weight: Int32, _ out: UnsafeMutablePointer<dh_bitmap>?) -> Bool {
  guard let utf8, let out, px > 0 else { return false }
  let string = String(cString: utf8)
  let font = NSFont.systemFont(ofSize: CGFloat(px), weight: weight != 0 ? .semibold : .regular)
  let attributes: [NSAttributedString.Key: Any] = [
    .font: font,
    NSAttributedString.Key(kCTForegroundColorFromContextAttributeName as String): true,
  ]
  let line = CTLineCreateWithAttributedString(NSAttributedString(string: string, attributes: attributes))
  var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
  let width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading)
  let w = Int(ceil(width)) + 2
  let h = Int(ceil(ascent) + ceil(descent)) + 2
  guard w > 2, h > 2, w <= 4096, h <= 1024, let memory = malloc(w * h) else { return false }
  memset(memory, 0, w * h)
  guard let context = CGContext(data: memory, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                                space: CGColorSpaceCreateDeviceGray(),
                                bitmapInfo: CGImageAlphaInfo.none.rawValue) else {
    free(memory)
    return false
  }
  context.setFillColor(gray: 1, alpha: 1)
  context.textPosition = CGPoint(x: 1, y: ceil(descent) + 1)
  CTLineDraw(line, context)
  out.pointee = dh_bitmap(w: UInt32(w), h: UInt32(h), ascent: Float(ascent),
                          alpha: memory.assumingMemoryBound(to: UInt8.self))
  return true
}
