# DeckHand API

Everything DeckHand can do is reachable here; the UI is just a client.

## Style: REST (JSON/HTTP) + WebSocket
- REST for commands and queries; WebSocket for live events (position, levels, track changes).
- Published OpenAPI spec. Works from curl, shell, Stream Deck, any language.
- gRPC and GraphQL deliberately not used (harder to script; GraphQL suits nested data queries,
  not commands). gRPC may be added later if needed.

## OpenAPI
The full machine-readable description (every path, scope, schema, the control-name tables and the
WebSocket protocol) is [`openapi.yaml`](openapi.yaml), OpenAPI 3.1. Open it in any OpenAPI viewer
(Swagger Editor, Redoc, Scalar, Postman, Insomnia) or feed it to a client generator. The code is the
source of truth if the two ever disagree.

## Scheduled commands
Any command may carry a schedule: an exact timestamp, beat or bar. The engine executes it
sample-accurately, so scripted sets are not limited by request jitter.

## Security
- Bearer tokens, each with a set of scopes: `read`, `control` (or narrower `control:deck:1`,
  `control:mixer`), `library:read`, `library:write`, `export`, `admin`. A scope covers its
  sub-scopes; `admin` covers everything. `POST /v1/commands` and the sampler routes need the
  broad `control` scope. Least privilege by default.
- Tokens stored hashed; expiry and revocation supported.
- TLS supported; self-generated certificate with pinning on first use, or user-supplied cert.
- Binds to localhost by default; remote access is opt-in.

## Implementation notes (module `api`, `src/api/`)
- Headers: `json.hpp` (JSON), `auth.hpp` (scopes, `TokenStore`, rate limiter, lockout), `server.hpp`
  (`ApiServer`, `generate_self_signed`, `certificate_fingerprint`). `test_client.hpp` is test-only.
- Scopes are colon paths (`control:deck:1`). A granted scope covers a required one if its segments are a
  prefix and each segment equals or is `*`; `admin` covers everything; nothing else implies anything.
  Route scopes may use captures: `control:deck:{deck}`. Scope names in the older list above
  (`deck.control` ...) are not special to the server; the application picks the names.
- Tokens: `dh_` + 256 random bits, stored as SHA-256 in a 0600 JSON file, written atomically; revoking
  removes the entry and drops live WebSocket connections within ~2 s.
- Auth: `Authorization: Bearer`; WebSocket also `Sec-WebSocket-Protocol: deckhand.v1, bearer.<token>`
  (list `deckhand.v1` first so the token is not echoed back). Tokens in URLs are ignored.
  Failed auth is counted per IP; after the threshold every request from that IP gets 429 until it expires.
- WebSocket messages: `{"id","cmd","args"}` -> `{"id","ok","status","result"|"error"}`; built-ins
  `subscribe`, `unsubscribe`, `ping`. Events: `{"topic","data"}`, filtered by the scope given to `publish()`.
- Plain HTTP only on loopback; TLS 1.2+ with a PEM cert/key. Unmatched paths need a token too (no route probing).
- Not implemented: chunked request bodies (400), HEAD, permessage-deflate, HTTP/2, IPv6 zone handling.
- Sockets and OpenSSL live in `net.cpp` only (POSIX; a Winsock port replaces that file). The process-wide
  SIGPIPE is set to ignore by the first listen.

## Reference (v1, as implemented)

The server starts with the application on `http://127.0.0.1:7373`. Settings live in the library
(`api.enabled`, `api.port`, `api.bind`, `api.tls`); binding anything other than loopback turns
TLS on with a self-signed certificate in the data folder (`api-cert.pem`). On first run an owner
token with the `admin` scope is written to `api-admin-token.txt` in the data folder (mode 0600).
Create narrower tokens with `POST /v1/tokens` or `deckhandctl token create`.

Send `Authorization: Bearer <token>`. Bodies and replies are JSON.

