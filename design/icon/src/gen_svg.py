#!/usr/bin/env python3
"""Generates the DeckHand icon concepts and the primary masters as SVG (standard library only).
Usage: python3 design/icon/src/gen_svg.py   (writes design/icon/concepts/*.svg and design/icon/deckhand-*.svg)
All geometry is computed here, so the artwork is original, parametric and reproducible."""
import math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.dirname(HERE)  # design/icon
C = 512

# Palette (documented in README.md)
NAVY_TOP, NAVY_BOT = "#14405E", "#07121F"
BRASS_HI, BRASS_MID, BRASS_LO = "#FFE0A0", "#F0A93A", "#9C5A17"
TEAL, TEAL_HI = "#2FE0C6", "#B8FFF2"
VINYL, VINYL_HI = "#0C141E", "#233246"

def f(x):
    return ("%.2f" % x).rstrip("0").rstrip(".")

def pol(r, deg):  # polar to xy, 0 deg = up, clockwise
    a = math.radians(deg)
    return C + r * math.sin(a), C - r * math.cos(a)

def defs():
    return f"""<defs>
<linearGradient id="bg" x1="0.15" y1="0" x2="0.85" y2="1"><stop offset="0" stop-color="{NAVY_TOP}"/><stop offset="1" stop-color="{NAVY_BOT}"/></linearGradient>
<radialGradient id="bgGlow" cx="0.5" cy="0.42" r="0.6"><stop offset="0" stop-color="{TEAL}" stop-opacity="0.16"/><stop offset="1" stop-color="{TEAL}" stop-opacity="0"/></radialGradient>
<radialGradient id="shadow" cx="0.5" cy="0.56" r="0.45"><stop offset="0.7" stop-color="#000" stop-opacity="0.38"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>
<linearGradient id="brass" x1="0.1" y1="0" x2="0.9" y2="1"><stop offset="0" stop-color="{BRASS_HI}"/><stop offset="0.5" stop-color="{BRASS_MID}"/><stop offset="1" stop-color="{BRASS_LO}"/></linearGradient>
<linearGradient id="brassR" x1="0.9" y1="0" x2="0.1" y2="1"><stop offset="0" stop-color="{BRASS_HI}"/><stop offset="0.55" stop-color="{BRASS_MID}"/><stop offset="1" stop-color="{BRASS_LO}"/></linearGradient>
<linearGradient id="vinyl" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{VINYL_HI}"/><stop offset="0.5" stop-color="{VINYL}"/><stop offset="1" stop-color="#05090E"/></linearGradient>
<linearGradient id="label" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{TEAL_HI}"/><stop offset="0.5" stop-color="{TEAL}"/><stop offset="1" stop-color="#0E8F86"/></linearGradient>
</defs>"""

def bg():
    return ('<rect width="1024" height="1024" fill="url(#bg)"/>'
            '<rect width="1024" height="1024" fill="url(#bgGlow)"/>'
            '<rect width="1024" height="1024" fill="url(#shadow)"/>')

def circle(r, fill="none", cx=C, cy=C, stroke=None, sw=0, op=None, extra=""):
    s = f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}" fill="{fill}"'
    if stroke:
        s += f' stroke="{stroke}" stroke-width="{f(sw)}"'
    if op is not None:
        s += f' opacity="{op}"'
    return s + f" {extra}/>"

def svg(inner, viewbox="0 0 1024 1024", with_bg=True):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{viewbox}" width="1024" height="1024">\n'
            f'{defs()}\n{bg() if with_bg else ""}\n{inner}\n</svg>\n')

def arc_wedge(r0, r1, a0, a1):
    x0, y0 = pol(r1, a0); x1, y1 = pol(r1, a1); x2, y2 = pol(r0, a1); x3, y3 = pol(r0, a0)
    return (f"M{f(x0)} {f(y0)} A{r1} {r1} 0 0 1 {f(x1)} {f(y1)} L{f(x2)} {f(y2)} "
            f"A{r0} {r0} 0 0 0 {f(x3)} {f(y3)} Z")

