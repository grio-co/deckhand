# SQLCipher (vendored amalgamation)

DeckHand's database library on every platform: SQLCipher, an encrypting fork of SQLite, as one C file.

| | |
|---|---|
| SQLCipher | 4.20.0 (contains SQLite 3.54.1) |
| Upstream | https://github.com/sqlcipher/sqlcipher, release tag `v4.20.0` |
| Upstream archive | `https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v4.20.0.tar.gz`, SHA-256 `38705f51b61ac0636a5fd064f9a77fafabc7227b2aa2513f1b14c8bb1102affc` |
| Licence | BSD-3-Clause (SQLCipher, Zetetic LLC); the SQLite code in it is public domain. See `LICENSE.md`. |

## Files and hashes

`SHA256SUMS` is written by the script; `sha256sum -c SHA256SUMS` (or `shasum -a 256 -c`) checks them.

```
8cd2347d18886fee4651153fabeeeffb7cec008700987dab1c00261febc4220f  sqlite3.c
e36eb6ed4de77e56a633866a8f84ed339785662d0cf79cf099f420b638bf28ec  sqlite3.h
2a2826f6acf46fa650730cf42cbb22a642be33a7ef119c9c4f4bf6daf3bef48e  LICENSE.md
```

## How it was generated

`scripts/vendor-sqlcipher.sh` downloads the pinned archive, refuses it unless its SHA-256 matches, runs
`./configure --disable-tcl --with-tempstore=yes` and `make sqlite3.c` (upstream's own amalgamation target, using the
`jimsh` interpreter that ships in the archive, so `tclsh` is not needed), and copies `sqlite3.c`, `sqlite3.h` and
`LICENSE.md` here. Running it twice gives byte-identical files. The sources are unmodified.

To update: change `VERSION` and `TARBALL_SHA256` in the script, run it, review the diff, update the tables above.

## How DeckHand builds it

`CMakeLists.txt` target `dh_sqlcipher` (static library, warnings off for this file):

- `SQLITE_ENABLE_FTS5` (the library's search index), `SQLITE_THREADSAFE=1`, `SQLITE_TEMP_STORE=2` (temporary files stay in
  memory, never unencrypted on disk), `SQLITE_DQS=0`, `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_DEFAULT_MEMSTATUS=0`.
  `SQLITE_DEFAULT_FOREIGN_KEYS` is not set: the library turns foreign keys on itself.
- With `DECKHAND_WITH_SQLCIPHER=ON` (default): `SQLITE_HAS_CODEC`, `SQLITE_EXTRA_INIT=sqlcipher_extra_init`,
  `SQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown`, and a crypto provider: CommonCrypto on Apple platforms
  (`SQLCIPHER_CRYPTO_CC`, part of the OS, so macOS and iOS need no OpenSSL for the database), OpenSSL's libcrypto
  elsewhere (`SQLCIPHER_CRYPTO_OPENSSL`, the library the API server already uses).
- With `DECKHAND_WITH_SQLCIPHER=OFF`: the same source as plain SQLite, no codec and no crypto library.
- `-DDECKHAND_SYSTEM_SQLITE=ON` skips all this and links a system or package library instead (it may lack FTS5).

Why vendored rather than fetched at configure time: builds are reproducible and work offline, portable builds need no
installed library, and the distribution and vcpkg SQLCipher packages are built without FTS5.
