# USB export (module `export`)

Writes a stick that CDJ/XDJ-class players can browse, plus a read-back parser, a structural validator and a
reference-export comparison harness used by `tests/test_export.cpp`. Built clean-room from the public DJ Link
Ecosystem Analysis ("Database Exports", "Analysis Files" and the dbserver "Requesting Nxs2 / 3-band waveforms"
sections) and the crate-digger Kaitai descriptions (`rekordbox_pdb.ksy`, `rekordbox_anlz.ksy`). No vendor files or
GPL code were used. **Nothing here has been verified on hardware or against a vendor-made export yet**; every
guess is listed below so the comparison harness can regression-test it the day a reference stick exists.

API: `src/export/include/deckhand/export/export.hpp` (`export_usb`, `read_export`; compatibility check in `export_preflight.hpp`; bool + error string, no
exceptions) and `export_check.hpp` (`validate_export`, `compare_exports`, `sanitize_fs_name`, `fat_path_problems`).

## Layout written

    PIONEER/rekordbox/export.pdb
    PIONEER/rekordbox/exportExt.pdb         (only with ExportOptions::write_ext_db, experimental)
    PIONEER/USBANLZ/P<3 hex>/<8 hex>/ANLZ0000.DAT | .EXT | .2EX
    Contents/<Artist>/<Album>/<file>        (copy_audio)

DB paths use forward slashes, rooted at the stick (`/Contents/...`, `/PIONEER/USBANLZ/...`). With
`copy_audio=false` a source already under the stick root keeps its relative path; otherwise the DB points at the
`Contents/` location anyway (a warning is logged).

### Names and collisions
`sanitize_fs_name` is deterministic: `/\:*?"<>|`, control characters, invalid UTF-8 and non-BMP characters (emoji)
become `_`; leading spaces and trailing dots/spaces are removed; reserved DOS names (CON, PRN, AUX, NUL, COM0-9,
LPT0-9, with or without extension) get a `_` prefix; names are capped in bytes (folders 60, files 255) keeping a
short extension; empty results fall back to "Unknown Artist" / "Unknown Album" / "track". Collisions are resolved
case-insensitively (FAT): ` (2)`, ` (3)` ... on the file name; folders that differ only by case share the spelling
of the first track (by id). The same source file exported by several tracks is copied once. DB path strings are
never truncated (error if a path would exceed 500 bytes).

### Failure behaviour
Everything is staged under `<final>.dhtmp`, then moved into place in one commit phase (audio, ANLZ, `export.pdb`
last). On any failure (free space estimate too small, folder not creatable, write error, rename error) every
staged file is deleted, files that were replaced are restored from `<final>.dhold`, and folders this run created
are removed; nothing is left at a final path. A pre-existing audio file is never overwritten or deleted: if the
destination name exists with different content the new file gets a ` (2)` name, if it is the same audio (size and
first/last 64 KiB) it is reused. Test hooks: `ExportOptions::available_bytes_override`, `fail_after_commits`.

## export.pdb (little-endian, 4096-byte pages)

- Page 0 header: len_page, num_tables = 20, next_unused_page = page count, word 0x10 = 1, sequence = page count + 1,
  table pointers from 0x1C (type, empty_candidate, first_page, last_page). Tables 0..19 all present.
- Per table: one index page (flags 0x64), then data pages (flags 0x24), chained by next_page, then one empty
  "candidate" page. `first_page` = index page, `last_page` = last data page (= index page when empty).
- Index page: after the common 0x20-byte header: 0x1FFF, 0x1FFF, magic 0x03EC, next_offset 0, page_index,
  next_page, 0x03FFFFFF, 0, num_entries 0, first_empty 0x1FFF, then empty entries 0x1FFFFFF8 to the page end.
- Data page: 0x28-byte header, heap from 0x28, row index built backwards from the page end in groups of 16
  (16 x u16 offsets, presence mask at base-4, transaction mask at base-2, stride 36 bytes). Rows are 4-byte
  aligned. Row counts: num_row_offsets in the low 13 bits, num_rows in the next 11, in 3 bytes at 0x18.
