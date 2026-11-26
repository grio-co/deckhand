# DeckHand icon

Original artwork, MIT licence (see `docs/BRAND.md`). No existing logo, mark, character, emoji or stock art was used or traced;
every shape is computed by `src/gen_svg.py`.

## The idea

A deckhand is a ship's hand; DeckHand is a hand on the decks. The icon is a **ship's helm whose rim is a record**: eight turned
brass handles around a vinyl band, a teal label for a hub, and a **playhead notch** at twelve o'clock. Steering the wheel is
cueing the record.

## Concepts explored (`concepts/`, sheet: `concepts.png`)

| File | Idea | Verdict |
|---|---|---|
| `a-helm.svg` | Helm wheel, rim is a platter, playhead notch | **Primary.** The silhouette (round body with eight handles) is unmistakable at 16 px and ownable; one idea, the pun lands without text. |
| `b-palm.svg` | Open hand, palm is a platter, fingertips are fader caps | The pun is literal but the hand reads as a glove or a candelabra at 32 px; busy silhouette. |
| `c-jog-rope.svg` | Jog wheel, waveform through the centre, rope ring | Rich at 1024 px, but at 16 px it is a ring with a line; the nautical cue (rope) is weak. Good alternative for a "pro tools" feel. |
| `d-two-decks.svg` | Two overlapping discs, crossfader below | Clear for DJs, but generic (two discs is the category's default picture) and has no pun. |

Two look-and-refine passes were made at 1024, 128, 32 and 16 px. Findings that changed the design: the first helm had hard
sheen wedges on the vinyl (replaced by soft specular arcs); at 16 px grooves and thin spokes turned to mud, so there is a
**small optical variant** (`*-small.svg`: no grooves, heavier spokes and rim, larger hub and playhead) used for 16 to 48 px;
the rope in C needed overlap to read as rope; the hand in B was re-fanned and centred.

## Files

| File | Use |
|---|---|
| `deckhand-icon.svg` | Master: full-bleed 1024 square, no baked-in mask. The motif stays inside a circle of radius 402 px (of 512), so macOS and iOS masks never clip it. |
| `deckhand-icon-small.svg` | Same, optical variant for 16 to 48 px. |
| `deckhand-mark.svg` | Free-form motif on transparent (viewBox 100 100 824 824). Windows, Linux, in-app. |
| `deckhand-mark-small.svg` | Free-form, optical variant for small sizes. |
| `deckhand-mark-mono.svg` | Single colour (black, alpha only): template images, menu bar, stamps. Tint it any colour. |
| `deckhand-icon-512.png` | Preview of the primary in the macOS tile. |
| `macos-app-icon-rendered-512.png` | The icon as Finder shows the built `DeckHand.app`. |
| `ios/AppIcon.appiconset/` | Drop-in iOS asset (light, dark, tinted; see `ios/README.md`). |
| `favicon.b64` | 32 px PNG, base64, pasted into the overlay page. |

## Colours

| Role | Hex |
|---|---|
| Sea navy (tile) | `#14405E` to `#07121F` (diagonal gradient), teal glow `#2FE0C6` at 16 % |
| Brass | `#FFE0A0` highlight, `#F0A93A` body, `#9C5A17` shadow |
| Teal (label, playhead) | `#2FE0C6`, highlight `#B8FFF2`, deep `#0E8F86` |
| Vinyl | `#233246` to `#0C141E` to `#05090E` |

Brass on deep navy with a teal accent: no red/black, orange/black, blue/white or purple pairing of the usual DJ products.

## Clear space and use

- Keep clear space of one handle width (about 7 % of the mark's diameter) on all sides of the free-form mark.
- Minimum size: 16 px (use the small variant below 49 px); never recolour the brass or the teal notch, except the mono version.
- On light and dark grounds the free-form mark works as is (the brass rim separates the dark vinyl from dark grounds).
- Do not add text, rounded masks or drop shadows to the masters; platforms add their own.

## Regenerate

```
sh tools/make_icons.sh          # every platform asset from the masters (macOS only: swiftc, iconutil, python3 stdlib)
sh tools/make_icons.sh --svg    # first rebuild the masters and concepts from design/icon/src/gen_svg.py
```

It writes `platform/macos/DeckHand.icns`, `platform/windows/resources/deckhand.ico`, `platform/linux/packaging/deckhand.svg`
and `icons/hicolor/*`, `platform/linux/shell/window_icon.hpp`, `ios/AppIcon.appiconset/`, `deckhand-icon-512.png`, `favicon.b64`.
Output is byte-for-byte deterministic. Helpers: `tools/icon_render.swift` (SVG rasteriser, tile, contact sheet) and
`tools/icon_pack.py` (ICO container, C++ header; `ico-check` parses an .ico back). The contact sheet is made with
`icon_render sheet concepts.png "A Helm=concepts/a-helm.svg" ...`.
