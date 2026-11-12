# Library sync

DeckHand can keep your library the same on several computers (macOS, Windows, Linux, iOS) and on a USB stick.
There is no DeckHand server and no account. Sync works through a folder that you already replicate yourself:
iCloud Drive, Dropbox, OneDrive, Syncthing, a NAS share, or a stick you carry around.

## What syncs

- Track metadata: title, artist, album, genre, label, comment, year, rating, colour, key, BPM, gain, duration,
  bitrate, sample rate, date added.
- Beat grids, hot cues, memory cues and loops.
- Playlists and folders (the tree, names, order, moves), and the tracks in each playlist, in order. The same track
  may appear several times in one playlist.
- Play history.
- A short list of settings: language, skin, analysis and cue preferences, and the stream/overlay text templates.

## What does not sync

- The audio files. Copy your music yourself (the same cloud folder, a NAS, a drive). A track whose file is not on
  this computer shows up as a "missing file" row with all its metadata, cues and playlist positions; it plays as
  soon as the file is there (see "Missing files").
- The library database file. It is never put in the sync folder, because cloud services corrupt live databases.
- Waveform caches (each device recomputes them).
- Anything device specific or secret: audio device and buffer settings, MIDI mappings, the API server settings,
  OBS and streaming credentials, tokens and passwords. A setting whose name contains token, password, secret, api,
  audio, midi, device or path is never synced, whatever the allowlist says.

## Setting up

1. Pick a folder that your cloud service replicates, for example `~/Dropbox/DeckHandSync-root` (any existing
   folder; DeckHand creates `DeckHandSync/v1/` inside it).
2. On every device, give DeckHand that folder and a device name ("Studio Mac", "Laptop").
3. Let each device sync (the app does this in the background). Devices appear in the device list once they have
   written their first file.

USB stick: use a folder on the stick as the sync folder. Plug it in, sync, unplug. If the stick is not present the
sync reports "sync folder is not available" and nothing is changed; DeckHand never creates the folder path for you.

Library on a stick (portable): run DeckHand with the library database on the stick and the music on the stick; no
sync folder is needed for that. To also follow your other computers, point the stick's DeckHand at the cloud folder.

Do not copy a library database file from one computer to another and then sync both: the copy shares the original's
device identity. Start the second computer with an empty library and let sync fill it.

## Missing files

Each track is identified by a content fingerprint (file size plus a hash of three 64 KiB windows of the file), not by
its path, so the same song matches across computers even when it lives at different paths. When another device
tells this one about a track it does not have, the row is created with a placeholder path. It relinks automatically
when:

- the other device's path also exists here with the same content (typical for a shared NAS mount), or
- you run "relink missing files" and point it at the folders that hold your music; files are matched by content, so
  the names and folder layout do not matter.

Re-tagging a file in place (which changes its size) gives it a new identity; DeckHand then sees a new track.

## Conflicts

Everything is merged field by field, and the edit with the later hybrid-logical-clock time wins. The clock combines
each device's wall clock with a counter that moves forward whenever the device reads another device's changes, so
an edit you make after seeing someone else's edit always wins over it, even when your computer's clock is wrong.
Two edits made without seeing each other are decided by clock time; with identical times the device id decides. The
result is the same on every device.

- Different fields of one track edited on two devices: both edits are kept (title from one, genre from the other).
- Same field edited on both: the later one wins on every device.
- Playlists: tracks added on two devices are both kept. Order is by position keys, so a track both devices moved
  is moved once, not duplicated. Two tracks inserted at the same spot stay in the same order everywhere.
- A track removed from a playlist on one device and moved on another: the removal wins.
- Deleting a track, playlist or folder wins over concurrent edits of it. Re-adding the same file later (rescanning)
  brings the track back, and that propagates too. Deleting a folder deletes what is inside it.
- Folders moved into each other on two devices (which would form a loop): the older of the conflicting moves is
  undone, so every device ends with the same loop-free tree.
- Play history is only ever added to; each device's entries are merged in.

## Safety

- Every device writes only inside its own directory, so the cloud service never sees two writers on one file.
- Files are written under a temporary name, flushed to disk and renamed into place. A crash at any point leaves
  either nothing or a complete file, and the next sync picks up where it stopped. Running sync again with nothing
  changed writes nothing.
- Every record and every file carries a checksum. A damaged or half-uploaded file is reported in the sync result
  and its undamaged part is used; when the complete file arrives it replaces the partial result. It never corrupts
  your library.
- Files from a newer DeckHand format are skipped with a warning. Unknown record types are ignored.
- Compaction (`compact()`) replaces a device's log files with one snapshot of its own state, which keeps the folder
  small and lets a new device join quickly. Deletions are kept as small tombstones indefinitely.

