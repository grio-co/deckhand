# Library interchange (module `import`)

Take a whole library out of DeckHand, or bring one in, through open files that most DJ software reads and writes:
the **DJ library XML** (the `DJ_PLAYLISTS` document: `COLLECTION/TRACK`, `TEMPO`, `POSITION_MARK`, a `PLAYLISTS` tree
of `NODE`s) and **M3U8** playlists. Written from the publicly documented structure; no code or sample files from other
projects. Interoperability with other applications' XML exports and imports has **not** been tested against those
applications yet; the format is followed as documented.

API: `src/import/include/deckhand/import/interchange.hpp`. Plain option structs in, a stats struct out. Nothing throws:
failures come back as `ok == false` plus `error`. Tests: `tests/test_interchange.cpp`.

## XML: what is carried

Per track: title (`Name`), artist, album, genre, comment (`Comments`), key (`Tonality`), BPM (`AverageBpm`, 2 decimals),
rating (0-5 stars as 0/51/.../255), colour tag (`Colour="0xRRGGBB"`), date added (`DateAdded`, day resolution, UTC),
duration (`TotalTime`, whole seconds), bitrate, play count (`PlayCount`), format (`Kind`, informational), file size
(`Size`, read from disk, informational), location (`Location`, `file://localhost/` URL, UTF-8 percent-encoded, so
spaces, `#`, `%`, `&` and non-ASCII are safe).

Beat grid: every tempo section as `TEMPO` (`Inizio` seconds, `Bpm`, `Metro="4/4"`, `Battito` = beat in bar 1-4).

Cues: hot cues (`Num` = slot), memory cues (`Num="-1"`), loops (`Type="4"` with `End`; `Num` = slot for hot loops, -1
for saved loops), each with `Name` and, when set, `Red/Green/Blue`. Times are seconds with millisecond resolution.

Playlists: the full folder/playlist tree (`Type="0"` folders, `Type="1"` playlists with `KeyType="0"`, tracks by
`TrackID`), in library order, repeats kept. `TrackID`s are renumbered 1..N in library id order.

Output is stable: tracks by library id, playlists in tree order, cues hot-by-slot then by position, fixed attribute
order, two-space indent, LF line endings, UTF-8. Control characters XML cannot hold are dropped; invalid UTF-8 becomes
U+FFFD.

### Not carried (DeckHand data the XML cannot hold)
Grid lock; per-track gain; smart playlists (rules are not static lists); play history detail (only the count survives);
last-played time; year, sample rate and any tag DeckHand does not show; sub-second duration and sub-day add time;
tempo meters other than 4/4; fade-in/fade-out/load marks and cue types other than cue and loop (ignored on import,
counted in `marks_ignored`); slots above 63. `Size` is informational and not used for matching.

## XML: import

`import_library_xml(lib, path, options)` parses and validates the whole file first, then writes in **one transaction**:
a malformed file, or any failure part-way, leaves the library untouched. Unknown elements and attributes are ignored.
Accepted input: UTF-8 (BOM allowed), `'` or `"` quotes, comments, CDATA, a DOCTYPE (never expanded; only the five
predefined entities and numeric references exist), decimal commas in numbers. Rejected with a line number: unclosed or
mismatched tags, unknown entities, unquoted attributes, UTF-16 / other encodings, invalid UTF-8, nesting deeper than
128, a root other than `DJ_PLAYLISTS`.

### Matching
1. Exact file path (after URL decoding).
2. If `match_by_name`: a library track whose **file name** (case-insensitive) is the same, whose duration is within one
   second, whose own file no longer exists, and which is the only such candidate. DeckHand does not store file size,
   so size is not used. A match by name relocates the track to the file's new path when that file exists.
3. Otherwise a new track is added. Files that are not on disk are imported anyway and counted in
   `tracks_missing_file` (first 200 paths in `missing_paths`); the library shows them as missing as usual.

Playlists match by parent + name + kind; a duplicate name inside one file creates a second playlist.

### Options (`InterchangeImportOptions`)
| option | default | meaning |
|---|---|---|
| `merge` | true | false: tracks already present are left completely alone (still usable in playlists) |
| `overwrite_cues` | false | false: only empty hot-cue slots and, if there are none, memory cues are filled |
| `overwrite_grid` | false | false: only tracks without a beat grid get one |
| `overwrite_metadata` | false | false: only empty title/artist/album/genre/comment/key/BPM/rating/colour are filled |
| `match_by_name` | true | step 2 above |
| `import_playlists` | true | |
| `import_play_counts` | false | adds history entries (one session, deck "import") up to the file's count; off by default so history is not padded |
| `dry_run` | false | does the work in the transaction and rolls it back: exact statistics, nothing written |

Merging playlists appends tracks not already listed and never reorders or removes. Importing the same file twice adds
nothing. New tracks take the file's date added; existing ones only with `overwrite_metadata`.

### Round trip
Export, import into an empty library (with `import_play_counts`), export again: byte-identical. Cues, loops, grid
sections and playlist order survive. Without `import_play_counts` the second file lacks `PlayCount`.

## M3U8
`export_playlist_m3u8(lib, id, path, relative_paths)`: `#EXTM3U`, `#PLAYLIST:`, `#EXTINF:<seconds>,<artist - title>`,
then the path (relative paths are relative to the playlist file and use `/`). A folder id exports its playlists
flattened. `export_all_playlists_m3u8(lib, dir, relative_paths)` writes one file per playlist, folders as directories;
names have `/\:*?"<>|` and control characters replaced by `_`, trailing dots/spaces removed, reserved Windows names
prefixed, length capped, collisions (case-insensitive) numbered ` (2)`.

`import_playlist_m3u8(lib, path, options)` reads UTF-8 with or without BOM, CRLF/LF/CR, absolute and relative paths
(resolved against the file, `\` accepted), `file://` URLs and `#EXTINF` (used for tracks it has to add). Stream URLs are
skipped (`playlist_entries_unresolved`). The playlist is named by `options.playlist_name`, else `#PLAYLIST:`, else the
file name. `M3uImportOptions`: `parent`, `merge`, `add_missing_tracks`, `dry_run`; same all-or-nothing transaction.
