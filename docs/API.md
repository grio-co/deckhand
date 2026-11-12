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
| `POST /v1/decks/{n}/{control}` | `control:deck:{n}` | Any deck control, e.g. `play`, `cue`, `sync`, `seek`, `hotcue.3`, `beatloop`, `eq_low` |
| `GET /v1/mixer`, `POST /v1/mixer/{control}` | `read`, `control:mixer` | `crossfader`, `head_mix`, `head_level`, `master_level` |
| `GET /v1/controls` | `read` | Every control name with kind, range, help and current value |
| `POST /v1/commands` | `control` | `{"commands": [{"control": "deck1.play", ...}, ...]}` |
| `GET /v1/schedule`, `DELETE /v1/schedule[/{id}]` | `read`, `control` | Pending scheduled commands |
| `GET /v1/library/tracks` | `library:read` | `search`, `playlist`, `sort`, `desc`, `limit`, `offset` |
| `GET`/`PATCH /v1/library/tracks/{id}`, `POST /v1/library/tracks` | `library:read` / `library:write` | Track with cues and grid; edit tags; add a file |
| `GET`/`POST /v1/library/playlists`, `PATCH`/`DELETE /v1/library/playlists/{id}` | `library:*` | Playlists and folders |
| `POST /v1/library/playlists/{id}/tracks`, `DELETE .../tracks/{position}` | `library:write` | Playlist contents |
| `GET /v1/history`, `GET /v1/history/{session}` | `library:read` | Play history |
| `POST /v1/export` | `export` | `{"destination": "/Volumes/STICK", "playlist_id": 3}` |
| `GET`/`POST /v1/tokens`, `DELETE /v1/tokens/{id}` | `admin` | Token management |

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
