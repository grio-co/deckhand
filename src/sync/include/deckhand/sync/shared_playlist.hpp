#pragma once
// One playlist shared with another person's library through a folder both of you replicate (cloud folder, stick).
// Library sync keeps YOUR devices identical; this shares a single playlist with a library that is not yours, so it
// carries no track data and no ids of yours: each entry names its track by content hash (when both sides have
// one) and by artist, title and duration, and each side matches that against its own files.
//
//   <folder>/DeckHandShared/<shared id>/<participant id>.dhsp   one file per participant, written only by its owner
//
// Merging is per entry and last-writer-wins on a (time, counter, participant) stamp, so adds, removals and moves
// from both sides combine, and reading the files in any order or twice gives the same playlist. A track that is not
// in your library stays in the shared playlist as a "missing" entry (shown by entries(), never put in your local
// playlist) and appears in it as soon as you have the track.
//
// No threads inside; no exceptions escape; failures come back in SharedResult::error.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand::sync {

struct SharedConfig {
  std::string folder;                 // the replicated folder; DeckHandShared/ is created inside it
  std::string label;                  // shown to the other participants ("Alice")
  std::function<std::int64_t()> clock;  // unix ms; default system clock
  std::function<std::string()> new_id;  // random hex id; default from the system
};

struct SharedTrackRef {
  std::string hash;  // content hash of the file ("" = unknown)
  std::string artist, title;
  std::int64_t duration_ms = 0;
};

struct SharedEntry {
  std::string entry_id;
  SharedTrackRef track;
  std::int64_t local_track_id = 0;  // 0 = not in this library
  bool missing = true;
};

struct SharedResult {
  bool ok = false;
  std::string error;
  std::string shared_id;
  std::int64_t playlist_id = 0;  // the local playlist
  int added = 0, removed = 0, moved = 0;  // local changes sent
  int received = 0;                       // entries changed here by the other side
  int missing = 0;                        // entries whose track is not in this library
};

struct SharedPlaylistInfo {
  std::string shared_id, name;
  std::vector<std::string> participants;  // labels
};

// Starts sharing a local playlist: writes your file and links the playlist.
SharedResult share_playlist(Library& lib, std::int64_t playlist_id, const SharedConfig& cfg);
// Creates a local playlist (under `parent`, 0 = top level) from a shared one and links it.
SharedResult join_shared_playlist(Library& lib, const std::string& shared_id, const SharedConfig& cfg, std::int64_t parent = 0);
// Sends your changes and applies theirs. Call again whenever the folder changed or the playlist was edited.
SharedResult sync_shared_playlist(Library& lib, std::int64_t playlist_id, const SharedConfig& cfg);
// Stops sharing: the local playlist stays as it is, the files in the folder are left alone.
void unlink_shared_playlist(Library& lib, std::int64_t playlist_id);
// The shared playlist in order, with the entries this library cannot match marked missing.
std::vector<SharedEntry> shared_playlist_entries(Library& lib, std::int64_t playlist_id, const SharedConfig& cfg);
// Shared playlists in the folder (for "join").
std::vector<SharedPlaylistInfo> list_shared_playlists(const SharedConfig& cfg);

}  // namespace deckhand::sync
