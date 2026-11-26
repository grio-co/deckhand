# Library backups

DeckHand copies its library before a new version upgrades it, into `backups/` beside `library.db`
(`library-<UTC time>-upgrade.db`; the newest five are kept). If the copy cannot be made, the library is not upgraded
and DeckHand reports why.

By hand (add `--data DIR` for a non-default data folder):

    deckhandctl backup              # copy now (safe while DeckHand runs)
    deckhandctl backups             # list the copies, newest first
    deckhandctl restore FILE        # replace the library with a copy; quit DeckHand first

`restore` saves the library it replaces (`...-before-restore.db`; a damaged one is renamed `library.db.damaged-<time>`),
refuses while the library is open, and refuses files that are not a sound DeckHand library or come from a newer
DeckHand. Opening a restored older copy upgrades it again (after another copy).

A restore does not undo changes that already synced: at the next sync DeckHand reads them back ([SYNC.md](SYNC.md)).

Copies contain everything in the library, including the folder-sync passphrase (stored there unencrypted), so they
are readable by their owner only. `restore` keeps the passphrase of the library it replaces, so that an older copy
does not bring back unencrypted sync; a damaged library's passphrase cannot be kept, so check Settings afterwards.

Only `library.db` is covered: not settings files outside it, API tokens or audio files. There is no screen for this in the app yet.

## Metadata safety

- **Your audio files are never written to.** Tags, cues, grids, ratings and colours live only in `library.db`. The library, import, sync and analysis code only read audio files; export copies them to the destination (source untouched) and the recorder writes new files.
- **Edit journal.** Every change to a track's title, artist, album, genre, key, comment, rating, year, colour, BPM or grid lock is recorded with its old and new value, source (`user`, `api`, `import`, `sync`, `undo`, `analysis`) and a batch id shared by edits made by one operation. 200 entries are kept per track. An edit can be undone while the field still holds the value the edit set (`undo_edit`, `undo_batch`; REST: `POST /v1/library/edits/{id}/undo`); an undo is journalled too, so it can be undone again.
- **Re-imports keep your edits.** Importing again, or a rescan, does not overwrite a field whose latest journal entry is a user or API edit. `deckhand-cli import --overwrite-edits` (`ImportOptions::overwrite_edited`) replaces them anyway. DeckHand's own analysis results (`analysis`) are not user edits: an import may replace them, and undo lists (`track_history`, `recent_edits`, the history routes) leave them out unless asked. Changes arriving from folder sync are journalled as `sync`; a track deleted by sync goes to the trash.
- **Trash.** Removing a track moves it, with its cues, beat grid and playlist places, to a trash kept 30 days (`deleted_tracks`, `restore_track`, `GET /v1/library/trash`). It is purged when the library opens. Waveform caches are not kept (they are rebuilt by analysis).
