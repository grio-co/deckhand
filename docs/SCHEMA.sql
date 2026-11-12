-- DeckHand library schema (draft v1). SQLite. Independent of any other DJ software's layout.
PRAGMA foreign_keys = ON;

CREATE TABLE schema_version (version INTEGER NOT NULL);
INSERT INTO schema_version VALUES (1);

-- Portable mode: tracks live under a root; only the relative path is stored.
CREATE TABLE library_roots (
  id INTEGER PRIMARY KEY,
  label TEXT NOT NULL,
  last_seen_path TEXT            -- advisory; re-resolved on each launch / relink
);

CREATE TABLE tracks (
  id INTEGER PRIMARY KEY,
  root_id INTEGER NOT NULL REFERENCES library_roots(id),
  rel_path TEXT NOT NULL,
  title TEXT, artist TEXT, album TEXT, genre TEXT, label TEXT, comment TEXT,
  year INTEGER, rating INTEGER, color INTEGER,
  duration_ms INTEGER, bpm REAL, key TEXT,
  format TEXT, bitrate INTEGER, sample_rate INTEGER,
  date_added INTEGER NOT NULL,
  UNIQUE (root_id, rel_path)
);

CREATE TABLE beatgrids (
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  position INTEGER NOT NULL,     -- ordinal of the tempo section
  first_beat_ms REAL NOT NULL,
  bpm REAL NOT NULL,
  PRIMARY KEY (track_id, position)
);

CREATE TABLE cues (
  id INTEGER PRIMARY KEY,
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  kind TEXT NOT NULL,            -- 'memory' | 'hot' | 'loop'
  slot INTEGER,                  -- hot cue slot, if any
  start_ms REAL NOT NULL,
  end_ms REAL,                   -- loops
  label TEXT, color INTEGER
);

CREATE TABLE waveforms (
  track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
  kind TEXT NOT NULL, data BLOB NOT NULL
);

CREATE TABLE playlists (
  id INTEGER PRIMARY KEY,
  parent_id INTEGER REFERENCES playlists(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  is_folder INTEGER NOT NULL DEFAULT 0,
  position INTEGER NOT NULL
);

CREATE TABLE playlist_tracks (
  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
  position INTEGER NOT NULL,
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  PRIMARY KEY (playlist_id, position)
);

-- Repeatable imports: remember where each row came from so re-importing updates, not duplicates.
CREATE TABLE import_map (
  source TEXT NOT NULL,          -- e.g. 'djlib-v6'
  source_id TEXT NOT NULL,
  entity TEXT NOT NULL,          -- 'track' | 'playlist'
  local_id INTEGER NOT NULL,
  PRIMARY KEY (source, entity, source_id)
);

CREATE TABLE history (
  id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL,
  track_id INTEGER REFERENCES tracks(id) ON DELETE SET NULL,
  deck TEXT NOT NULL,
  started_at INTEGER NOT NULL,   -- unix ms
  played_ms INTEGER NOT NULL
);

CREATE INDEX idx_tracks_artist ON tracks(artist);
CREATE INDEX idx_tracks_title ON tracks(title);
CREATE INDEX idx_history_started ON history(started_at);
