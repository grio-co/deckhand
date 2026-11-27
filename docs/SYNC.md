# Library sync

DeckHand can keep your library the same on several computers (macOS, Windows, Linux, iOS) and on a USB stick.
There is no DeckHand server and no account. Sync works through a folder that you already replicate yourself:
iCloud Drive, Dropbox, OneDrive, Syncthing, a NAS share, or a stick you carry around.

## What syncs

- Track metadata: title, artist, album, genre, label, comment, year, rating, colour, key, BPM, gain, duration,
  bitrate, sample rate, date added.
- Grid locks.
- Beat grids, hot cues, memory cues and loops.
- Playlists and folders (the tree, names, order, moves), and the tracks in each playlist, in order. The same track
  may appear several times in one playlist.
- Smart playlists: name, order, match mode and rules. A rule that names a playlist follows that playlist across devices.
- Linked mix points (a position in one track tied to a position in another), with their transition length, type and
  label. See "Linked mix points".
- Analysis data, optionally: phrases, vocal regions and the analysis cue points (first sound, last sound, load cue).
  See "Analysis data".
- Play history.
- A short list of settings: language, skin, browser columns, column widths and key notation, analysis and cue
  preferences, and the stream/overlay text templates.

## What does not sync

- The audio files, unless you choose playlists whose audio should travel (see "Audio files"). Otherwise copy your
  music yourself (the same cloud folder, a NAS, a drive). A track whose file is not on
  this computer shows up as a "missing file" row with all its metadata, cues and playlist positions; it plays as
  soon as the file is there (see "Missing files").
- The library database file. It is never put in the sync folder, because cloud services corrupt live databases.
- Waveform caches (each device recomputes them; the library does not store waveform data).
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

## Encryption

Enter the same passphrase on every device (Settings, Library sync). DeckHand then encrypts what it writes into the
folder and accepts only encrypted files from other devices.

- Turning it on replaces the files that device had written. Other devices that were already syncing stop with "this
  sync folder is encrypted" until they get the passphrase, and write nothing meanwhile.
- A passphrase that does not open the folder is rejected the first time, before anything is written.
- Changing it: enter the new one on every device. Until then, devices skip each other's files and say so; nothing
  is lost. Clearing it everywhere writes plain files again.
- There is no recovery. The passphrase only protects the folder: forgetting it costs nothing on a device that has
  the library, so set a new one on every device.
- Protected: titles, cues, playlists, history, settings and file paths. Visible to whoever can read the folder:
  device names, random device ids, file sizes and times.
- The passphrase is stored in each device's local library database. It is never synced and the API cannot read it.
- The cloud provider may keep earlier, unencrypted versions of files in its own history or trash.
- Builds without OpenSSL (currently iOS) cannot use or read an encrypted folder.

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

## Linked mix points

A link is "when track A reaches this position, track B starts from that position". It joins two tracks, so each end is
named by the track's content fingerprint (the same identity as everything else here), never by a local row number.

- Each link has its own id (`<device>:<counter>`, entity `L`). Its fields (positions, transition length, type, label,
  creation time) merge like any other: the later edit wins per field, so a position changed on one computer and the
  label changed on another both survive.
- Deleting a link writes a tombstone. A deleted link is never brought back by an older edit, by a device that joins
  later, or by a compaction. Tombstones follow the same retention rules as playlists.
- A link whose track is not in this library yet is kept in the sync data as pending and is written into the library by
  the first sync after that track exists (placeholders for missing files count as existing). `SyncResult::links_pending`
  says how many wait. Two links with the same ends and positions (within 1 ms) made independently on two devices become
  one; the lower id survives.
- Positions that follow a grid edit (the library moves them with the grid) are ordinary edits and sync as such.
- Setting: `SyncConfig::sync_mix_links` (default on).

## Analysis data