# ---------------------------------------------------------------- A: helm wheel whose rim is a platter
def helm(spokes=8, offset=22.5, ring_only=True):
    """Full helm motif on transparent. The interior of the rim is open (spokes only) via a ring built from paths."""
    o = []
    for k in range(spokes):
        a = offset + k * 360 / spokes
        o.append(f'<g transform="rotate({f(a)} {C} {C})">'
                 f'<rect x="{C-21}" y="{C-340}" width="42" height="260" fill="url(#brass)"/>'
                 f'<rect x="{C-34}" y="{C-402}" width="68" height="112" rx="34" fill="url(#brass)"/>'
                 f'<rect x="{C-34}" y="{C-352}" width="68" height="12" fill="{BRASS_LO}" opacity="0.55"/>'
                 f'<rect x="{C-34}" y="{C-327}" width="68" height="8" fill="{BRASS_LO}" opacity="0.4"/>'
                 f'</g>')
    # outer brass rim ring (r 324..298), vinyl band (298..212), inner brass ring (212..186)
    o.append(f'<path fill="url(#brassR)" fill-rule="evenodd" d="{ring(324, 186)}"/>')
    o.append(f'<path fill="url(#vinyl)" fill-rule="evenodd" d="{ring(299, 211)}"/>')
    for r in (286, 270, 254, 238, 224):
        o.append(circle(r, stroke="#FFFFFF", sw=2.5, op=0.10))
    # platter sheen
    for a0, a1 in ((296, 330), (116, 150)):
        x0, y0 = pol(262, a0); x1, y1 = pol(262, a1)
        o.append(f'<path d="M{f(x0)} {f(y0)} A262 262 0 0 1 {f(x1)} {f(y1)}" fill="none" stroke="#fff" stroke-width="30" stroke-linecap="round" opacity="0.06"/>')
        x0, y0 = pol(262, a0 + 6); x1, y1 = pol(262, a1 - 6)
        o.append(f'<path d="M{f(x0)} {f(y0)} A262 262 0 0 1 {f(x1)} {f(y1)}" fill="none" stroke="#fff" stroke-width="8" stroke-linecap="round" opacity="0.14"/>')
    # hub: brass disc, teal label, spindle
    o.append(circle(124, "url(#brass)"))
    o.append(circle(124, stroke=BRASS_LO, sw=6, op=0.5))
    o.append(circle(86, "url(#label)"))
    o.append(circle(58, stroke="#06544F", sw=3, op=0.55))
    o.append(circle(15, "#07121F"))
    # playhead notch at 12 o'clock (between two handles)
    px0, py0 = pol(212, 0); px1, py1 = pol(299, 0)
    o.append(f'<rect x="{C-16}" y="{f(py1-4)}" width="32" height="{f(py0-py1+8)}" rx="10" fill="{TEAL}" opacity="0.22"/>')
    o.append(f'<rect x="{C-7}" y="{f(py1)}" width="14" height="{f(py0-py1)}" rx="4" fill="{TEAL_HI}"/>')
    o.append(f'<path d="M{C-26} {f(py1-26)} L{C+26} {f(py1-26)} L{C} {f(py1+22)} Z" fill="{TEAL}" stroke="#07121F" stroke-width="6" stroke-linejoin="round"/>')
    return "\n".join(o)

