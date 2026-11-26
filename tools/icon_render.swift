// Rasteriser helper for tools/make_icons.sh (AppKit only; nothing to install). Uses the system SVG renderer.
//   icon_render svg  IN.svg OUT.png SIZE            plain render, transparent where the SVG is
//   icon_render tile IN.svg OUT.png SIZE            macOS tile: artwork in a rounded rectangle with margins and a soft shadow
//   icon_render flat IN.svg OUT.png SIZE            opaque square (iOS): no alpha, no rounded corners
//   icon_render dark MARK.svg OUT.png SIZE          iOS dark variant: the mark on a near-black tile
//   icon_render tint IN.svg OUT.png SIZE            iOS tinted variant: greyscale, opaque
//   icon_render rgba IN.svg OUT.rgba SIZE           raw straight-alpha RGBA bytes (for the Linux window icon)
//   icon_render sheet OUT.png label=a.svg ...       contact sheet of concepts (256 px and 32 px, labelled)
import AppKit

func die(_ s: String) -> Never { FileHandle.standardError.write((s + "\n").data(using: .utf8)!); exit(1) }

func bitmap(_ w: Int, _ h: Int) -> NSBitmapImageRep {
    let r = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                     isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 32)!
    memset(r.bitmapData!, 0, r.bytesPerRow * h)  // fresh bitmaps are not guaranteed to start transparent
    return r
}

func draw(into rep: NSBitmapImageRep, _ body: (CGContext) -> Void) {
    let gc = NSGraphicsContext(bitmapImageRep: rep)!
    gc.imageInterpolation = .high
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = gc
    body(gc.cgContext)
    NSGraphicsContext.restoreGraphicsState()
}

func loadSVG(_ path: String) -> NSImage {
    guard let img = NSImage(contentsOf: URL(fileURLWithPath: path)) else { die("cannot read \(path)") }
    return img
}

func renderSVG(_ path: String, _ size: Int) -> NSBitmapImageRep {
    let img = loadSVG(path)
    let rep = bitmap(size, size)
    draw(into: rep) { _ in img.draw(in: NSRect(x: 0, y: 0, width: size, height: size)) }
    return rep
}

func save(_ rep: NSBitmapImageRep, _ path: String) {
    guard let d = rep.representation(using: .png, properties: [:]) else { die("png encode failed") }
    try! d.write(to: URL(fileURLWithPath: path))
}

func tilePath(_ r: CGRect) -> CGPath {
    // Rounded rectangle with the macOS proportion (corner radius ~22.4% of the tile edge).
    let rad = r.width * 0.2237
    return CGPath(roundedRect: r, cornerWidth: rad, cornerHeight: rad, transform: nil)
}

func renderTile(_ path: String, _ size: Int) -> NSBitmapImageRep {
    let art = renderSVG(path, max(size, 64) * (size < 64 ? 1 : 1))
    let rep = bitmap(size, size)
    let s = CGFloat(size)
    // Small sizes keep a thinner margin so the motif keeps its pixels (macOS does the same for 16 and 32 px).
    let m: CGFloat = size <= 40 ? 0.04 : 100.0 / 1024
    let tile = CGRect(x: s * m, y: s * m, width: s * (1 - 2 * m), height: s * (1 - 2 * m))
    draw(into: rep) { cg in
        // soft shadow under the tile
        cg.saveGState()
        cg.setShadow(offset: CGSize(width: 0, height: -s * (size <= 40 ? 0.01 : 10.0 / 1024)), blur: s * (size <= 40 ? 0.03 : 22.0 / 1024), color: CGColor(gray: 0, alpha: 0.38))
        cg.addPath(tilePath(tile)); cg.setFillColor(CGColor(gray: 0.1, alpha: 1)); cg.fillPath()
        cg.restoreGState()
        cg.saveGState()
        cg.addPath(tilePath(tile)); cg.clip()
        put(art, NSRect(x: tile.minX, y: tile.minY, width: tile.width, height: tile.height))
        // gentle top light and a hairline rim so the tile separates from dark backgrounds
        let g = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                           colors: [CGColor(gray: 1, alpha: 0.14), CGColor(gray: 1, alpha: 0)] as CFArray, locations: [0, 1])!
        cg.drawLinearGradient(g, start: CGPoint(x: tile.midX, y: tile.maxY), end: CGPoint(x: tile.midX, y: tile.midY), options: [])
        cg.restoreGState()
        cg.addPath(tilePath(tile.insetBy(dx: 0.5, dy: 0.5)))
        cg.setStrokeColor(CGColor(gray: 1, alpha: 0.16)); cg.setLineWidth(max(1, s / 512)); cg.strokePath()
    }
    return rep
}

func renderFlat(_ path: String, _ size: Int) -> NSBitmapImageRep {
    let art = renderSVG(path, size)
    let rep = bitmap(size, size)
    draw(into: rep) { cg in
        cg.setFillColor(CGColor(red: 0.03, green: 0.07, blue: 0.12, alpha: 1)); cg.fill(CGRect(x: 0, y: 0, width: size, height: size))
        put(art, NSRect(x: 0, y: 0, width: size, height: size))
    }
    return rep
}

