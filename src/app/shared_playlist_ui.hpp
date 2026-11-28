#pragma once
// Behind the shared-playlist panel (Panel::Shared): where the shared folder is, whether a playlist is linked, what the
// other side has put there, and the four things the panel does (share, join, sync, leave). The merge itself is
// deckhand::sync (src/sync/shared_playlist.hpp); this is the glue the panel and the REST status route share. Pure
// logic over the library and a folder, so it is tested headless with two libraries.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"
#include "deckhand/sync/shared_playlist.hpp"

namespace deckhand {

// The folder the shared playlists live in: the one chosen in the panel (setting "shared.folder"), else the library-sync
// folder ("sync.folder"). Empty when neither is set.
inline std::string shared_folder(Library& lib) {
  const std::string own = lib.setting("shared.folder");
  return own.empty() ? lib.setting("sync.folder") : own;
}

// Folder and the name the other participants see ("sync.device_name", else `fallback_label`).
inline sync::SharedConfig shared_settings_config(Library& lib, const std::string& fallback_label = "DeckHand") {
  sync::SharedConfig c;
  c.folder = shared_folder(lib);
  c.label = lib.setting("sync.device_name");
  if (c.label.empty()) c.label = fallback_label;
  return c;
}

// The shared id a local playlist is linked to ("" = not shared). Reads the link the sync module keeps in the settings.
inline std::string shared_link_id(Library& lib, std::int64_t playlist_id) {
  const std::string text = lib.setting("shared.link." + std::to_string(playlist_id));
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = text.substr(at, nl - at);
    if (line.compare(0, 7, "shared\t") == 0) return line.substr(7);
    at = nl + 1;
  }
  return {};
}

struct SharedStatus {
  bool linked = false;
  std::string shared_id;
  std::string folder;
  bool folder_ok = false;                       // the folder exists
  std::vector<std::string> participants;        // labels, when the folder could be read
  int entries = 0, missing = 0;                 // of the merged list: all, and those this library has no match for
};

SharedStatus shared_status(Library& lib, std::int64_t playlist_id, const sync::SharedConfig& cfg);

// Shared playlists in the folder that this library is not linked to yet (what "Join" offers).
struct JoinChoice {
  std::string shared_id, name;
  std::vector<std::string> participants;
};
std::vector<JoinChoice> shared_join_choices(Library& lib, const sync::SharedConfig& cfg);

// What a panel action reports.
struct SharedOutcome {
  bool ok = false;
  std::string error;            // the sync module's words
  std::string shared_id;
  std::int64_t playlist_id = 0; // the playlist shared, joined or synced
  int sent = 0, received = 0, missing = 0;
};
SharedOutcome shared_share(Library& lib, std::int64_t playlist_id, const sync::SharedConfig& cfg);
SharedOutcome shared_join(Library& lib, const std::string& shared_id, std::int64_t parent_folder, const sync::SharedConfig& cfg);
SharedOutcome shared_sync(Library& lib, std::int64_t playlist_id, const sync::SharedConfig& cfg);
void shared_leave(Library& lib, std::int64_t playlist_id);

}  // namespace deckhand