def helm_small(spokes=8, offset=22.5):
    """Optical variant for 16-48 px: no grooves, heavier spokes and rim, bigger hub and playhead."""
    o = []
    for k in range(spokes):
        a = offset + k * 360 / spokes
        o.append(f'<g transform="rotate({f(a)} {C} {C})">'
                 f'<rect x="{C-34}" y="{C-340}" width="68" height="260" fill="url(#brass)"/>'
                 f'<rect x="{C-52}" y="{C-410}" width="104" height="130" rx="52" fill="url(#brass)"/></g>')
    o.append(f'<path fill="url(#brassR)" fill-rule="evenodd" d="{ring(338, 170)}"/>')
    o.append(f'<path fill="#0A1119" fill-rule="evenodd" d="{ring(272, 232)}"/>')
    o.append(f'<path fill="#101A26" fill-rule="evenodd" d="{ring(272, 206)}"/>')
    o.append(circle(140, "url(#brass)"))
    o.append(circle(96, "url(#label)"))
    o.append(circle(20, "#07121F"))
    py0 = C - 206; py1 = C - 272
    o.append(f'<rect x="{C-22}" y="{py1-6}" width="44" height="{py0-py1+12}" rx="12" fill="{TEAL_HI}"/>')
    o.append(f'<path d="M{C-44} {py1-46} L{C+44} {py1-46} L{C} {py1+14} Z" fill="{TEAL}" stroke="#07121F" stroke-width="10" stroke-linejoin="round"/>')
    return "\n".join(o)

def ring(ro, ri):
    return (f"M{C-ro} {C} a{ro} {ro} 0 1 0 {2*ro} 0 a{ro} {ro} 0 1 0 {-2*ro} 0 Z "
            f"M{C-ri} {C} a{ri} {ri} 0 1 0 {2*ri} 0 a{ri} {ri} 0 1 0 {-2*ri} 0 Z")

# ---------------------------------------------------------------- B: open palm, palm is a platter, fingers are fader caps
def palm():
    o = []
    px, py = C, 640
    def finger(ang, length, w):
        return (f'<g transform="rotate({ang} {px} {py})">'
                f'<rect x="{px-w//2}" y="{py-length}" width="{w}" height="{length}" rx="{w//2}" fill="url(#brass)"/>'
                f'<rect x="{px-w//2+12}" y="{py-length+34}" width="{w-24}" height="40" rx="14" fill="{VINYL}"/>'
                f'<rect x="{px-w//2+12}" y="{py-length+34}" width="{w-24}" height="10" rx="5" fill="{TEAL}"/></g>')
    # fingers behind the palm, thumb at the left
    for ang, length, w in ((-26, 470, 96), (-9, 520, 98), (9, 520, 98), (26, 470, 96), (-72, 390, 104)):
        o.append(finger(ang, length, w))
    o.append(circle(262, "url(#brassR)", cx=px, cy=py))
    o.append(circle(238, "url(#vinyl)", cx=px, cy=py))
    for r in (216, 196, 176, 156):
        o.append(circle(r, cx=px, cy=py, stroke="#fff", sw=2.5, op=0.1))
    o.append(circle(92, "url(#label)", cx=px, cy=py))
    o.append(circle(15, "#07121F", cx=px, cy=py))
    return "\n".join(o)

# ---------------------------------------------------------------- C: jog wheel, waveform through the centre, rope ring
def jog():
    o = []
    o.append(circle(392, "url(#vinyl)"))
    # rope ring: slanted segments
    n = 48
    for k in range(n):
        a = k * 360 / n
        x, y = pol(372, a)
        o.append(f'<rect x="{f(x-20)}" y="{f(y-42)}" width="40" height="84" rx="20" fill="url(#brass)" stroke="{BRASS_LO}" stroke-width="3" '
                 f'transform="rotate({f(a+28)} {f(x)} {f(y)})"/>')
    o.append(circle(318, stroke=BRASS_LO, sw=10, op=0.8))
    o.append(circle(300, "url(#vinyl)"))
    for r in (280, 258, 236):
        o.append(circle(r, stroke="#fff", sw=2.5, op=0.1))
    # waveform bars crossing the centre
    hs = [40, 70, 120, 90, 180, 130, 230, 150, 260, 110, 200, 160, 90, 60, 36]
    bw, gap = 26, 14
    total = len(hs) * bw + (len(hs) - 1) * gap
    x0 = C - total / 2
    for i, h in enumerate(hs):
        x = x0 + i * (bw + gap)
        col = TEAL if i < 8 else BRASS_MID
        o.append(f'<rect x="{f(x)}" y="{f(C-h/2)}" width="{bw}" height="{h}" rx="13" fill="{col}"/>')
    o.append(f'<rect x="{C-5}" y="{C-300}" width="10" height="600" rx="5" fill="{TEAL_HI}" opacity="0.95"/>')
    return "\n".join(o)

