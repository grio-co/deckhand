#include "deckhand/app/plugin_service.hpp"

#include <filesystem>

#include "deckhand/audio/plugin_host.hpp"
#include "deckhand/plugins/catalog.hpp"
#include "deckhand/plugins/plugin.hpp"

namespace deckhand {

PluginService& plugin_service() {
  static PluginService s;
  return s;
}

PluginService::~PluginService() { wait(); }

void PluginService::set_dir(const std::string& dir) {
  std::lock_guard<std::mutex> lk(mx_);
  dir_ = dir;
  plugin_host_set_dir(dir);
  st_.supported = plugins::hosting_supported();
}

void PluginService::wait() {
  std::thread t;
  {
    std::lock_guard<std::mutex> lk(mx_);
    if (thread_.joinable()) t = std::move(thread_);
  }
  if (t.joinable()) t.join();
}

PluginScanStatus PluginService::status() const {
  std::lock_guard<std::mutex> lk(mx_);
  return st_;
}

std::vector<PluginRow> PluginService::list() {
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(mx_);
    dir = dir_;
  }
  std::vector<PluginRow> out;
  if (dir.empty() || !plugins::hosting_supported()) return out;
  plugins::PluginCatalog cat(dir + "/plugins.cache");
  if (!cat.load()) {
    rescan(false);
    return out;
  }
  for (const plugins::PluginInfo& p : cat.plugins()) {
    PluginRow r;
    r.id = p.id;
    r.name = p.name;
    r.maker = p.maker;
    r.format = p.format == plugins::Format::AudioUnit ? "au" : "vst3";
    r.blocked = cat.blocked(p.id);
    if (r.blocked)
      for (const auto& b : cat.blocklist())
        if (b.id == p.id) r.reason = b.reason;
    r.vetted = !r.blocked && plugin_host_is_vetted(p.id);
    out.push_back(std::move(r));
  }
  return out;
}

bool PluginService::vet(const std::string& id, std::string& error) {
  plugins::PluginInfo info;
  return plugin_host_vet(id, info, error);
}

void PluginService::rescan(bool force) {
  wait();  // a finished earlier run
  std::lock_guard<std::mutex> lk(mx_);
  if (st_.running || dir_.empty() || !plugins::hosting_supported()) return;
  st_.running = true;
  st_.done = st_.total = 0;
  st_.current.clear();
  st_.blocked_now = 0;
  thread_ = std::thread([this, force] { run(force); });
}

void PluginService::run(bool force) {
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(mx_);
    dir = dir_;
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  plugins::PluginCatalog cat(dir + "/plugins.cache");
  if (force) cat.scan();
  else cat.refresh(false);
  const std::vector<plugins::PluginInfo> usable = cat.usable();
  {
    std::lock_guard<std::mutex> lk(mx_);
    st_.total = static_cast<int>(usable.size());
  }
  int blocked = 0;
  for (const plugins::PluginInfo& p : usable) {
    {
      std::lock_guard<std::mutex> lk(mx_);
      st_.current = p.name;
    }
    if (!plugin_host_is_vetted(p.id)) {
      plugins::PluginInfo got;
      std::string err;
      if (!plugin_host_vet(p.id, got, err)) ++blocked;
    }
    std::lock_guard<std::mutex> lk(mx_);
    ++st_.done;
  }
  std::lock_guard<std::mutex> lk(mx_);
  st_.running = false;
  st_.current.clear();
  st_.blocked_now = blocked;
  st_.last_message = std::to_string(usable.size()) + " plug-ins, " + std::to_string(blocked) + " blocked";
  ++st_.generation;
}

}  // namespace deckhand