Not provided yet: encryption of the folder contents (see the format appendix for the planned hook), garbage
collection of tombstones, and syncing the audio files themselves.

While a sync runs, nothing else may use the library database connection; the caller runs it from one worker and
keeps other threads off the library connection until it returns.

---

# Appendix: format (version 1)

Layout under the sync folder:

```
DeckHandSync/v1/<device-id>/<sequence>.dhlog     append-only change log, immutable once written
DeckHandSync/v1/<device-id>/<sequence>.dhsnap    snapshot of the device's own state (compaction)
```

`device-id` is 16 lowercase hex digits, random, generated once per library. `sequence` is a zero-padded decimal
counter, unique per device and increasing. Only the owning device writes in its directory. Names starting with a dot
and not matching `<digits>.dhlog|.dhsnap` (temporary files) are ignored by readers.

File: 8 bytes, then records.

```
bytes 0-3  "DHLG"
byte  4    format version (1). Readers skip a file with a higher version.
byte  5    file kind: 1 log, 2 snapshot
byte  6    flags: bit 0 = the file may contain encrypted records (see below)
byte  7    reserved (0)
```

Record framing (little endian):

```
u32 length      = 1 + payload size
u8  type
payload
u32 crc32       of type + payload (IEEE, reflected, as zlib)
```

Types: `0x01` header, `0x02` register, `0x20` encrypted batch (reserved), `0xFF` trailer. Any other type is skipped
using its length. A record whose checksum fails, or that is cut off, ends the readable part of the file.

Varints are LEB128; "zigzag" is a signed varint; strings are varint length plus UTF-8 bytes; doubles are 8 bytes
little endian IEEE 754.

Header (first record): `str device_id, str device_name, varint sequence, zigzag created_ms`.

Trailer (last record): `varint record_count (register + skipped), u32 crc32 of every byte before the trailer
record`. A file without a valid trailer is incomplete: its valid records are used, and it is read again whenever its
size changes.

Register record: `u8 kind, str entity_id, str field, zigzag wall_ms, varint counter, str device, u8 value_type,
value` with value types 0 null, 1 integer (zigzag), 2 real (double), 3 text. The stamp (wall_ms, counter, device)
orders registers; the larger stamp wins, so merging is commutative, associative and idempotent. A reader may apply
records in any order and any number of times.

Entity kinds, ids and fields:

| kind | entity | id | fields |
|---|---|---|---|
| T | track | content fingerprint `f1-<size hex>-<hash128 hex>` | `_del`, metadata columns, `grid`, `file_name`, `hint` |
| C | cue | `<fingerprint>#s<slot>` (hot cue or loop on a slot) or `#m@<ms>` / `#l@<ms>` (memory cue / loop) | `_del`, `kind`, `start_ms`, `end_ms`, `label`, `color` |
| P | playlist or folder | `<device>:<counter>` | `_del`, `name`, `is_folder`, `parent` (id or empty), `pos` |
| E | playlist entry | `<device>:<counter>` | `_del`, `pl` (playlist id), `trk` (fingerprint), `pos` |
| H | history row | `<device>:<counter>` | `sess`, `trk`, `deck`, `started`, `played` |
| S | history session | `<device>:<counter>` | `started` |
| G | setting | key | `v` |

`_del` = 1 marks a tombstone; 0 or absent means present. Empty values (NULL, 0, empty text) and absent registers
are equivalent. `pos` is a fractional position key: a string over `0-9A-Za-z` compared bytewise; entries sort by
(`pos`, entity id). Inserting between two neighbours generates a key strictly between theirs. `grid` is the beat
grid as text, `first_beat_ms,bpm,bar_beat;` repeated.

Effective playlist tree: a node's parent is its `parent` register, or the top level when that is empty or unknown.
Cycles are broken by sending the node whose `parent` register has the oldest stamp in the cycle to the top level;
descendants of a deleted node are deleted.

Reading a device directory: use the newest complete snapshot, plus all logs with a higher sequence; logs below
that snapshot's sequence are covered and ignored. A snapshot holds the registers the device itself authored that
are still winning in its view, so the union of all devices' snapshots and logs equals the full state.

Encryption hook: a writer that supports encryption sets flag bit 0 and wraps register records in type `0x20`
records: `u8 cipher_id, str nonce, bytes ciphertext`, the ciphertext being a sequence of ordinary framed records
sealed with an authenticated cipher keyed from a user passphrase (key derivation parameters would go in an
extended header record). This version never writes them and reports and skips them when read; the header, trailer
and checksums stay in the clear so a device without the key can still tell a file is complete.

Local state: the sync module keeps its own tables in the library database (`sync_meta`, `sync_reg`, `sync_map`,
`sync_track`, `sync_files`). `sync_reg` is the merged state and doubles as the "last exported state" the next capture
pass diffs the live tables against.
