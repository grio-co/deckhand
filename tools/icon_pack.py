#!/usr/bin/env python3
"""Container helpers for tools/make_icons.sh (standard library only).
  icon_pack.py ico OUT.ico SIZE:FILE.rgba ...     Windows .ico; 256 is PNG-compressed, smaller sizes are classic 32-bit DIBs
  icon_pack.py ico-check IN.ico [OUTDIR]          parse the entry table back, optionally write every entry as PNG
  icon_pack.py header IN.rgba OUT.hpp SIZE        C++ header with SIZExSIZE ABGR8888 pixels (Linux window icon)
Input .rgba files are raw straight-alpha RGBA bytes, SIZE*SIZE*4 long."""
import os, struct, sys, zlib


def png_from_rgba(rgba, w, h):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib_from_rgba(rgba, s):
    xor = bytearray()
    for y in range(s - 1, -1, -1):  # bottom-up, BGRA
        row = rgba[y * s * 4:(y + 1) * s * 4]
        for x in range(s):
            r, g, b, a = row[x * 4:x * 4 + 4]
            xor += bytes((b, g, r, a))
    mask = bytearray()
    stride = ((s + 31) // 32) * 4
    for y in range(s - 1, -1, -1):  # AND mask: 1 = transparent (alpha is authoritative for 32 bpp, kept consistent anyway)
        bits = bytearray(stride)
        for x in range(s):
            if rgba[(y * s + x) * 4 + 3] < 128:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    hdr = struct.pack("<IiiHHIIiiII", 40, s, s * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return hdr + bytes(xor) + bytes(mask)


def cmd_ico(out, specs):
    entries = []
    for spec in specs:
        size, path = spec.split(":", 1)
        s = int(size)
        rgba = open(path, "rb").read()
        if len(rgba) != s * s * 4:
            sys.exit(f"{path}: expected {s*s*4} bytes, got {len(rgba)}")
        entries.append((s, png_from_rgba(rgba, s, s) if s >= 256 else dib_from_rgba(rgba, s)))
    entries.sort(key=lambda e: e[0])
    data = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    blobs = b""
    for s, blob in entries:
        data += struct.pack("<BBBBHHII", 0 if s >= 256 else s, 0 if s >= 256 else s, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        blobs += blob
    with open(out, "wb") as fh:
        fh.write(data + blobs)


def decode_png_rgba(blob):
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(blob):
        n, t = struct.unpack(">I4s", blob[pos:pos + 8])
        d = blob[pos + 8:pos + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT": idat += d
        pos += 12 + n
    raw, out, stride = zlib.decompress(idat), bytearray(), w * 4
    prev = bytearray(stride)
    for y in range(h):
        ft, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0; b = prev[i]; c = prev[i - 4] if i >= 4 else 0
            if ft == 1: line[i] = (line[i] + a) & 255
            elif ft == 2: line[i] = (line[i] + b) & 255
            elif ft == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line; prev = line
    return w, h, bytes(out)


def cmd_check(path, outdir=None):
    d = open(path, "rb").read()
    rsv, typ, n = struct.unpack("<HHH", d[:6])
    assert rsv == 0 and typ == 1, "not an ICO"
    print(f"{path}: {n} entries, {len(d)} bytes")
    for i in range(n):
        w, h, cc, _, planes, bpp, size, off = struct.unpack("<BBBBHHII", d[6 + 16 * i:22 + 16 * i])
        w, h = w or 256, h or 256
        blob = d[off:off + size]
        assert off + size <= len(d)
        kind = "PNG" if blob[:8] == b"\x89PNG\r\n\x1a\n" else "DIB"
        print(f"  {w}x{h} {bpp}bpp {kind} offset {off} size {size}")
        if outdir:
            os.makedirs(outdir, exist_ok=True)
            if kind == "PNG":
                pw, ph, rgba = decode_png_rgba(blob)
                assert (pw, ph) == (w, h)
            else:
                hs, bw, bh, _, bits = struct.unpack("<IiiHH", blob[:16])
                assert hs == 40 and bw == w and bh == 2 * h and bits == 32
                px = blob[40:40 + w * h * 4]
                rgba = bytearray()
                for y in range(h - 1, -1, -1):
                    for x in range(w):
                        b, g, r, a = px[(y * w + x) * 4:(y * w + x) * 4 + 4]
                        rgba += bytes((r, g, b, a))
            open(os.path.join(outdir, f"ico_{w}.png"), "wb").write(png_from_rgba(bytes(rgba), w, h))


def cmd_header(src, out, size):
    s = int(size)
    d = open(src, "rb").read()
    assert len(d) == s * s * 4
    words = [d[i] | d[i + 1] << 8 | d[i + 2] << 16 | d[i + 3] << 24 for i in range(0, len(d), 4)]  # 0xAABBGGRR
    lines = ["// Generated by tools/make_icons.sh from design/icon/deckhand-mark.svg. Do not edit.",
             "#pragma once", "#include <cstdint>", "",
             f"constexpr int kWindowIconSize = {s};",
             "// Straight alpha, one 0xAABBGGRR word per pixel (SDL_PIXELFORMAT_ABGR8888), row-major.",
             "constexpr std::uint32_t kWindowIconPixels[kWindowIconSize * kWindowIconSize] = {"]
    for i in range(0, len(words), 12):
        lines.append("    " + ",".join("0x%08x" % w for w in words[i:i + 12]) + ",")
    lines.append("};")
    open(out, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    c, a = sys.argv[1], sys.argv[2:]
    if c == "ico": cmd_ico(a[0], a[1:])
    elif c == "ico-check": cmd_check(*a)
    elif c == "header": cmd_header(*a)
    else: sys.exit(__doc__)
