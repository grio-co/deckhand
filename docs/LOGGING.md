# Logging

Public levels: ERROR, WARN, INFO, DEBUG. Hidden fifth level: TRACE (function entry/exit with
symbol names), enabled only by an explicit flag/environment variable, never by the UI.
- The audio thread never logs directly: it posts to a lock-free ring buffer drained elsewhere.
- Portable mode writes logs into the data folder. Nothing is sent anywhere.
