#include "deckhand/plugins/catalog.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace deckhand::plugins {
namespace {

std::string clean(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> f;
  std::size_t a = 0;
  for (;;) {
    const std::size_t b = line.find('\t', a);
    if (b == std::string::npos) {
      f.push_back(line.substr(a));
      break;
    }
    f.push_back(line.substr(a, b - a));
    a = b + 1;
  }
  return f;
}

bool write_atomic(const std::string& path, const std::string& body) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << body;
    if (!o) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

PluginCatalog::PluginCatalog(std::string cache_path) : path_(std::move(cache_path)) { load_blocklist(); }

void PluginCatalog::mark_blocked() {
  for (auto& p : plugins_) p.blocked = blocked(p.id);
}

bool PluginCatalog::load() {
  plugins_.clear();
  std::ifstream in(path_, std::ios::binary);
  if (!in) return false;
  std::string line;
  if (!std::getline(in, line) || line != "deckhand-plugins\t" + std::to_string(kCacheVersion)) return false;
  if (!std::getline(in, line) || line.rfind("stamp\t", 0) != 0) return false;
  const std::string stamp = line.substr(6);
  if (stamp != scan_fingerprint()) return false;
  std::vector<PluginInfo> list;
  while (std::getline(in, line)) {
    const auto f = split_tabs(line);
    if (f.size() != 11 || f[0] != "P") return false;  // damaged: rescan
    PluginInfo p;
    p.id = f[1];
    p.format = f[2] == "1" ? Format::Vst3 : Format::AudioUnit;
    p.name = f[3];
    p.maker = f[4];
    p.type = f[5];
    p.subtype = f[6];
    p.manufacturer = f[7];
    try {
      p.version = (std::uint32_t)std::stoul(f[8]);
      p.type_code = (std::uint32_t)std::stoul(f[9]);
      const std::string rest = f[10];  // "<subtype_code>,<manufacturer_code>,<midi>"
      std::stringstream ss(rest);
      std::string a, b, c;
      std::getline(ss, a, ',');
      std::getline(ss, b, ',');
      std::getline(ss, c, ',');
      p.subtype_code = (std::uint32_t)std::stoul(a);
      p.manufacturer_code = (std::uint32_t)std::stoul(b);
      p.has_midi = c == "1";
    } catch (...) {
      return false;
    }
    list.push_back(std::move(p));
  }
  stamp_ = stamp;
  plugins_ = std::move(list);
  mark_blocked();
  return true;
}

void PluginCatalog::scan() {
  plugins_ = scan_installed();
  stamp_ = scan_fingerprint();
  mark_blocked();
  save();
}

bool PluginCatalog::refresh(bool force) {
  if (!force && load()) return false;
  scan();
  return true;
}

bool PluginCatalog::save() const {
  std::ostringstream o;
  o << "deckhand-plugins\t" << kCacheVersion << "\nstamp\t" << clean(stamp_) << "\n";
  for (const auto& p : plugins_)
    o << "P\t" << clean(p.id) << '\t' << (p.format == Format::Vst3 ? 1 : 0) << '\t' << clean(p.name) << '\t' << clean(p.maker) << '\t'
      << clean(p.type) << '\t' << clean(p.subtype) << '\t' << clean(p.manufacturer) << '\t' << p.version << '\t' << p.type_code << '\t'
      << p.subtype_code << ',' << p.manufacturer_code << ',' << (p.has_midi ? 1 : 0) << "\n";
  return write_atomic(path_, o.str());
}

std::vector<PluginInfo> PluginCatalog::usable() const {
  std::vector<PluginInfo> out;
  for (const auto& p : plugins_)
    if (!p.blocked) out.push_back(p);
  return out;
}

const PluginInfo* PluginCatalog::find(const std::string& id) const {
  for (const auto& p : plugins_)
    if (p.id == id) return &p;
  return nullptr;
}

bool PluginCatalog::blocked(const std::string& id) const {
  return std::any_of(block_.begin(), block_.end(), [&](const BlockEntry& b) { return b.id == id; });
}

void PluginCatalog::block(const std::string& id, const std::string& reason) {
  if (!blocked(id)) block_.push_back({clean(id), clean(reason), (long long)std::time(nullptr)});
  mark_blocked();
  save_blocklist();
}

void PluginCatalog::unblock(const std::string& id) {
  block_.erase(std::remove_if(block_.begin(), block_.end(), [&](const BlockEntry& b) { return b.id == id; }), block_.end());
  mark_blocked();
  save_blocklist();
}

void PluginCatalog::load_blocklist() {
  block_.clear();
  std::ifstream in(path_ + ".blocklist", std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    const auto f = split_tabs(line);
    if (f.size() != 3 || f[0].empty()) continue;
    long long when = 0;
    try {
      when = std::stoll(f[2]);
    } catch (...) {
    }
    block_.push_back({f[0], f[1], when});
  }
}

bool PluginCatalog::save_blocklist() const {
  std::ostringstream o;
  for (const auto& b : block_) o << b.id << '\t' << b.reason << '\t' << b.when << "\n";
  return write_atomic(path_ + ".blocklist", o.str());
}

}  // namespace deckhand::plugins
