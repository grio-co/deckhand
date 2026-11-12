# USB export (module `export`)

Writes a stick that CDJ/XDJ-class players can browse, plus a read-back parser used by `tests/test_export.cpp`.
Built clean-room from the public DJ Link Ecosystem Analysis ("rekordbox export" and "ANLZ files" pages) and the
crate-digger Kaitai description. No vendor files or GPL code were used. **Nothing here has been verified on
hardware or against a reference export yet**; every guess is listed below so it can be regression-tested.

API: `src/export/include/deckhand/export/export.hpp` (`export_usb`, `read_export`; bool + error string).

## Layout written

    PIONEER/rekordbox/export.pdb
    PIONEER/USBANLZ/P<3 hex>/<8 hex>/ANLZ0000.DAT | .EXT | .2EX
    Contents/<Artist>/<Album>/<file>        (copy_audio)

DB paths use forward slashes, rooted at the stick (`/Contents/...`, `/PIONEER/USBANLZ/...`). With
`copy_audio=false` a source already under the stick root keeps its relative path; otherwise the DB points at the
`Contents/` location anyway (a warning is logged). Artist/album directory names are sanitised (`/\:*?"<>|` and
control chars become `_`, 100 byte cap); name clashes get ` (2)`, ` (3)`.

## export.pdb (little-endian, 4096-byte pages)

- Page 0 header: len_page, num_tables = 20, next_unused_page = page count, sequence, table pointers from 0x1C
  (type, empty_candidate, first_page, last_page). Tables ids 0..19 all present.
- Per table: one index page (flags 0x64), then data pages (flags 0x24), chained by next_page, then one empty
  "candidate" page. `first_page` = index page, `last_page` = last data page.
- Data page: 0x28-byte header, heap from 0x28, row index built backwards from the page end in groups of 16
  (16 x u16 offsets, presence mask at base-4, transaction mask at base-2, group stride 36 bytes).
  Rows are 4-byte aligned. Row counts packed as num_row_offsets (13 bit) | num_rows << 13 in 3 bytes at 0x18.
- Rows: tracks (type 0, subtype 0x24, 21 string offsets), genres, artists (near 0x60 / far 0x64), albums (near
  0x80 / far 0x84), labels (empty), keys, colors (8 palette rows), playlist tree, playlist entries, artwork
  (empty), columns, history tables (empty), unknown types 9-12, 14, 15 (empty). Track tempo is BPM x 100,
  duration whole seconds, file_type from extension (mp3 1, m4a 4, flac 5, wav 0x0B, aif 0x0C).
- Strings: short ASCII (<=126 bytes, `(len+1)<<1 | 1`), long ASCII (0x40) and long UTF-16LE (0x90) for non-ASCII.
  Strings are capped at 254 source bytes (400 for the two path strings); artist/album rows switch to the far
  variant when the name offset would not fit one byte.

## ANLZ (big-endian)

- DAT: PMAI header, PPTH, PVBR, PQTZ, PWAV (400 cols), PWV2 (100 cols), PCOB memory, PCOB hot.
- EXT: PMAI, PPTH, PWV3 (150/s), PCO2 memory, PCO2 hot, PWV4 (1200 x 6, optional), PWV5 (150/s).
- 2EX (optional, CDJ-3000): PPTH, PWV7 (150/s x 3), PWV6 (1200 x 3).
- Waveform derivation from the {low, mid, high} input: height = round(max band x 31 / 255) (previews take the max
  per band over each column's slice); PWAV/PWV3 byte = whiteness << 5 | height; PWV2 = round(max x 15 / 255);
  PWV5 = R(low) G(mid) B(high) 3 bits each normalised to the loudest band, height in bits 6..2.
  PWV6/PWV7 bytes are mid, high, low as documented.

## Undocumented, guessed or ambiguous (please test against a reference export and hardware)

PDB
1. Bit order of the 24-bit row-count field (written little-endian: offsets low 13 bits, rows high 11 bits).
   The source describes 13/11 bits without stating the order.
2. Index pages: where the index header sits (written at 0x28), its field values, and that `num_entries` is 0.
   The source says entries point at pages with deleted rows; real exports may list all data pages.
3. Empty-table handling: first_page = last_page = index page; the empty-candidate page content and the
   next_page of the last data page (written = empty_candidate page; the candidate's own next = 0x03FFFFFF).
4. `free_size`/`used_size` semantics (written: used = heap bytes, free = page - 0x28 - index - used).
5. `index_shift` = 0x20 x position in page for tracks/artists/albums; transaction fields (tranrc = row count,
   tranri = 0, tranrf = presence mask); page flags 0x24 for data pages; seq numbers; header words at 0x10/0x14.
6. Track row: bitmask 0x000C0700, u2 = 0, u3/u4 = 19048/30967, u5 = 0x29, disc number 0, play count 0,
   composer/remixer/original artist/label ids 0, artwork id 0. String 2 = "1"; date added/analyze date = export
   date; all other undocumented strings empty. `autoload_hotcues` / `publish_track_info` not set.
7. Color row unknown fields (unknown1 = 0, unknown2 = 1), colour names/ids 1..8 per the page.
8. **Columns table (0x10) contents are a guess** (27 names, ids 1..27, second u16 = 0x80+n, U+FFFA/U+FFFB
   wrappers on the first four). The page marks this table unconfirmed.
9. Tables 9-12, 14, 15 and 19 are written empty; their purpose is unknown. `exportExt.pdb` (tags) is not written.
10. Playlist tree: `unknown` field written 0; sort_order = position among siblings; ids come from the caller.
11. Page placement: tables are laid out sequentially, not interleaved as a vendor tool would.
12. Rows larger than a page (extreme string lengths) are an error, not split.

ANLZ
13. PMAI header words 0x0C..0x1B (written 1, 0x10000, 0x10000, 0).
14. PVBR body: 400 u32 seek offsets estimated linearly from file size; real layout undocumented.
15. PCOB: `memory_count` written = cue count; `status` written 1 (source says 0 / 4); order_first/order_last
    follow the quoted sequence literally (single cue gets 0xFFFF/0xFFFF); loop_time of a point = 0xFFFFFFFF;
    bytes 0x28..0x37 of PCPT zero.
16. PCO2: entries padded to 0x38 + comment length (8 extra zero bytes after the colour bytes); hot-cue colour
    code written 0 with the RGB bytes; the palette code table is not available; loop quantisation fields 0;
    memory points carry `color_id` at 0x1C, hot cues write 0 there; comments capped at 120 UTF-16 units.
17. **PWV4 entry layout is not documented** (written 0,0,low,mid,high,peak). Disable with
    `write_color_preview=false` if the player misrenders. PWV4 header unknown word written 0.
18. Whiteness mapping (share of the high band) and the colour mapping for PWV5 are choices, not spec.
19. `.2EX` with PWV6/PWV7: the page is ambiguous about which file holds PWV7; both are put in `.2EX`.
    Not written: PQT2 (extended beat grid), PSSI (phrases), PWVC, PCO2 variants beyond the above.
20. USBANLZ directory name is a hash-like value derived from the track id, not from the vendor hash.

Reader: `read_export` parses what the writer emits plus tolerates partial PCP2 entries; it is not a general
parser for arbitrary vendor exports (e.g. UTF-8 `0x20` strings, ISRC strings, exportExt.pdb are not handled).