func renderDark(_ markPath: String, _ size: Int) -> NSBitmapImageRep {
    let s = CGFloat(size)
    let mark = renderSVG(markPath, size)
    let rep = bitmap(size, size)
    draw(into: rep) { cg in
        let g = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                           colors: [CGColor(red: 0.05, green: 0.12, blue: 0.19, alpha: 1), CGColor(red: 0.01, green: 0.03, blue: 0.06, alpha: 1)] as CFArray,
                           locations: [0, 1])!
        cg.drawLinearGradient(g, start: CGPoint(x: 0, y: s), end: CGPoint(x: s, y: 0), options: [])
        let m = s * 824 / 1024
        put(mark, NSRect(x: (s - m) / 2, y: (s - m) / 2, width: m, height: m))
    }
    return rep
}

func renderTint(_ path: String, _ size: Int) -> NSBitmapImageRep {
    let src = renderFlat(path, size)
    let rep = bitmap(size, size)
    let n = size * size
    let p = src.bitmapData!, q = rep.bitmapData!
    for i in 0..<n {
        let r = Double(p[i * 4]), g = Double(p[i * 4 + 1]), b = Double(p[i * 4 + 2])
        // stretch so the mark spans dark grey to near white; the system maps this luminance to the tint colour
        var l = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255
        l = min(1, max(0, (l - 0.02) * 1.25))
        let v = UInt8(l * 255)
        q[i * 4] = v; q[i * 4 + 1] = v; q[i * 4 + 2] = v; q[i * 4 + 3] = 255
    }
    return rep
}

func put(_ rep: NSBitmapImageRep, _ r: NSRect) {  // NSBitmapImageRep.draw(in:) copies; composite source-over instead
    let im = NSImage(size: NSSize(width: rep.pixelsWide, height: rep.pixelsHigh)); im.addRepresentation(rep)
    im.draw(in: r, from: .zero, operation: .sourceOver, fraction: 1)
}

func saveOpaque(_ rep: NSBitmapImageRep, _ path: String) {  // PNG without an alpha channel (App Store icons must not have one)
    let w = rep.pixelsWide
    guard let src = rep.cgImage,
          let cg = CGContext(data: nil, width: w, height: w, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                             bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { die("opaque context failed") }
    cg.draw(src, in: CGRect(x: 0, y: 0, width: w, height: w))
    guard let out = cg.makeImage(),
          let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil) else { die("png failed") }
    CGImageDestinationAddImage(dest, out, nil)
    if !CGImageDestinationFinalize(dest) { die("png write failed") }
}

func label(_ text: String, at p: CGPoint, color: NSColor, size: CGFloat) {
    (text as NSString).draw(at: p, withAttributes: [.font: NSFont.systemFont(ofSize: size, weight: .semibold), .foregroundColor: color])
}

func sheet(_ out: String, _ items: [(String, String)]) {
    let cw = 300, W = cw * items.count + 20, H = 520
    let rep = bitmap(W, H)
    draw(into: rep) { cg in
        cg.setFillColor(CGColor(gray: 0.17, alpha: 1)); cg.fill(CGRect(x: 0, y: 0, width: W, height: H))
        cg.setFillColor(CGColor(gray: 0.96, alpha: 1)); cg.fill(CGRect(x: 0, y: 0, width: W, height: 150))
        for (i, it) in items.enumerated() {
            let x = 20 + i * cw
            put(renderTile(it.1, 256), NSRect(x: x, y: 250, width: 256, height: 256))
            label(it.0, at: CGPoint(x: x + 4, y: 218), color: .white, size: 15)
            // small sizes on dark and light grounds
            put(renderTile(it.1, 32), NSRect(x: x, y: 176, width: 32, height: 32))
            put(renderTile(it.1, 16), NSRect(x: x + 44, y: 184, width: 16, height: 16))
            label("32 / 16 px", at: CGPoint(x: x + 70, y: 184), color: NSColor(white: 0.7, alpha: 1), size: 11)
            put(renderTile(it.1, 32), NSRect(x: x, y: 100, width: 32, height: 32))
            put(renderTile(it.1, 16), NSRect(x: x + 44, y: 108, width: 16, height: 16))
            put(renderTile(it.1, 128), NSRect(x: x + 80, y: 22, width: 96, height: 96))
            label("on light", at: CGPoint(x: x, y: 62), color: NSColor(white: 0.3, alpha: 1), size: 11)
        }
    }
    save(rep, out)
}

let a = CommandLine.arguments
guard a.count >= 3 else { die("usage: icon_render svg|tile|flat|dark|tint|rgba|sheet ...") }
switch a[1] {
case "svg": save(renderSVG(a[2], Int(a[4])!), a[3])
case "tile": save(renderTile(a[2], Int(a[4])!), a[3])
case "flat": saveOpaque(renderFlat(a[2], Int(a[4])!), a[3])
case "dark": saveOpaque(renderDark(a[2], Int(a[4])!), a[3])
case "tint": saveOpaque(renderTint(a[2], Int(a[4])!), a[3])
case "rgba":
    // Round-trip through PNG: the decoded bitmap is straight (non-premultiplied) alpha, which SDL and ICO want.
    let s = Int(a[4])!
    let png = renderSVG(a[2], s).representation(using: .png, properties: [:])!
    let rep = NSBitmapImageRep(data: png)!
    precondition(rep.pixelsWide == s && rep.samplesPerPixel == 4)
    try! Data(bytes: rep.bitmapData!, count: s * s * 4).write(to: URL(fileURLWithPath: a[3]))
case "sheet":
    let items = a[3...].map { s -> (String, String) in
        let p = s.split(separator: "=", maxSplits: 1).map(String.init); return (p[0], p[1]) }
    sheet(a[2], items)
default: die("unknown command \(a[1])")
}
