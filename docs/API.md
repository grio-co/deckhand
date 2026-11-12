# DeckHand API

Everything DeckHand can do is reachable here; the UI is just a client.

## Style: REST (JSON/HTTP) + WebSocket
- REST for commands and queries; WebSocket for live events (position, levels, track changes).
- Published OpenAPI spec. Works from curl, shell, Stream Deck, any language.
- gRPC and GraphQL deliberately not used (harder to script; GraphQL suits nested data queries,
  not commands). gRPC may be added later if needed.

## Scheduled commands
Any command may carry a schedule: an exact timestamp, beat or bar. The engine executes it
sample-accurately, so scripted sets are not limited by request jitter.

## Security
- Bearer tokens, each with an ACL scope: e.g. `deck.control`, `mixer.control`, `library.read`,
  `library.write`, `stream.control`, `admin`. Least privilege by default.
- Tokens stored hashed; expiry and revocation supported.
- TLS supported; self-generated certificate with pinning on first use, or user-supplied cert.
- Binds to localhost by default; remote access is opt-in.
