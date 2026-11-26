# DeckHand brand

The icon is a ship's helm whose rim is a record, with a playhead notch (a deckhand is a ship's hand; DeckHand is a hand on the
decks). It is original work, released under the project's MIT licence like the rest of the repository. Design notes, the other
concepts and how to regenerate everything: `design/icon/README.md`.

## Files

- `design/icon/deckhand-icon.svg` full-bleed app icon master; `deckhand-mark.svg` free-form mark (transparent);
  `deckhand-mark-mono.svg` single-colour template; `*-small.svg` optical variants for 16 to 48 px.
- Built assets: `platform/macos/DeckHand.icns`, `platform/windows/resources/deckhand.ico`, `platform/linux/packaging/`
  (`deckhand.svg`, `icons/hicolor/`), `design/icon/ios/AppIcon.appiconset/`. All come from `sh tools/make_icons.sh`; do not edit them by hand.

## Colours

Navy `#14405E` to `#07121F`, brass `#F0A93A` (highlight `#FFE0A0`, shadow `#9C5A17`), teal `#2FE0C6`, vinyl `#0C141E`.

## Do

- Use the masters as they are, scaled; use the small variant under 49 px; keep one handle width of clear space.
- Use the mono mark for single-colour contexts (any colour).
- Skins may recolour their own UI freely; the app icon stays as designed.

## Don't

- Don't add text or effects, rotate the wheel, recolour the brass or teal, or put the mark inside another shape.
- Don't combine it with other products' logos or imply endorsement by them.

Forks may use the icon under the MIT licence; if you ship a modified product, rename it and change the icon so users can tell them apart.
