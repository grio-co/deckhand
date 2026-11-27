#include "deckhand/audio/plugin_host.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "deckhand/audio/ipc.hpp"
#include "deckhand/plugins/catalog.hpp"
#include "deckhand/plugins/probe.hpp"

namespace deckhand {
namespace {
std::mutex g_mx;      // the settings below
std::mutex g_vet_mx;  // one probe at a time (not held by the settings accessors)
std::string g_dir, g_probe;

bool probed_ok(const std::string& dir, const plugins::PluginInfo& p) {
  std::ifstream in(dir + "/plugins.probed", std::ios::binary);
  std::string line;
  const std::string want = p.id + "\t" + std::to_string(p.version);
  while (std::getline(in, line))
    if (line == want) return true;
  return false;
}
}  // namespace

void plugin_host_set_dir(const std::string& dir) {
  std::lock_guard<std::mutex> lk(g_mx);
  g_dir = dir;
}
std::string plugin_host_dir() {
  std::lock_guard<std::mutex> lk(g_mx);
  return g_dir;
}
void plugin_host_set_probe_exe(const std::string& exe) {
  std::lock_guard<std::mutex> lk(g_mx);
  g_probe = exe;
}
std::string plugin_host_probe_exe() {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!g_probe.empty()) return g_probe;
  if (const char* e = std::getenv("DECKHAND_PLUGIN_PROBE")) return e;
  const std::string d = ipc::executable_dir();
  if (d.empty()) return {};
#if defined(_WIN32)
  return d + "\\deckhand-plugin-probe.exe";
#else
  return d + "/deckhand-plugin-probe";
#endif
}

bool plugin_host_vet(const std::string& id, plugins::PluginInfo& out, std::string& error) {
  if (!plugins::hosting_supported()) return error = "plug-ins are not available in this build", false;
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    dir = g_dir;
  }
  if (dir.empty()) return error = "the plug-in folder is not set", false;
  const std::string exe = plugin_host_probe_exe();
  std::lock_guard<std::mutex> lk(g_vet_mx);
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  plugins::PluginCatalog cat(dir + "/plugins.cache");
  cat.refresh(false);
  const plugins::PluginInfo* p = cat.find(id);
  if (!p) return error = "plug-in not found: " + id, false;
  if (cat.blocked(id)) {
    error = "the plug-in is blocked";
    for (const auto& b : cat.blocklist())
      if (b.id == id && !b.reason.empty()) error += " (" + b.reason + ")";
    return false;
  }
  if (!probed_ok(dir, *p)) {
    std::error_code fe;
    if (exe.empty() || !std::filesystem::exists(std::filesystem::u8path(exe), fe)) return error = "the plug-in probe helper was not found", false;
    const plugins::ProbeResult r = plugins::probe_out_of_process(exe, id, 8000);
    if (!r.ok) {
      cat.block(id, r.error);
      error = "the plug-in failed its safety probe (" + r.error + ") and was blocked";
      return false;
    }
    std::ofstream o(dir + "/plugins.probed", std::ios::binary | std::ios::app);
    o << p->id << '\t' << p->version << '\n';
  }
  out = *p;
  return true;
}

bool plugin_host_is_vetted(const std::string& id) {
  if (!plugins::hosting_supported()) return false;
  const std::string dir = plugin_host_dir();
  if (dir.empty()) return false;
  plugins::PluginCatalog cat(dir + "/plugins.cache");
  cat.load();
  const plugins::PluginInfo* p = cat.find(id);
  return p && !cat.blocked(id) && probed_ok(dir, *p);
}

}  // namespace deckhand