- Rows: tracks (subtype 0x24, 21 string offsets), genres, artists (near 0x60 / far 0x64), albums (near 0x80 / far
  0x84; far from 200 name bytes), labels (empty), keys, colors (8 palette rows), playlist tree, playlist entries
  (1-based), artwork (empty), columns, history tables (empty), unknown types 9-12, 14, 15, 17, 18 (empty).
  Track tempo is BPM x 100, duration whole seconds, file_type from the extension (mp3 1, m4a 4, flac 5, wav 0x0B,
  aif 0x0C).
- Strings: short ASCII (<=126 bytes, `(len+1)<<1 | 1`), long ASCII (0x40) and long UTF-16LE (0x90) for non-ASCII.
  Strings are capped at 254 source bytes (file name 255, the two path strings 500).

## exportExt.pdb (experimental, `write_ext_db`, default off)
Same page structure as `export.pdb`. The documentation lists two table types for it: tags (3) and tag tracks (4);
only those two are written (`num_tables = 2`). Rows follow "Tag Rows" / "Tag Track Rows": tag rows have subtype
0x0680 (0x0684 with a far name), `tag_index` = 0x20 x row position, category id (0 for a category row),
`category_pos`, id, `raw_is_category`, 3, name offset, offset of an empty string; tag-track rows are
`0, track_id, tag_id, 3`. DeckHand has no tag data of its own: `ExportTrack::tags` (names) feeds one category (id 1,
named `ExportOptions::tag_category`, default "DeckHand") and one tag row per distinct name (ids from 1 000 000
since the page says tags have "very large" ids). With no tags both tables are empty (valid, but pointless).
Undocumented: whether players need further tables (the Kaitai enum lists types 0-8), the position of the empty
unknown string inside the row (written directly after the fixed part), what rekordbox does with the three other
category ids 2-4, and whether players accept the file at all; hence experimental.

## ANLZ (big-endian)

