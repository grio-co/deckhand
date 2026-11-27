#pragma once
// The library settings that configure sync, and the conversion to SyncConfig. One place, so the sync job, the settings
// panel and the REST routes agree. Keys (all in the library settings table):
//   sync.folder, sync.device_name, sync.password         existing
//   sync.mix_links                "1" (default) / "0"    linked mix points travel with the library
//   sync.analysis                 "0" (default) / "1"    phrases, vocal regions and analysis cue points travel
//   sync.audio_playlists          "id,id,..."            playlists / folders whose audio this device publishes
//   sync.music_folder             directory              where audio from other devices is imported ("" = no import)
//   sync.audio_budget_mb          integer MiB, 0 = none  most the audio store in the sync folder may hold
//   sync.audio_import_budget_mb   integer MiB, 0 = none  most one run copies into the music folder
//   sync.allow_unencrypted_audio  "0" (default) / "1"    store audio without encryption when no crypto library exists
// None of the audio keys syncs between devices (their names contain "audio" or "folder").
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"
#include "deckhand/sync/sync.hpp"

namespace deckhand {

inline std::vector<std::int64_t> parse_id_list(const std::string& text) {
  std::vector<std::int64_t> out;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find(',', at);
    if (end == std::string::npos) end = text.size();
    const std::string t = text.substr(at, end - at);
    char* e = nullptr;
    const long long v = std::strtoll(t.c_str(), &e, 10);
    if (!t.empty() && e && *e == 0 && v > 0 && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    at = end + 1;
  }
  return out;
}
inline std::string format_id_list(const std::vector<std::int64_t>& ids) {
  std::string s;
  for (const std::int64_t id : ids) s += (s.empty() ? "" : ",") + std::to_string(id);
  return s;
}
inline std::uint64_t mib_setting(const std::string& v) {
  const long long n = std::atoll(v.c_str());
  return n <= 0 ? 0 : static_cast<std::uint64_t>(std::min<long long>(n, 1ll << 30)) << 20;
}

// Fills the new fields of `cfg` from the settings (folder, name and passphrase stay the caller's business).
inline void load_sync_options(Library& lib, deckhand::sync::SyncConfig& cfg) {
  cfg.sync_mix_links = lib.setting("sync.mix_links", "1") != "0";
  cfg.sync_analysis = lib.setting("sync.analysis", "0") == "1";
  cfg.audio_playlists = parse_id_list(lib.setting("sync.audio_playlists"));
  cfg.music_folder = lib.setting("sync.music_folder");
  cfg.audio_budget_bytes = mib_setting(lib.setting("sync.audio_budget_mb"));
  cfg.audio_import_budget_bytes = mib_setting(lib.setting("sync.audio_import_budget_mb"));
  cfg.allow_unencrypted_audio = lib.setting("sync.allow_unencrypted_audio", "0") == "1";
}

// True when a sync run should also call sync_audio(): something to publish or somewhere to import to.
inline bool sync_audio_wanted(const deckhand::sync::SyncConfig& cfg) { return !cfg.audio_playlists.empty() || !cfg.music_folder.empty(); }

}  // namespace deckhand
