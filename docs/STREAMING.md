# Streaming: now playing overlays and OBS

DeckHand can tell your stream what is on air, with no scripting. Everything is local: the overlay server only
listens on `127.0.0.1`, and OBS is reached over its own WebSocket.

## Which track is "on air"

A track goes on air when it has been the loudest audible, playing deck (channel fader and crossfader taken into
account) for 8 seconds (configurable). While the current track is still audible, a challenger has to be clearly
louder, so cuts back and forth and long blends never make the title flicker. When everything is paused the last
track stays. Loading the same track on the other deck does not count as a new track.

## Text files

Point the "Now playing files" folder at any directory. DeckHand writes, atomically (OBS never sees half a file):

| File | Content |
| --- | --- |
| `title.txt`, `artist.txt` | just that field |
| `now-playing.txt` | your template, default `{artist} - {title}`. Placeholders: `{title} {artist} {album} {key} {bpm} {deck}` |
| `now-playing.json` | all fields as JSON, UTF-8 |
| `tracklist.txt` | `00:00 Artist - Title` lines, time since the first track (`h:mm:ss` after one hour); paste into a stream or video description |

In OBS add a "Text (Read from file)" source and choose `now-playing.txt`.

## Overlay Browser Source

Enable the overlay server (default port 8765), then in OBS add a **Browser** source with the URL
`http://127.0.0.1:8765/` (width 1280, height 720 is fine; the card sits bottom-left). The background is
transparent. The page updates live (server-sent events) and animates when the title changes.

Options go in the query string, e.g. `http://127.0.0.1:8765/?theme=accent&size=36&align=right`:

* `theme` = `dark` (default), `light`, `accent`, `minimal` (text only, with a shadow)
* `size` = font size in pixels, 10 to 120 (default 28)
* `align` = `left`, `center`, `right`
* `accent` = hex colour without `#`, e.g. `accent=00c2ff`
* `label` = small caption (default "Now playing"; `label=` hides it); `art=0` hides the artwork

Other URLs: `/now-playing.json` (current track), `/events` (event stream), `/artwork` (current cover art).

## Connecting OBS

1. In OBS: Tools, WebSocket Server Settings, enable the server (port 4455 by default) and set a password.
2. In DeckHand enter host (usually `127.0.0.1`), port and password. DeckHand connects in the background and keeps
   retrying with a growing delay (1 s up to 15 s) if OBS is closed or restarts.

Only plain `ws://` is supported, which is what OBS provides by default. Passwords are never sent: the OBS v5
challenge/response is used.

DeckHand can read the scene list, switch the program scene, set a text source, show or hide a source in a scene,
and start, stop or toggle streaming and recording (and read their status).

## Automation rules

Rules connect decks to OBS. Each rule has a trigger, an optional deck (empty = any) and an action:

| Trigger | Typical use |
| --- | --- |
| track change (a new track went on air) | set a text source to a template; show a lower third for N seconds |
| deck N starts / stops playing | switch to a scene when a deck starts, back to another when it stops |

| Action | Settings |
| --- | --- |
| set text | text source name, template (same placeholders as above) |
| switch scene | scene name |
| show source for T seconds | scene, source name, seconds (it is hidden again afterwards; a repeat extends the time) |

Names must match OBS exactly. If OBS is not connected, actions are skipped (nothing is queued for later).

## Turning it on in DeckHand (current build)
There is no settings screen yet; the outputs are switched on with settings through the API
(`PUT /v1/settings/{key}` with `{"value": "..."}`, admin token) and take effect at the next start.

| Setting | Meaning |
|---|---|
| `stream.files` = `1`, `stream.folder` | Write the now-playing text files (default folder: `NowPlaying` in the data folder) |
| `stream.overlay` = `1`, `stream.overlay_port` | Serve the browser overlay on `http://127.0.0.1:8765/` |
| `stream.template` | Text template, e.g. `{artist} - {title}` |
| `obs.enabled` = `1`, `obs.host`, `obs.port`, `obs.password` | Connect to OBS WebSocket |
| `obs.text_source`, `obs.text_template` | Text source to update on each track change |

The OBS password is stored in the library file in plain text and cannot be read back through the API.
