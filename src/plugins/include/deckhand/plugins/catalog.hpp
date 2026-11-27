// Plug-in catalogue: scan results cached in a text file with a version stamp, plus a persistent blocklist of
// plug-ins that failed (crashed, hung or misbehaved in a probe).
#pragma once
#include <string>
#include <vector>

#include "deckhand/plugins/plugin.hpp"

namespace deckhand::plugins {

struct BlockEntry {
  std::string id, reason;
  long long when = 0;  // unix seconds
};

class PluginCatalog {
 public:
  // Cache lives in `cache_path`; the blocklist beside it in `cache_path + ".blocklist"`.
  explicit PluginCatalog(std::string cache_path);

  bool load();                       // reads the cache; false when missing, unreadable or the stamp is stale
  void scan();                       // rescans the system (no instantiation) and writes the cache
  bool refresh(bool force = false);  // load(), else scan(); true when it rescanned
  bool save() const;

  const std::vector<PluginInfo>& plugins() const { return plugins_; }  // `blocked` is filled in
  std::vector<PluginInfo> usable() const;                              // minus blocked
  const PluginInfo* find(const std::string& id) const;

  bool blocked(const std::string& id) const;
  void block(const std::string& id, const std::string& reason);  // persisted at once
  void unblock(const std::string& id);
  const std::vector<BlockEntry>& blocklist() const { return block_; }

  static constexpr int kCacheVersion = 1;

 private:
  void mark_blocked();
  void load_blocklist();
  bool save_blocklist() const;
  std::string path_;
  std::string stamp_;
  std::vector<PluginInfo> plugins_;
  std::vector<BlockEntry> block_;
};

}  // namespace deckhand::plugins