# ---------------------------------------------------------------- D: two decks, crossfader line
def decks():
    o = []
    r = 250
    ax, bx = C - 150, C + 150
    for cx, label in ((ax, "label"), (bx, "brass")):
        o.append(circle(r + 22, "url(#brassR)", cx=cx))
        o.append(circle(r, "url(#vinyl)", cx=cx))
        for rr in (225, 200, 175):
            o.append(circle(rr, cx=cx, stroke="#fff", sw=2.5, op=0.1))
        o.append(circle(78, f"url(#{label})", cx=cx))
        o.append(circle(13, "#07121F", cx=cx))
    # overlap lens, lit
    o.append(f'<clipPath id="lensA"><circle cx="{ax}" cy="{C}" r="{r+22}"/></clipPath>')
    o.append(f'<g clip-path="url(#lensA)"><circle cx="{bx}" cy="{C}" r="{r+22}" fill="{TEAL}" opacity="0.28"/></g>')
    # crossfader
    o.append(f'<rect x="{C-340}" y="{C+300}" width="680" height="34" rx="17" fill="{VINYL}" stroke="url(#brass)" stroke-width="8"/>')
    o.append(f'<rect x="{C-60}" y="{C+270}" width="120" height="94" rx="22" fill="url(#brass)"/>')
    o.append(f'<rect x="{C-6}" y="{C+280}" width="12" height="74" rx="6" fill="{VINYL}"/>')
    return "\n".join(o)

CONCEPTS = {
    "a-helm": ("Helm", helm()),
    "b-palm": ("Palm", palm()),
    "c-jog-rope": ("Jog and rope", jog()),
    "d-two-decks": ("Two decks", decks()),
}

# ---------------------------------------------------------------- monochrome template (alpha only)
def mono(spokes=8, offset=22.5):
    k = "#000"
    o = []
    for i in range(spokes):
        a = offset + i * 360 / spokes
        o.append(f'<g transform="rotate({f(a)} {C} {C})"><rect x="{C-21}" y="{C-340}" width="42" height="260" fill="{k}"/>'
                 f'<rect x="{C-34}" y="{C-402}" width="68" height="112" rx="34" fill="{k}"/></g>')
    o.append(circle(311, stroke=k, sw=26))
    o.append(circle(199, stroke=k, sw=26))
    for r in (278, 250, 224):
        o.append(circle(r, stroke=k, sw=8, op=0.55))
    o.append(circle(124, k))
    o.append(f'<rect x="{C-12}" y="{C-312}" width="24" height="126" rx="6" fill="{k}"/>')
    return "\n".join(o)

def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)

def main():
    for name, (_, inner) in CONCEPTS.items():
        write(os.path.join(OUT, "concepts", name + ".svg"), svg(inner))
    primary = helm()
    # master icon: full-bleed square; motif at its natural size (handle tips at r 402 of 512, inside every platform mask)
    write(os.path.join(OUT, "deckhand-icon.svg"), svg(primary))
    small = helm_small()
    write(os.path.join(OUT, "deckhand-icon-small.svg"), svg(small))
    write(os.path.join(OUT, "deckhand-mark-small.svg"), svg(small, viewbox="100 100 824 824", with_bg=False))
    # free-form mark: transparent, cropped to the motif plus a small margin
    write(os.path.join(OUT, "deckhand-mark.svg"), svg(primary, viewbox="100 100 824 824", with_bg=False))
    write(os.path.join(OUT, "deckhand-mark-mono.svg"),
          f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="100 100 824 824" width="1024" height="1024">\n{mono()}\n</svg>\n')

if __name__ == "__main__":
    main()
