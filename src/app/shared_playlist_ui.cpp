// Glue for the shared-playlist panel and the REST status route (shared_playlist_ui.hpp).
#include "shared_playlist_ui.hpp"

#include <algorithm>
#include <filesystem>

namespace deckhand {

namespace {
SharedOutcome outcome(const sync::SharedResult& r) {
  SharedOutcome o;
  o.ok = r.ok;
  o.error = r.error;
  o.shared_id = r.shared_id;
  o.playlist_id = r.playlist_id;
  o.sent = r.added + r.removed + r.moved;
  o.received = r.received;
  o.missing = r.missing;
  return o;
}
}  // namespace

SharedStatus shared_status(Library& lib, std::int64_t playlist_id, const sync::SharedConfig& cfg) {
  SharedStatus s;
  s.shared_id = shared_link_id(lib, playlist_id);
  s.linked = !s.shared_id.empty();
  s.folder = cfg.folder;
  std::error_code ec;
  s.folder_ok = !cfg.folder.empty() && std::filesystem::is_directory(cfg.folder, ec);
  if (!s.linked || !s.folder_ok) return s;
  for (const sync::SharedPlaylistInfo& p : sync::list_shared_playlists(cfg))
    if (p.shared_id == s.shared_id) s.participants = p.participants;
  for (const sync::SharedEntry& e : sync::shared_playlist_entries(lib, playlist_id, cfg)) {
    ++s.entries;
    if (e.missing) ++s.missing;
  }
  return s;
}

std::vector<JoinChoice> shared_join_choices(Library& lib, const sync::SharedConfig& cfg) {
  std::vector<JoinChoice> out;
  std::error_code ec;
  if (cfg.folder.empty() || !std::filesystem::is_directory(cfg.folder, ec)) return out;
  std::vector<std::string> mine;  // shared ids already linked here
  for (const PlaylistNode& n : lib.playlists()) {
    const std::string id = n.is_folder ? std::string() : shared_link_id(lib, n.id);
    if (!id.empty()) mine.push_back(id);
  }
  for (const sync::SharedPlaylistInfo& p : sync::list_shared_playlists(cfg)) {
    if (std::find(mine.begin(), mine.end(), p.shared_id) != mine.end()) continue;
    out.push_back({p.shared_id, p.name, p.participants});
  }
  return out;
}

SharedOutcome shared_share(Library& lib, std::int64_t playlist_id, const sync::SharedConfig& cfg) {
  if (!shared_link_id(lib, playlist_id).empty()) {
    SharedOutcome o;
    o.error = "already shared";
    o.shared_id = shared_link_id(lib, playlist_id);
    o.playlist_id = playlist_id;
    return o;
  }
  return outcome(sync::share_playlist(lib, playlist_id, cfg));
}

SharedOutcome shared_join(Library& lib, const std::string& shared_id, std::int64_t parent_folder, const sync::SharedConfig& cfg) {
  return outcome(sync::join_shared_playlist(lib, shared_id, cfg, parent_folder));
}

SharedOutcome shared_sync(Library& lib, std::int64_t playlist_id, const sync::SharedConfig& cfg) {
  return outcome(sync::sync_shared_playlist(lib, playlist_id, cfg));
}

void shared_leave(Library& lib, std::int64_t playlist_id) { sync::unlink_shared_playlist(lib, playlist_id); }

}  // namespace deckhand