| Method and path | Scope | Purpose |
|---|---|---|
| `GET /v1/status` | `read` | Application, audio device, track count |
| `GET /v1/decks`, `GET /v1/decks/{n}` | `read` | Deck state, loaded track, cues, loop |
| `POST /v1/decks/{n}/load` | `control:deck:{n}` | `{"track_id": 12}` or `{"path": "/abs/file"}` |
| `POST /v1/decks/{n}/{control}` | `control:deck:{n}` | Any deck control, e.g. `play`, `cue`, `sync`, `seek`, `hotcue.3` (slots 1..16), `beatloop`, `eq_low`, `key_sync`, `key_sync_mode` |
| `GET /v1/mixer`, `POST /v1/mixer/{control}` | `read`, `control:mixer` | `crossfader`, `head_mix`, `head_level`, `master_level`, `mic_on`, `mic_level`, `mic_eq_low`, `mic_eq_high`, `mic_talkover`, `mic_talkover_depth`, `mic_monitor`, `mic_gate` |
| `GET /v1/controls` | `read` | Every control name with kind, range, help and current value |
| `POST /v1/commands` | `control` | `{"commands": [{"control": "deck1.play", ...}, ...]}` |
| `GET /v1/schedule`, `DELETE /v1/schedule[/{id}]` | `read`, `control` | Pending scheduled commands |
| `GET /v1/library/tracks` | `library:read` | `search`, `playlist`, `sort`, `desc`, `limit`, `offset` |
| `GET`/`PATCH /v1/library/tracks/{id}`, `POST /v1/library/tracks` | `library:read` / `library:write` | Track with cues and grid; edit tags; add a file |
| `GET /v1/library/tracks/{id}/history`, `GET /v1/library/edits` | `library:read` | Edit journal: every changed field with old and new value, source (`user`, `api`, `import`, `sync`, `undo`, `analysis`; the last is hidden unless `include_analysis=1`) and batch; 200 entries kept per track |
| `POST /v1/library/edits/{id}/undo`, `POST /v1/library/edit-batches/{id}/undo` | `library:write` | Restore the old value(s); 409 if a field changed again since (nothing is undone) |
| `POST /v1/library/tracks/{id}/grid`, `POST /v1/library/tracks/{id}/cues/follow-grid` | `library:write` | Replace a track's grid; cues that were on the old grid (15 ms of a beat, half or quarter beat) keep their beat position (loops keep their length in beats; halve/double leaves positions; off-grid cues stay and are listed). Body: `sections`, `bpm`, `lock`, `cues_follow` (`ask` default, `always`, `never`; setting `cues_follow_grid`), `dry_run`. With `ask` the response holds the proposal and `batch_id`; `follow-grid` with that `batch_id` applies it inside the same edit batch, so one undo restores grid and cues |
| `POST /v1/library/analysis/strip` | `library:write` | Clear derived data only: `kinds` of grid, key, loudness, waveform, first_sound, phrases, vocals, stems for `ids`, a `playlist_id`/`search`, or `all`; `force`, `dry_run`. Tags, cues, playlists and the main cue are never touched; locked grids and hand-set keys are skipped unless forced. Undo (edit batch) restores grid, tempo, key and lock; loudness, waveform, phrases and first-sound/load cue are not undoable |
| `GET /v1/library/duplicates`, `POST /v1/library/duplicates/hash`, `POST /v1/library/duplicates/merge` | `library:read` / `library:write` | Duplicate groups (`exact` by file hash, `likely` by artist, title and length, with confidence, differences and a `suggested_keep`), a bounded hashing step, and `merge` (`keep_id`, `remove_ids`, `options`: `copy_cues`, `fill_metadata`, `combine_history`, `dry_run`): playlists re-pointed without doubles, cues/grid copied where missing, history combined, removed tracks to the trash, audio files never touched |
| `GET /v1/library/trash`, `POST /v1/library/trash/{id}/restore` | `library:read` / `library:write` | Removed tracks (kept 30 days) and restoring one with its cues, grid and playlist places |
| `POST /v1/library/export/xml`, `POST /v1/library/playlists/{id}/export/m3u8`, `POST /v1/library/export/m3u8` | `export` | Write the library as DJ library XML, or playlists as M3U8, to a server-side path ([INTERCHANGE.md](INTERCHANGE.md)) |
| `POST /v1/library/import/xml`, `POST /v1/library/import/m3u8` | `library:write` | Import them; `dry_run` reports without writing; imported edits are journalled (source `import`) and keep user/API edits unless `overwrite_metadata` |
| `POST /v1/export/preflight`, `GET /v1/export/report` | `export`, `library:read` | Check which tracks a club player can read before exporting; report of the last export. `POST /v1/export` takes `convert_incompatible` and `player_profile` |
| `GET`/`POST /v1/library/playlists`, `PATCH`/`DELETE /v1/library/playlists/{id}` | `library:*` | Playlists and folders |
| `POST /v1/library/playlists/{id}/tracks`, `DELETE .../tracks/{position}` | `library:write` | Playlist contents |
| `GET /v1/decks/{n}/grid`, `POST /v1/decks/{n}/grid/{action}` | `read`, `control:deck:{n}` | Beat grid of the loaded track and its edits: `shift` (ms), `align`, `set_first_beat`, `downbeat`, `rotate`, `bpm_delta`, `set_bpm`, `scale`, `tap`, `add_section`, `remove_section`, `lock`, `reanalyse`, `undo`, `edit` |
| `GET /v1/decks/{n}/stems`, `POST /v1/decks/{n}/stems` | `read`, `control:deck:{n}` | Stem playback: state (`none`, `loading`, `attached`, `failed`; per stem mute, solo, isolate, level, meter, fx) and `{"attach", "detach", "enabled", "preset": "acapella\|instrumental\|drums_only\|no_drums\|reset", "stems": {"vocals": {"mute", "solo", "isolate", "level"}}}`. Controls: `deckN.stem.<vocals\|drums\|bass\|other>.<mute\|solo\|isolate\|level>`, `deckN.stems.<acapella\|instrumental\|drums_only\|no_drums\|reset\|enable\|detach>`; per-stem effect chains are `deckN.stemM.chain.*` (M 1..4) |
| `GET /v1/decks/{n}/cues`, `POST /v1/decks/{n}/cues/{action}` | `read`, `control:deck:{n}` | Memory cues, saved loops, hot cue names and colours: `memory_set`, `memory_delete`, `memory_style`, `memory_move`, `loop_save`, `loop_recall`, `loop_delete`, `hot_set`, `hot_style`, `hot_loop`, `hot_delete`, `main_set`, `wave_zoom` |
| `GET`/`POST /v1/library/smart-playlists`, `GET`/`PATCH`/`DELETE .../{id}`, `GET .../{id}/tracks` | `library:*` | Smart playlists: saved rule sets (text, BPM range, key set, genre, rating, colour, added within N days, play count, in / not in a playlist, missing file; all or any) |
| `GET /v1/debug/windows`, `GET /v1/debug/hit?window=&x=&y=` | `admin` | The UI's windows, and what a click at a point of a (shaped, transparent) window would do: none / drag / control / resize / solid |
| `GET /v1/preview`, `POST /v1/preview/load`, `POST /v1/preview/{control}` | `read`, `control` | Pre-listen in the headphones (`play`, `stop`, `seek`, `level`, `master`); refused without a headphone output unless `preview.master` is on |
| `GET /v1/history`, `GET /v1/history/{session}` | `library:read` | Play history |
| `GET /v1/queue`, `POST /v1/queue/add`, `POST /v1/queue/move`, `DELETE`/`PATCH /v1/queue/entries/{id}`, `POST /v1/queue/{clear,shuffle,skip,back}`, `GET /v1/queue/history`, `GET`/`PATCH /v1/queue/settings` | `read`, `control` | The play queue (persisted; survives a restart). `add` takes `track_ids` / `track_id` / `playlist` / `smart_playlist` / `search` and `mode` (`end`, `next` = play next, behind earlier play-nexts, or `at` + `position`), plus `shuffle`, `replace`, `limit`, `force` (ignore the duplicates policy). Entries carry optional per-entry overrides (`mix_in_ms`, `mix_out_ms`, `transition_beats`, `transition_type`, `max_play_ms`; `null` = automatic). Settings: `duplicates` (`allow` / `skip_recent`) with `duplicates_window`, and `endless` (`enabled`, `source` = `playlist:<id>` / `smart:<id>` / `search:<text>`, `keep`, `order`) |
| `GET /v1/automix`, `POST /v1/automix/{start,stop,pause,resume,skip}`, `GET`/`PATCH /v1/automix/settings` | `read`, `control` | Automix: state, transport and settings (transition type and length, mix-out bars, tempo-gap limit, bass swap, `override_mode` yield / hands_off, `on_empty` finish / fade). Controls: `automix.run`, `automix.start`, `.stop`, `.pause`, `.skip`, `.next_type`, `.next_length`, `.queue_clear`, `.queue_shuffle`, `.queue_skip`, `.queue_back` (`POST /v1/controls/{name}`) |
| `GET`/`PUT /v1/library/tracks/{id}/automix` | `library:read`, `library:write` | A track's automix points: `mix_in_ms`, `mix_out_ms`, `transition_beats`, `transition_type`, `max_play_ms` (`null` / missing = automatic; PUT replaces all five). GET adds `effective`: the mix-in and mix-out automix would use. Unset points default to the phrase analysis's mix-in and mix-out when the track has phrases (the main cue still wins for the mix-in), else to the load cue and "N bars before the last sound" |
| `GET /v1/library/tracks/{id}/phrases` | `library:read` | The stored phrase analysis: sections (start/end ms and bars, energy 1..10, label + `label_confidence`, `kind`, `boundary_confidence`, `kick`); `bars=1` adds `bar_starts_ms` and `bar_energy` |
| `GET /v1/library/tracks/{id}/suggestions` | `library:read` | What goes well after this track, best first. Score = tempo (within the allowed pitch change, also half / double time) + key on the wheel + energy (seed outro vs candidate intro, from the phrase analysis) + shared genre + rating, plus the pairs the user played back to back before, a small boost for never-played tracks, minus a penalty for tracks played in this session. Each item has `score`, `pitch_percent`, `pair_count` and `factors` (`id`, `points`, `value`, `detail`, `tag`: the breakdown). Query: `limit`, `playlist`, `max_pitch` (percent), `half_double`, `harmonic_only`, `exclude_queued`, `exclude_played` (0/1). Weights and defaults are settings: `suggest.w.tempo` / `key` / `energy` / `genre` / `rating` / `history` / `explore` / `recent`, `suggest.max_pitch`, `suggest.half_double`, `suggest.harmonic_only`, `suggest.exclude_queued`, `suggest.exclude_played` (change with `PUT /v1/settings/{key}`) |
| `GET /v1/decks/{deck}/suggestions` | `read` | The same for the track on a deck; `deck` is 1 to 4, or `master` for the deck on air. 409 when nothing is loaded |
| `GET /v1/library/tracks/{id}/transitions` | `library:read` | Tracks played right after this one (count, last time) and pairs saved with a note |
| `POST /v1/library/transitions` | `library:write` | Saves a pair (`from`, `to`, `note`) in the transition history |
| `GET /v1/library/tracks/{id}/cue-suggestions` | `library:read` | Hot-cue suggestions from the phrases, best first: `max` (0..8), `mix_out_bars` (8, 16, 32), `habit=1` (bias toward this user's cueing, learned from their cued tracks), `memory=1` (boundary candidates too). Writes nothing |
| `POST /v1/library/tracks/{id}/cue-suggestions/apply` | `library:write` | Writes the top `max` suggestions into FREE hot slots only (`memory: true` adds boundary memory cues); never touches an existing cue. One journalled batch (`batch_id`, undo via `POST /v1/library/edit-batches/{id}/undo`); `dry_run`; returns `written` (with 1-based `slot`) and `skipped` |
| `POST /v1/library/cue-suggestions/apply` | `library:write` | The same over `ids`, or `playlist_id` / `search` (at most 5000 tracks) in one batch and one transaction; `dry_run`; reports `tracks_without_phrases` |
| `POST /v1/export` | `export` | `{"destination": "/Volumes/STICK", "playlist_id": 3}`, also `playlist_ids`, `stem_mixes` (`["acapella", "instrumental"]`, `"all"` for the four stems, or a bit set), `stem_mixes_by_playlist`, `stem_bits` (16/24), `stems_inline`, `stems_folder`. Asynchronous; `GET /v1/export/report` then has `stem_tracks`, `no_stems[]`, `cues_not_exported` (hot cues 9-16: the stick format carries 8; they are left out and never fail the export), `required_bytes`, `available_bytes`, `ok`, `error` |
| `POST /v1/export/verify_device`, `POST /v1/export/backup_device`, `GET /v1/export/device_backups` | `export` | Pre-gig check of a stick (`{"path": "/Volumes/STICK"}`), dated hash-verified backup of its library (`include_audio`), the backups newest first |
| `POST /v1/export/restore_device` | `admin` | `{"path": "/Volumes/STICK", "backup": "<folder from device_backups>", "force": false}`. Never implicit; refuses over an existing library unless `force` is true |
| `POST /v1/decks/{n}/key_sync`, `POST /v1/decks/{n}/key_match`, `POST /v1/decks/{n}/key_reset` | `control:deck:{n}` | Key sync: follow the other deck's key (value 1 / 0, none toggles), match once, reset. The deck's `key_sync` object holds `key`, `follow`, `result` (ok, too_far, no_own_key, no_reference_key, leader), `pitch` |
| `GET`/`PUT /v1/key_sync/config` | `read` / `control` | `{"mode": 0 or 1 (match / any compatible key), "fine": false (cents), "limit": 2.0 (semitones)}`; settings `keysync.mode`, `keysync.fine`, `keysync.limit` |
| `GET /v1/lighting/status`, `GET /v1/lighting/preview` | `read` | Lighting (DMX over Art-Net / sACN, docs/LIGHTING.md): what runs and what it sends; one swatch per fixture. `available` is false in a build without the module |
| `GET`/`PUT /v1/lighting/config` | `admin` | The whole lighting configuration (outputs, patch, show: master, latency, palette, strobe cap, `strobe_enabled: false` = no strobes). PUT merges, validates, saves `lighting.json`. Everything is off by default; an output only sends once enabled with an address |
| `POST /v1/lighting/command` | `control` | `{"action": "blackout" / "hold" / "enable" / "master" / "strobe" / "override" / "release", ...}` |
| `GET /v1/library/shared-playlists?folder=` | `library:read` | Shared playlists found in a replicated folder |
| `POST /v1/library/playlists/{id}/share`, `DELETE ...` | `library:write` | Publish a playlist to a shared folder (`{"folder": "/abs/dir", "label": "Alice"}`) / stop sharing |
| `POST /v1/library/shared-playlists/{shared_id}/join`, `POST /v1/library/playlists/{id}/shared-sync` | `library:write` | Subscribe (creates a local playlist) / merge both sides; the reply has `summary` (`sent_added`, `sent_removed`, `sent_moved`, `received`, `missing`) |
| `GET /v1/controllers` | `read` | Connected and previously seen controllers: mapping file, decks, pad modes, per-controller settings (library settings `controller.<name>.jog_scale`, `ticks_per_rev`, `pickup`, `leds`, `mapping`) |
| `GET`/`POST /v1/tokens`, `DELETE /v1/tokens/{id}` | `admin` | Token management |

Lighting controls (`lighting.blackout`, `lighting.hold`, `lighting.master`, `lighting.strobe`, `lighting.enable`) are named controls
too; the application carries them out, so they cannot be scheduled.

Track preparation controls are also named controls (`GET /v1/controls`): `deck1.metronome`, `deck1.grid_shift`
(`{"value": -3}` moves the grid 3 ms earlier), `deck1.grid_bpm`, `deck2.memory_cue_set`, `deck1.hotcue_style.3`
(`{"text": "Drop", "color": 16734300}`), `deck1.wave_zoom`, `mixer.metronome_level`, `preview.play`, ... They work in
`POST /v1/commands` and over the WebSocket too, but cannot be scheduled (they are carried out by the application, not the
engine). The metronome click and the pre-listen player play on the headphone pair only. Colours are numbers `0xRRGGBB`.

Control bodies: `{"value": 0.5}`. Without a value a toggle flips, a button is tapped and a trigger
fires. Any control may be scheduled by adding one trigger; the engine then executes it on the exact
output sample, and the reply is `202` with a `scheduled_id`:

| Trigger | Fires |
|---|---|
| `"in_ms": 500` or `"at_unix_ms": 1790000000000` | At that time (as exact as the audio clock can be related to wall time: about one audio callback) |
| `"when": {"deck": 1, "seconds": 200.0}` | When deck 1's playhead reaches 200 s (at once if it is already past) |
| `"at_beat": {"deck": 1, "beat": 256, "bar": true}` | On that beat of deck 1's grid (beat 0 = first grid beat); `bar` moves it to the next bar line |
| `"in_beats": {"deck": 1, "beats": 16, "bar": true}` | That many beats from deck 1's current beat |

A toggle given a value (`{"value": 1}`) is compared with the deck's state when the request arrives,
not when it fires. The result of each scheduled command arrives on the `events` topic as
`{"type": "scheduled", "scheduled_id", "control", "ok", "clock_frame"}`.

WebSocket at `/v1/ws` (token in `Authorization`, or `Sec-WebSocket-Protocol: bearer.<token>`):
`{"cmd": "subscribe", "args": {"topics": ["decks", "mixer", "events"]}}`. `decks` and `mixer` are
sent ten times a second; `events` carries `deck.loaded`, `deck.play`, `deck.pause` and `scheduled`.
`{"cmd": "control", "args": {"control": "deck1.play", "value": 1}}` runs a control.

    TOKEN=$(cat ~/Library/Application\ Support/DeckHand/api-admin-token.txt)
    curl -H "Authorization: Bearer $TOKEN" -X POST http://127.0.0.1:7373/v1/decks/1/play
    curl -H "Authorization: Bearer $TOKEN" -X POST -d '{"value":1,"when":{"deck":1,"seconds":180}}' \
         http://127.0.0.1:7373/v1/decks/2/play

## Play queue and automix

Automix plays the play queue with two decks. While one plays, the next entry is loaded on the other and parked at its mix-in
point (the entry's override, else the track's own point, else the user's main cue, else the analysis load cue). Shortly before
the transition (about 4 s) the whole transition is handed to the engine as scheduled commands anchored to the playing deck's
beats, so the incoming track starts on the exact sample of a bar line. The mix-out is the point where the outgoing track is
completely gone: the entry's override, else the track's own, else `mix_out_bars` (16) bars before the last audible sound
(`last_sound_ms`, stored by analysis; the file length when unknown), capped by `max_play_ms` counted from the mix-in. The
transition starts `transition_beats` before the mix-out, on the nearest bar line (rounded down when `max_play_ms` limits it).

Transitions: `crossfade` (equal power on the channel faders, bass swap halfway by the low EQ), `cut` (on the bar), `fade`
(fade out, then the next fades in; used for tracks without a beat grid, timed in seconds), `echo_out` (the outgoing track is cut
into an echo tail). The tempo of the incoming track starts at the outgoing tempo and ramps back to its own over the transition,
unless the gap is above `max_sync_gap_pct` (8 %): then it keeps its own tempo and `unsynced_type` (cut by default) is used.
Length and type come from the outgoing track's entry override, the incoming track's entry override, then their own points, then
the global settings. Automix drives the channel faders and sets the crossfader to the centre; the crossfader is left at the
centre while it runs. With `override_mode` `yield`, a control you touch during a transition (a fader, a bass knob, the
incoming tempo, the crossfader) is left alone until the transition ends, and touching a deck's transport (pause, seek, loop,
jog, load) abandons the transition; `hands_off` carries on regardless.

WebSocket `events` topic: `queue.changed` (`revision`, `size`), `automix.state`, `automix.transition_start` (`type`,
`from_deck`, `to_deck`, `from_track`, `to_track`, `beats`, `synced`), `automix.transition_end`, `automix.entry_failed`
(a track that cannot be loaded is skipped), `automix.override`, `automix.finished`. After a restart the queue is restored
(an entry whose transition was scheduled but had not started is first again) and automix waits for `start`; `resume_available`
says it was running.

## Step sequencer and slicing

The sampler has a 16-step pattern player (8 patterns, 16 slots, see docs/SEQUENCER.md). Everything is also a named control, so it
works in `POST /v1/commands`, over the WebSocket, as a keyboard shortcut and scheduled like any engine control:
`seq.run`, `seq.record` (toggles), `seq.bpm` (0 follows the decks), `seq.swing` (0..0.75), `seq.pattern.<1..8>.select` and `.queue` (from
the next bar), `seq.clear` (none = the pattern being played, 0 = all, 1..8 = that one), `seq.step.<1..16>.slot<1..16>` (a cell of the pattern
being played: 0 off, 0..1 on at that velocity, none = toggle), `sampler.slot.<1..16>.slices` (0, 8 or 16), `sampler.slot.<n>.slice.<1..16>`
(plays one slice, value = velocity), `sampler.slot.<n>.scratch` (1 holds the sample's position, 0 lets it go) and
`sampler.slot.<n>.scratch.delta` (moves the held sample by that many seconds of the sample, signed).

| Route | Scope | Purpose |
|---|---|---|
| `GET /v1/sequencer` | `read` | Running, recording, pattern (1..8), queued pattern (0 = none), step (1..16, 0 = stopped), `bpm`, `swing`, `follows_deck`, and `patterns` (all eight as text) |
| `GET`/`PUT /v1/sequencer/patterns/{n}` | `read` / `control` | One pattern as text (`{"text": "dhseq1;..."}`); PUT replaces it (400 for a text that is not valid) |
| `POST /v1/sequencer/step` | `control` | `{"pattern": 3, "step": 5, "slot": 2, "on": true, "velocity": 0.8}` (pattern defaults to the one being played) |
| `POST /v1/sequencer/{action}` | `control` | `run`, `record` (`{"on": true}`, none = toggle), `bpm`, `swing` (`{"value": n}`), `select`, `queue` (`{"pattern": n}`), `clear` (`{"pattern": n}`, `{"all": true}`, `slot` limits it to one slot) |
| `GET /v1/sampler/slices` | `read` | Per slot: `loaded`, `slices`, `last_slice` (1-based, 0 = none), `scratching` |
| `POST /v1/sampler/{slot}/slices` | `control` | `{"count": 0 / 8 / 16}` |
| `POST /v1/sampler/{slot}/slices/{index}/trigger` | `control` | Plays slice `index` once; `{"velocity": 0..1}` optional |

The eight patterns, the tempo (while the sequencer runs on its own tempo, not a deck's), the swing and the selected pattern are kept
in the library settings (`seq.pattern.1..8`, `seq.bpm`, `seq.swing`, `seq.current`) a moment after each change, and sent to the engine at
start and again after an engine process restart.

## Linked mix points and vocal regions

A linked mix point says: when track A reaches `from_ms`, track B starts from `to_ms` (optionally with its own `transition_beats` and
`transition_type`). Automix uses the link between the playing track and the next queued one instead of the computed mix-out and mix-in;
the suggestions list shows a "linked mix point" reason (and lets linked tracks through the tempo and key filters); the positions follow
grid edits of their own track like cues do; the waveform marks them (a forward flag at the start of a link out of the track, a backward
flag at the end of a link into it). Merging duplicates moves the links of the removed tracks to the kept track.

| Route | Scope | Purpose |
|---|---|---|
| `GET /v1/library/mix-links?track=` (or `from=`, `to=`, or `from=&to=`) | `library:read` | Links out of and into a track / out of one / into one / between two tracks |
| `GET /v1/library/tracks/{id}/mix-links` | `library:read` | `{"from": [...], "to": [...]}` |
| `POST /v1/library/mix-links` | `library:write` | `{"from_track", "from_ms", "to_track", "to_ms", "transition_beats"?, "transition_type"?, "label"?}` (201) |
| `GET`/`PUT`/`DELETE /v1/library/mix-links/{id}` | `library:read` / `library:write` | One link; PUT changes the fields given (`transition_*` may be `null`) |
| `POST /v1/mix-links/from-decks` | `library:write` | `{"from_deck": 1, "to_deck": 2}`: link the two playheads (the same as the `ui.link.1.2` shortcut control) |
| `GET /v1/library/tracks/{id}/vocals` | `library:read` | Vocal regions found in the vocals stem (`regions` as `start_ms` / `end_ms`); filled in the background after a stem separation, drawn as a thin band on the waveform |

The grid edit reply (`POST /v1/library/tracks/{id}/grid`) and `POST .../cues/follow-grid` carry `mix_links_moved`; a duplicate merge
carries `mix_links_moved` too.

## One instance per data folder

A second DeckHand started on the same data folder is refused before it touches the engine process: the first holds an OS lock on
`<data folder>/deckhand.lock` (released by the OS when it ends, however it ends). The second shows the message (macOS also brings the first
window forward) and exits. Portable mode keeps its data beside the app, so it has its own lock.
