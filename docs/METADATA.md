# Metadata fill from an open music database

DeckHand can fill in missing tags (album, year, genre) for tracks you select, using the public MusicBrainz web service. It is
**off by default** and never runs in the background.

## What is sent, to whom, when

- **To:** `musicbrainz.org`, over HTTPS (`https://musicbrainz.org/ws/2/`). The certificate chain and host name are verified
  against the system trust store; there is no fallback to plain HTTP.
- **When:** only after you turned the lookup on, and only for tracks you selected and asked to look up. Nothing is sent at start-up,
  on import or on a schedule.
- **What, per track:** the track's **artist and title**, as text in the query of one request. If a good match is found, a second
  request carries the database's own release id (not your data).
- **Never sent:** file paths or names, album, duration, rating, cues, playlists, the library or anything else. Album and duration stay
  on this machine and are only used to choose between the answers.
- **Request headers:** `User-Agent: DeckHand/<version>` and nothing else (no URL, name, e-mail address or other
  contact detail). Your IP address is visible to the service as for any web request.
- **Rate:** at most one request per second. On HTTP 503 or 429 DeckHand waits (2 s, doubling, or the `Retry-After` time, capped at
  30 s) and retries up to 3 times, then gives up on that track.
- **Limits:** at most 3 redirects (https, same host only), responses over 2 MB are refused, 15 s timeout per request.

## What is written

Nothing, until you accept. A lookup returns **proposals**: field, current value, proposed value, confidence and the source page
on the database. Accepted proposals are written through the library's normal tag edit, so they appear in the edit history
(source `metadata`) and are undone as one batch.

Rules the matcher follows:

- Titles and artists are compared after normalising case, punctuation, diacritics, a leading "The", `feat.` credits and
  remaster/original-mix suffixes. A remix, edit, dub or live qualifier on one side only counts as a different recording.
- A candidate's length must be within 5 s of the track's (when both are known).
- Original releases are preferred over compilations, live and unofficial releases.
- Below the confidence threshold (default 80 %) nothing is proposed.
- A field that already has a value is not touched unless the caller asks for overwriting; a field you edited by hand (according to
  the edit history) is not touched unless the caller asks for that as well. A proposal is skipped when the field changed after the
  lookup.
- Tracks without an artist or a title are skipped.
- Label, ISRC and track number are shown as information only; the library has no field for them.

## Musical key

The database holds no musical key and DeckHand does not invent another source for it. Key stays with DeckHand's local analysis.

## Turning it on and off

- Setting `metadata.musicbrainz.enabled`: `1` = lookups allowed, anything else (the default) = off. With it off no request is made.
- Command line: `deckhandctl metadata status | enable | disable`, then
  `deckhandctl metadata fill --ids 1,2,3` (prints proposals; add `--apply 1` to accept all of them) and
  `deckhandctl metadata undo BATCH_ID`.
- Build: `-DDECKHAND_WITH_METADATA=OFF` (or a platform without a TLS library) builds a stub: the network part always fails with
  "this build has no network lookup".

- Application: Settings, section "Metadata fill", has the switch and says what is sent. The panel `ui.metadata` (default shortcut
  `mod+alt+shift+m`) works on the browser selection at the moment it opens: **Look up** runs on a worker thread with progress and
  Cancel, each proposal is shown as current > proposed with its confidence and can be accepted or rejected (a change to a value
  that is already there starts rejected), **Apply** writes the accepted ones as one batch and **Undo last batch** restores it.
  The edit history (Trash and history panel, `GET /v1/library/edits`) shows these edits with source `metadata`.
- REST: `GET` and `PUT /v1/metadata` (status with proposals; the opt-in), `POST /v1/metadata/lookup | cancel | review | apply | undo`
  (docs/API.md). A lookup is refused with 403 while the opt-in is off.
- Tests never reach the network: they inject a canned transport, and the HTTPS transport refuses to open a socket while the
  environment variable `DECKHAND_NO_NETWORK` is set (CMake sets it for every ctest test).

## Not verified

Everything against the real service: the request syntax, the response shapes (the tests use hand-written samples built from the
documented format), the TLS client on a live connection, and system trust-store loading on Windows and Linux.