With `SyncConfig::sync_analysis` on, a device sends the phrase analysis (sections and per-bar energy), the vocal regions
and the analysis cue points of each track, keyed by the track's fingerprint (entity `A`, fields `phr`, `voc`, `cue`),
and applies what other devices sent, so a second computer does not analyse again.

- Each item starts with the algorithm version that made it. A device never replaces data with an older version, and
  never sends an older version over a newer one: if a computer with a fast clock writes version 1 after another wrote
  version 2, the version 2 owner sends it again and wins. With equal versions the later write wins.
- Beat grid, key, BPM and gain (the auto-gain / loudness result) already travel with the track metadata (see
  "Conflicts") whatever this switch says; they are not duplicated here. A locked grid is edited only by the user,
  and that edit syncs like any other (later wins); phrase data is not applied over a locked grid it does not match.
- Switching it off sends and applies nothing, but what other devices wrote is still kept: switching it on later
  applies it at the next sync. The library stores no waveform data, so there is none to sync.
- Size: measured with synthetic four-minute tracks (128 bars, 12 sections, 8 vocal regions, cue points), the folder
  grows by about 1.9 KB per track (a track's other sync data is about 0.8 KB). Bar start times are stored as
  run-length coded offsets and the per-bar energy in thousandths. For a 20,000 track library that is about 38 MB, so
  the setting is off by default.

## Audio files

Optional, per playlist, and off until you choose playlists (`SyncConfig::audio_playlists`) and call
`SyncEngine::sync_audio()` after a sync. A folder in a chosen playlist counts with everything inside.

```
<folder>/DeckHandSync/audio/<first two hex digits>/<hash>.dhau
```

- Publishing: for each track of the chosen playlists whose file is on this computer, the whole file is hashed (128 bit,
  not cryptographic: it detects damage, not forgery) and copied into the store under that hash. The same audio under
  several tracks or from several devices is stored once. An object is written under a temporary name, `.<hash>.<device>.tmp`,
  and renamed, so another device sees all of it or none; if the file changes while it is copied, the copy is dropped.
  The track gets a register `audio` = `<hash>:<size>`, which is how other devices find it. Nothing here deletes
  objects: the store only grows (delete the folder to start over).
- Importing: with `SyncConfig::music_folder` set, each track that is a missing-file placeholder here and has an
  `audio` register is looked for in the music folder first (same size and fingerprint, any name: the track is
  relinked, nothing is copied), then fetched from the store. A fetched file is verified against its hash and size
  while it is written to `.dhimport-<hash>.part` in the music folder, then renamed to the track's file name. A
  different file with that name is never overwritten: the new one gets `name (<8 hex digits>).ext`. The track is then
  relinked, so its cues, links and playlist places stay.
- Resuming: run it as often as you like. Finished files are skipped; a cut-off copy (a crash, or an object still being
  uploaded by the cloud service) is redone or reported as waiting. Leftovers of this device are removed at the start.
- Budgets: `audio_budget_bytes` caps the store in the sync folder (the check uses what is already there, from every
  device); `audio_import_budget_bytes` caps what one call copies into the music folder. What does not fit is left for a
  later call and counted as `waiting_budget`.
- Report (`AudioReport`): `exported` and `exported_bytes` (copied into the folder), `already_stored`, `imported` and
  `imported_bytes`, `relinked`, `skipped` (not possible: file missing here, unreadable, changed while copying),
  `waiting_budget`, `waiting_files` (not arrived or incomplete yet), `failed` (damaged or not openable, never used),
  `store_bytes`, `warnings`.
- Encryption: with a passphrase, every object is encrypted with AES-256-GCM in 1 MiB chunks, using the folder's key
  and a random nonce per chunk, and the order and last chunk are authenticated. The file's hash name is still visible
  to whoever can read the folder (it is a hash of the audio, so it shows which files exist and how big they are, not
  what they sound like). A device that requires encryption does not accept a plain object. A build without a crypto
  library cannot encrypt: `sync_audio()` then refuses unless `allow_unencrypted_audio` is on. With that setting on,
  audio is stored and accepted unencrypted even when a passphrase is set (the logs stay encrypted).
- `sync_audio()` needs one normal sync first when a passphrase is set (the folder's key is created there).

## Safety

- Every device writes only inside its own directory, so the cloud service never sees two writers on one file.
- Files are written under a temporary name, flushed to disk and renamed into place. A crash at any point leaves
  either nothing or a complete file, and the next sync picks up where it stopped. Running sync again with nothing
  changed writes nothing.
- Every record and every file carries a checksum. A damaged or half-uploaded file is reported in the sync result
  and its undamaged part is used; when the complete file arrives it replaces the partial result. It never corrupts
  your library.
- Files from a newer DeckHand format are skipped with a warning. Unknown record types are ignored.
- Compaction replaces a device's log files with one snapshot of its own state, which keeps the folder small and
  lets a new device join quickly. It happens by itself once a device has more than 64 files. It also drops the
  details of anything deleted more than 180 days earlier, in that device's snapshot and database. The deletion
  itself (one small tombstone) is kept indefinitely, so a device that was away for a long time still learns of it.

Restoring a library backup: at the next sync DeckHand reads back what that library had written since the backup,
then the other devices' changes. A restore therefore does not undo changes that already synced; edits made after the
restore win. If encryption was set up or changed after the backup, enter the current passphrase first. A backup from
before sync was set up joins as a new device, and its playlists appear next to the synced ones.

Not provided: removing tombstones altogether (that needs every device to confirm it has seen the deletion), and
removing audio files from the store again (see "Audio files").

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
| T | track | content fingerprint `f1-<size hex>-<hash128 hex>` | `_del`, metadata columns, `grid`, `file_name`, `hint`, `audio` (`<hash>:<size>`) |
| C | cue | `<fingerprint>#s<slot>` (hot cue or loop on a slot) or `#m@<ms>` / `#l@<ms>` (memory cue / loop) | `_del`, `kind`, `start_ms`, `end_ms`, `label`, `color` |
| P | playlist or folder | `<device>:<counter>` | `_del`, `name`, `is_folder`, `parent` (id or empty), `pos` |
| E | playlist entry | `<device>:<counter>` | `_del`, `pl` (playlist id), `trk` (fingerprint), `pos` |
| H | history row | `<device>:<counter>` | `sess`, `trk`, `deck`, `started`, `played` |
| S | history session | `<device>:<counter>` | `started` |
| G | setting | key | `v` |
| Q | smart playlist | `<device>:<counter>` | `_del`, `name`, `match_all`, `rules`, `pos` |
| L | linked mix point | `<device>:<counter>` | `_del`, `from`, `to` (track fingerprints), `from_ms`, `to_ms`, `beats`, `type`, `label`, `created` |
| A | track analysis | content fingerprint | `phr`, `voc`, `cue` (text, first line `v<N>`; see the codec in `analysis_codec.cpp`) |

`_del` = 1 marks a tombstone; 0 or absent means present. Empty values (NULL, 0, empty text) and absent registers
are equivalent. `pos` is a fractional position key: a string over `0-9A-Za-z` compared bytewise; entries sort by
(`pos`, entity id). Inserting between two neighbours generates a key strictly between theirs. `grid` is the beat
grid as text, `first_beat_ms,bpm,bar_beat;` repeated. `rules` is the library's smart-rule JSON, except that an
"in playlist" rule has `id` 0 and carries the playlist's entity id in `t`. A reader that cannot parse it keeps its own
rules and does not overwrite the register. Readers ignore entity kinds they do not know.

Effective playlist tree: a node's parent is its `parent` register, or the top level when that is empty or unknown.
Cycles are broken by sending the node whose `parent` register has the oldest stamp in the cycle to the top level;
descendants of a deleted node are deleted.

Reading a device directory: use the newest complete snapshot, plus all logs with a higher sequence; logs below
that snapshot's sequence are covered and ignored. A snapshot holds the registers the device itself authored that
are still winning in its view, so the union of all devices' snapshots and logs equals the full state.

Encrypted files set flag bit 0. Their header record carries one more field, `str salt` (16 random bytes), and their
registers are in `0x20` records instead of `0x02`: `u8 cipher_id (1 = AES-256-GCM), u8 last, str nonce (12 bytes),
ciphertext followed by the 16 byte tag`. The plaintext is a sequence of ordinary framed register records, about 1 MiB
per `0x20` record; the final one has `last` = 1 and is present even when empty. The key is scrypt(passphrase, salt,
N = 32768, r = 8, p = 1, 32 bytes). Authenticated data: the 8 file bytes, the header payload, the record's index in
the file (u32, from 0) and the `last` byte. Plain register records in an encrypted file are ignored. Headers,
trailers and checksums stay in the clear, so a device without the key can still tell a file is complete; it does not
mark such a file as read. A device joins the salt of encrypted files already in the folder that its passphrase opens.
The trailer's record count is the number of records between header and trailer.

Pruned tombstones: compaction keeps, for an entity deleted before the retention period, only `_del` (plus `parent`
for a playlist and `pl` for an entry). A writer that brings an entity back therefore writes all its fields again.

Local state: the sync module keeps its own tables in the library database (`sync_meta`, `sync_reg`, `sync_map`,
`sync_track`, `sync_files`, `sync_ana`). `sync_reg` is the merged state and doubles as the "last exported state" the next capture
pass diffs the live tables against.

Audio object (`.dhau`): 16 fixed bytes `"DHAU"`, version 1, flags (bit 0 = encrypted), 2 reserved, u64 plain size; encrypted
objects continue with a 16 byte salt. Plain: the file bytes follow. Encrypted: chunks of `u32 plain length, 12 byte nonce,
ciphertext + 16 byte tag`, at most 1 MiB of plaintext each (an empty file is one empty chunk); the authenticated data is the
header bytes, the chunk index (u32) and a `last` byte.

## Checking two computers

`test_sync` can check encrypted sync between two real machines with throwaway data. Copy the folder between the
steps however you like; it uses a fixed test passphrase and temporary libraries, not yours.

    computer 1:  test_sync --interop write  FOLDER
    computer 2:  test_sync --interop reply  FOLDER
    computer 1:  test_sync --interop verify FOLDER


# Sharing one playlist with another person

Library sync above keeps *your* devices identical: playlists, folders and their entries already sync as shared
objects between devices of the same library (entities `P` and `E`, tracks named by content fingerprint). It does not
share anything with a different person's library, which has its own tracks, ids and folders. A shared playlist does,
for one playlist at a time.

Where it lives: `src/sync/shared_playlist.cpp` (`deckhand::sync::share_playlist`, `join_shared_playlist`,
`sync_shared_playlist`, `shared_playlist_entries`), tested with two libraries and a temp folder in
`tests/test_shared_playlist.cpp`. It is not wired to a screen or the API yet.

## How it works

```
<folder>/DeckHandShared/<shared id>/<participant id>.dhsp
```

Every participant writes only their own file (a temporary file, then a rename), and reads everyone's. A file is
UTF-8 text: the magic line `DHSP1`, `playlist`, `participant` (id and label), `name` (with its stamp), one `e` line
per entry the participant authored or changed, and a final `end` line. A file without `end` was cut off and is
ignored until it is whole.

An entry holds its own id (`<participant id>:<counter>`), a position key (the same fractional keys as the library
sync), a tombstone flag, a stamp (time, counter, participant) and the track's identity: content hash when known,
artist, title and duration. It carries nothing of the sender's ids or paths.

- **Matching.** Each side finds its own file for an entry: the same content hash when both have one, else the same
  artist and title (case-insensitive) and a duration within two seconds. An entry with no match is *missing*: it stays
  in the shared playlist, `shared_playlist_entries` lists it with `missing = true`, and it is never put in the local
  playlist. When the track is added to the library the entry takes its place at the next sync, and a copy added by
  hand is recognised as the same entry (no duplicate).
- **Merging.** Per entry, the larger stamp wins, so adds, removals and moves from both sides combine, the order
  of reading does not matter and repeating a sync changes nothing. The playlist is the live entries ordered by
  (position key, entry id). A move and a removal of the same entry resolve to whichever came last. The name travels
  the same way (the larger stamp wins; the one who joined does not send it back).
- **Local changes.** The library keeps, per shared playlist, the merged list as it last applied it (setting
  `shared.link.<playlist id>`). A sync compares the local playlist with it: tracks that disappeared are removals,
  new tracks are adds, tracks that changed their relative order are moves (the longest run that kept its order
  stays put; the rest get new keys between their neighbours). Entries that were missing here are not touched.

## Limits

- Clocks: stamps use each computer's clock first, so a computer whose clock is far ahead wins ties for a while.
- Nothing is encrypted (the files hold artist, title, duration and an optional content hash): put the folder where
  only the people you share with can read it.
- The hash is only used when the library has one for the track (the duplicate finder computes it); otherwise
  artist, title and duration decide, which can confuse two different recordings of the same name and length.
- Removing a participant, joining a playlist you already have, and a screen for all of this are left for later.

## Through the API, and transitions from sync
Shared playlists have REST routes (docs/API.md): `GET /v1/library/shared-playlists?folder=`, `POST /v1/library/playlists/{id}/share`
(publish), `POST /v1/library/shared-playlists/{shared_id}/join` (subscribe), `POST /v1/library/playlists/{id}/shared-sync`
(merge; the reply carries `summary`: `sent_added`, `sent_removed`, `sent_moved`, `received`, `missing`) and
`DELETE /v1/library/playlists/{id}/share`. Library sync also rebuilds the transition counts behind the track suggestions
whenever play history arrives from another device, so what you played on the laptop shapes the suggestions on the desktop.

## In the application

Settings keys (library settings; none of the audio keys syncs between devices): `sync.mix_links` (default on), `sync.analysis` (default off;
the settings panel notes about 1.9 KB per track), `sync.audio_playlists` (playlist or folder ids, comma separated), `sync.music_folder`,
`sync.audio_budget_mb`, `sync.audio_import_budget_mb` (0 = no limit), `sync.allow_unencrypted_audio` (default off; the panel warns that anyone
who can open the sync folder can then listen). `src/app/include/deckhand/app/sync_settings.hpp` turns them into `SyncConfig`.

A sync run (settings panel button, startup, `POST /v1/sync/run`) calls `sync_once()` and, when audio playlists or a music folder are set and
that succeeded, `sync_audio()`. The panel shows the last numbers (sent, received, linked mix points waiting for their tracks, analysis items
received, and the audio step: shared, imported, relinked, waiting, damaged, store size); `GET /v1/sync` returns the same as JSON.

### Known gap: a concurrent re-analysis can replace a locked grid

If device A locks a grid and device B, which had not seen the lock, re-analyses the same track afterwards, the track's `grid` field (written by
B, later) wins the per-field last-writer-wins merge while `grid_locked` stays 1 from A: the grid is replaced although it is locked. A fix
needs the writer's lock state to travel with the grid: capture writes `grid_locked` in the same batch whenever it writes `grid`; apply then
keeps a locally locked grid when the incoming grid arrives with `grid_locked = 0` (and lets the next capture on the locking device re-assert
its grid and lock with a newer stamp so everyone converges), while an explicit lock or unlock still merges last-writer-wins. This touches
capture, apply and the state merge in `src/sync/sync.cpp` (`kTrackFields`, the track apply loop near `lib.replace_beatgrid`), so it was left
alone; a test would run two engines in one process (see tests/sync_dev.hpp) with B unlocked and re-analysing after A's lock.
