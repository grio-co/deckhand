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

Copies contain everything in the library, including the folder-sync passphrase (stored there unencrypted), so they
are readable by their owner only. `restore` keeps the passphrase of the library it replaces, so that an older copy
does not bring back unencrypted sync; a damaged library's passphrase cannot be kept, so check Settings afterwards.

Only `library.db` is covered: not settings files outside it, API tokens or audio files. How a restore interacts with
folder sync ([SYNC.md](SYNC.md)) has not been tested. There is no screen for this in the app yet.