- DAT: PMAI header, PPTH, PVBR, PQTZ, PWAV (400 cols), PWV2 (100 cols), PCOB memory, PCOB hot.
- EXT: PMAI, PPTH, PWV3 (150/s), PCO2 memory, PCO2 hot, PWV4 (1200 x 6, optional), PWV5 (150/s).
- 2EX (optional, CDJ-3000): PPTH, PWV7 (150/s x 3), PWV6 (1200 x 3).
- Waveform derivation from the {low, mid, high} input: height = round(max band x 31 / 255) (previews take the max
  per band over each column's slice); PWAV/PWV3 byte = whiteness << 5 | height; PWV2 = round(max x 15 / 255);
  PWV5 = R(low) G(mid) B(high) 3 bits each normalised to the loudest band, height in bits 6..2; PWV6/PWV7 bytes are
  mid, high, low as documented; PWV4 entries: [high share, high share, (2 low + mid)/3, low, mid, high].
- **PQT2 (extended beat grid) is not written.** It appears in none of the public sources listed above (not in
  `rekordbox_anlz.ksy`, not on the Analysis Files page), so no layout can be produced without guessing. **PSSI
  (song structure) is not written**: it is documented, but DeckHand has no phrase analysis to feed it. PWVC is
  likewise undocumented. The read-back parser, validator and comparison tolerate unknown tags (they are listed
  in the tag inventory), so a reference file containing them can still be inspected.

## Audit of the numbered uncertainties (against the public documentation, re-checked this round)

Outcome per item: **confirmed** (cited), **corrected** (what changed) or **undocumented** (why, and the choice).
Totals: 7 confirmed, 2 corrected, 11 undocumented.

PDB
1. **Confirmed.** "Table Page Header": "The first 13 bits, num_row_offsets ... The final 11 bits, num_rows"; the Kaitai
   `page` type declares `bit-endian: le` with `b13` then `b11`, so offsets are the low 13 bits.
2. **Corrected.** "Index Pages": the index-specific header follows the *common* header at 0x20 (not at 0x28 as
   written before): unknowna 0x20, unknownb 0x22, magic 0x03EC at 0x24 (the ksy page header independently notes
   "1004 for strange blocks" at 0x24), next_offset 0x26, page_index 0x28, next_page 0x2C, 0x03FFFFFF 0x30, 0 0x34,
   num_entries 0x38, first_empty 0x3A, entries from 0x3C. `num_entries = 0` is confirmed ("entries point to pages
   with deleted rows", "usually empty if there are no deleted rows"). Still a guess: `first_empty = 0x1FFF`
   for an index with no entries at all, and filling the rest of the page with the empty value 0x1FFFFFF8.
3. **Undocumented.** What an empty table's pages look like, the content of the empty-candidate page and the
   next_page of the last data page (the page only says the chain must be stopped at last_page because following
   next_page "will start reading pages of some different table"). Written: first = last = index page when
   empty; last data page -> empty candidate page; candidate: empty data page, next 0x03FFFFFF.
4. **Confirmed.** free_size: "unused space in the page heap (excluding the row index)", used_size: "bytes in
   use in the page heap" (header section). Written free = 4096 - 0x28 - index - used. Whether the vendor counts
   gaps of deleted rows in `used` is irrelevant for a freshly written file.
5. **Undocumented.** `index_shift` ("TODO name from @flesniak, but what does it mean?"); written 0x20 x row position
   (the tag rows' `tag_index` "increases by 0x20 for each row" supports the pattern). Header word 0x10 (written 1) and the
   exact sequence values are unexplained; the documented semantics (page sequence <= database sequence, which
   is the "next" number) hold and are validated. Confirmed pieces: page_flags 0x24 data / 0x64 index ("typically"),
   transaction_row_count = rows touched, transaction_row_index = first touched row (0x1FFF = failed).
6. **Undocumented.** Track row `bitmask` ("what do the bits mean?", written 0x000C0700), the u32 at 0x14 ("some
   ID", 0), `play_count`/`disc_number` (0), unknown strings 3, 4, 8, 9, 13, 18, `message`, `lyricist`, ISRC (empty),
   `publish_track_info`/`autoload_hotcues` (empty = off, documented as "empty or ON"). Confirmed: u3 19048, u4
   30967, u5 0x29, the 3 before the offsets, file_type at 0x5A, string 2 as an ASCII number, YYYY-MM-DD dates.
7. **Confirmed.** Color rows: id at 5, name at 8, ids 1..8 = Pink, Red, Orange, Yellow, Green, Aqua, Blue, Purple
   (Table 3). The five unknown bytes at 0 and the byte at 7 remain guesses (0..0, 1 / 0).
8. **Undocumented.** Columns table (0x10, "Details not yet confirmed"): names, ids and the second u16 are guesses.
9. **Undocumented.** Tables 9, 10, 14, 15, 17, 18 (purpose unknown) are written empty. The prose lists history
   playlists/entries/history as 0x11/0x12/0x13 but the ksy enum numbers them 11/12/19 (and 17/18 unknown); since
   all of 9-19 are written, empty, both numberings are covered. `exportExt.pdb` is now written (see above).
10. **Confirmed.** Playlist tree rows: parent_id 0, unknown u32 4, sort_order 8, id 0x0C, raw_is_folder 0x10, name
    0x14; entries: entry_index, track_id, playlist_id. The unknown u32 is written 0; the base of entry_index
    (written 1-based) is not stated.
11. **Undocumented.** Page placement: tables are laid out sequentially (each table: index page, data, empty
    candidate); the documentation only requires the linked-list structure.
12. **Undocumented.** Rows that do not fit a page. String caps (254 / 255 / 500 bytes) keep every row below
    ~3.5 KiB so this cannot happen; an over-long row is still an error, not split.

ANLZ
13. **Undocumented.** PMAI header bytes 0x0C..0x1B ("we do not yet know the purpose of the header values that
    come after len_file"); written 1, 0x10000, 0x10000, 0. len_header 0x1C is confirmed.
14. **Undocumented.** PVBR body: len_header 0x10, one unknown u32, 400 u32 entries are confirmed (ksy); the entry
    meaning is not. Written: linear byte offsets from the file size (exact for constant bit rate).
15. **Undocumented.** PCOB: layout confirmed (header 0x18, lencues at 0x12, entries 0x38 long with PCPT header
    0x1C, hot_cue, status, 0x00100000, order_first/last, type 1/2, 0x0003E8, time, loop_time). Unexplained:
    `memory_count` (written = cue count), `status` (prose: 0 regular / 4 active loop; ksy enum 0 disabled, 1
    enabled, 4 active loop; written 1), loop_time of a non-loop (written 0xFFFFFFFF), bytes 0x28..0x37 (zero).
    order_first/order_last follow the quoted sequence literally (single cue: 0xFFFF / 0xFFFF).
16. **Confirmed.** PCO2/PCP2: header 0x14 / 0x10, type 0x10, 0x0003E8, time 0x14, loop_time 0x18, color_id 0x1C,
    seven bytes 0x1D (first = 1), loop numerator 0x24 / denominator 0x26 (0 = not quantised), len_comment 0x28
    (UTF-16BE with trailing NUL), then colour code and R, G, B ("0 = default green"). Not documented and kept
    conservative: the palette code table (code 0 + RGB), 8 zero pad bytes after the colour (partial entries are
    explicitly allowed), comment cap of 120 characters.
17. **Corrected.** PWV4 entries were `0,0,low,mid,high,peak`. The protocol analysis ("Requesting Nxs2 Track
    Waveforms") documents six bytes per column: two "whiteness" bytes, energy of the lower half of the frequency
    range, lower third, middle, top. Now written `[high share, high share, (2 low + mid)/3, low, mid, high]`; how
    the vendor derives the first three is not documented, so those are still choices. Header u32 0x14 (0) unknown.
18. **Confirmed.** PWAV/PWV3 byte (5-bit height, 3-bit whiteness), PWV2 (4-bit height), PWV5 bit layout (3 bits red,
    green, blue from the top, then 5 bits of height). Which band feeds which colour channel and the whiteness
    mapping are visual choices, not format questions.
19. **Confirmed.** Both PWV6 (header 0x14, no extra word) and PWV7 (header 0x18, unknown 0x00960000) are requested
    from the `.2EX` file in the protocol analysis, so both are written there. PQT2 / PSSI: see above (omitted).
20. **Undocumented.** The `P<3 hex>/<8 hex>` folder naming is not described in the sources; only the path stored
    in track string 14 matters to a player. Written: hash-like value derived from the track id, unique per id.

## Compatibility check (`export_preflight.hpp`, `format_probe.hpp`)
Players read fewer formats than a computer does. The classic trap is 32-bit float WAV: it plays everywhere on a
computer and silently fails to load in the club. `preflight_export(tracks, profile)` finds such files **before**
the stick is taken anywhere; `export_usb(..., ExportResult&)` carries the same report back with every export.

- **Probe** (`probe_format(path)`): dependency-free header sniffer, bounded reads, never reads a whole file and
  never throws. Returns container, codec, sample rate, bit depth, float-vs-integer, channels, MP3 bitrate/VBR.
  Covers WAV (fmt chunk, WAVE_FORMAT_EXTENSIBLE sub-format GUIDs, IEEE float, RF64/BW64), AIFF/AIFC (COMM, `NONE`,
  `twos`, `sowt`, `fl32`/`fl64`, `in24`/`in32`), FLAC (STREAMINFO), MP3 (ID3v2 skipped, first frame confirmed by
  the next, Xing/Info/VBRI), MP4/M4A (atoms down to `stsd`: AAC vs ALAC, ALAC depth and rate from its cookie,
  FairPlay `sinf`/`drms`/`M4P ` brand = copy-protected), Ogg (Opus/Vorbis), ASF/WMA. Anything else, truncated or
  hostile input comes back `recognized=false` (or `truncated=true`); the tests cut every format at many offsets.
- **Rule table** (`player_profiles()`, one table in `export_preflight.cpp`, data only): profiles `club` (current
  generation, default) and `club_legacy` (FLAC/ALAC refused). The first matching rule decides; each rule has a
  stable id, a plain-language message and a source flag. **`documented`** = the project's list of known-solid
  rules; **`unverified`** = a conservative guess. **None of the table was checked against hardware. Before
  trusting it, check every rule against the manual of the player you will use, and edit the table.**

  | id | verdict | source |
  |---|---|---|
  | `pcm.float` (32/64-bit float WAV/AIFF) | will not play | documented |
  | `pcm.int32` (32-bit integer WAV/AIFF) | will not play | documented |
  | `pcm.ok` (16/24-bit integer, 44.1/48/88.2/96 kHz) | ok | documented |
  | `mp3.ok`, `aac.ok` (44.1/48 kHz) | ok | documented |
  | `flac.ok`, `alac.ok` (<= 24-bit, <= 96 kHz) | warning: current players read it, older ones cannot (`club_legacy`: will not play) | documented |
  | `opus.unsupported`, `vorbis.unsupported`, `ogg.unsupported`, `wma.unsupported` | will not play | documented |
  | `drm.protected` (`.m4p`, FairPlay) | will not play | documented |
  | `pcm.rate` (16/24-bit at another rate, e.g. 192 kHz), `pcm.bits` (8-bit), `pcm.little_endian_aiff`, `mp3.rate`, `mp3.layer` (layer I/II), `aac.rate` | warning | unverified |
  | `pcm.codec` (ADPCM, A-law, ... in WAV/AIFF), `flac.depth|rate`, `alac.depth|rate` (> 24-bit or > 96 kHz), `format.unsupported` | will not play | unverified |

  Stick-level checks (not in the table): `source.missing`, `source.empty`, `file.too_large` (> 4 GB on FAT32),
  `path.too_long` (database path > 500 bytes), `name.changed` (characters FAT32/exFAT cannot store: renamed on
  the stick), `file.unrecognized` / `file.damaged`, `ext.mismatch` (named .mp3 but contains WAV: players choose
  the decoder by name).
- **Report**: per track `ok` / `warning` / `will_not_play`, the issues with reason id, message and source flag, and
  `fixable`. `PreflightReport::text()` and `to_json()` render it. The export itself never refuses because of the
  report: it proceeds and returns the report in `ExportResult`.
- **Fix** (`ExportOptions::convert_incompatible`, default off): a fixable file is written to the stick as a 24-bit
  integer PCM WAV (`<name>.wav`, clamped, no dithering, sample rate kept). The user's source is only ever read.
  The database row describes the converted file (name, size, file type WAV, bitrate = rate x 24 x channels,
  sample depth 24), the ANLZ path and `validate_export` agree with it. A converted file is never reused from a
  previous export; a name clash gets a ` (2)` suffix as usual.
  **Limits**: this module has no decoder (it must not depend on `audio`), so the conversion is done straight from
  the parsed header, as pure byte conversion, and only covers 32-bit integer and 32/64-bit float **WAV/AIFF/AIFF-C**
  (mono or stereo, at 44.1/48/88.2/96 kHz). Files at other rates would need resampling, which is out of scope: they
  stay `will_not_play` and the message says so. FLAC, ALAC, Ogg and the rest are not converted; re-encode them in an
  audio editor. Converted files carry no tags (the database holds the metadata) and are limited to < 4 GB.
- Tests: `tests/test_export_preflight.cpp`.

## Validator: `validate_export(root, report, options)`
Reads the stick only. Errors (stable codes in `ValidationReport::issues`): `pdb.header`, `pdb.table_range`,
`pdb.chain_loop|chain_shared|chain_last|next_range|page_index|page_type`, `pdb.index_page_flag|data_page_flag`,
`pdb.page_seq`, `pdb.row_count` (num_rows vs presence bits), `pdb.presence_mask` (bit beyond num_row_offsets),
`pdb.index_overflow`, `pdb.row_bounds`, `pdb.row_overlap`, `pdb.row_bad`, `pdb.used_size`, `pdb.free_size`,
`dup.id`, foreign keys `fk.track_artist|album|genre|key|label|color|artwork|composer|original_artist|remixer`,
`fk.album_artist`, `fk.playlist_parent|playlist_cycle`, `fk.entry_track|entry_playlist`, `dup.entry_index`,
`fk.tag_category|tagtrack_track|tagtrack_tag`; per track `anlz.missing_DAT|missing_EXT` (2EX optional),
`anlz.structure` (tag lengths walk the file exactly, len_file matches), `anlz.tag_missing`, `anlz.ppth`
(PPTH equals the track's file path in every file), `anlz.beatgrid_length|order|bar` (strictly increasing times,
beat numbers cycle 1-4), `anlz.cue_entry|cue_range|loop_range|hot_slot`, `anlz.waveform_size`, `anlz.dup_path`;
paths `fat.path` (component > 255 UTF-16 units, forbidden characters, trailing dot/space, reserved names, empty
components, > 500 bytes), `fat.case_collision` (paths or folders differing only by case), `path.absolute`,
`path.file_name`; `audio.missing|size` (skip with `ValidateOptions::check_audio=false`); `export.leftover`
(`.dhtmp`/`.dhold`). Warnings: `pdb.row_align`, `pdb.orphan_page`, `pdb.next_unused`, `anlz.orphan`,
`anlz.beatgrid_range|tempo`, `anlz.waveform_length`. `tests/test_export.cpp` validates every export it writes and
corrupts a written export in 30 targeted ways, asserting each one is flagged.

## Reference-export comparison: `compare_exports(reference, ours, report, error)`
Parses both sticks with the low-level readers, matches tracks by audio file name (title as fallback) and reports
(`CompareReport::text()`): structure (file header words, table order and page order, per table: pages, rows,
rows per page, page flag/u14/u24/u26 value sets, free/used consistency, transaction fields, index page header
bytes, what the last page's next_page points at, row alignment), file inventory by shape (hex runs collapsed),
field-level differences for each matched track (every track-row field including unknown ones, strings with their
encoding kind), the other tables' rows keyed by name/path (unknown fields included), per-track ANLZ tag
inventories (tag order, len_header, header bytes, PCOB/PCO2/PQTZ payload heads, tags present on one side only
incl. PQT2/PSSI, PMAI header words; aggregated by message), and a histogram of the values the reference uses for
each field DeckHand guesses next to what DeckHand writes.

### How to produce the reference stick (owner, once)
1. In rekordbox create a new playlist "DeckHand reference" with 5-10 tracks that are also in the DeckHand library
   (so the audio is identical): include **one track with hot cues A-C (with one named and one coloured)**, **one with
   several memory cues and a memory loop plus one saved loop**, **one with a non-ASCII title and artist (for example
   Japanese or accented)**, one MP3, one FLAC or WAV, and one with a key, genre, rating and colour label set. Run
   analysis on all of them (beat grid, waveform).
2. Format an **empty** USB stick as FAT32 (exFAT as a second variant if you like) and export only that playlist
   to it with rekordbox ("Export" with the stick as destination; leave the other sync options at defaults).
   Optionally also a copy with "My Tag" assigned to one track, to produce `exportExt.pdb` content.
3. Copy the whole stick contents to a folder (e.g. `~/reference-stick`). Do not edit anything.
4. In DeckHand export the same 5-10 tracks as one playlist to another empty folder or stick
   (`~/deckhand-stick`; the same file names matter: tracks are matched by audio file name).
5. Run the comparison (a few lines of C++ against the library, or from a unit test):
   `compare_exports("~/reference-stick", "~/deckhand-stick", report, error)` then print `report.text()`; and run
   `validate_export` on both (the reference is expected to produce a few warnings only). The histogram section
   lists, per guessed field, every value the reference uses; update the writer's constants from it and add the
   reference's tag inventories to the tests.

## Robustness results
20 000 synthetic tracks (8 beats, one cue, 30 s each, no waveform data supplied so 4 500 waveform samples are
synthesised per track) and 500 playlists (20 folders, 480 playlists of 60 tracks): measured on the dev Mac: export 27 s (about 0.6 GB of
ANLZ files, 60 000 files), validator 20 s, read-back 51 s, peak RSS 151 MB for the whole test process (the test also
holds the 20 000-track input); all tables validate. The test prints the line "20000 tracks / 500 playlists". Memory is bounded by the row data
of the database tables (tens of MiB) because ANLZ files are built and staged one track at a time and the database
pages are streamed to disk one page at a time; the track list given by the caller is the dominant allocation.

Reader: `read_export` parses what the writer emits plus UTF-8 (`0x20`) strings and ISRC strings (kind 0x90 whose
body starts with 03 in the first string slot) and `exportExt.pdb`; it is not a general parser for arbitrary vendor
exports (use `compare_exports` / `validate_export` for those).
