#include "deckhand/app/chain_presets.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace deckhand {
namespace {
std::mutex g_mx;
std::string g_dir;
namespace fs = std::filesystem;
fs::path file_of(const std::string& clean) { return fs::u8path(g_dir) / fs::u8path(clean + ".json"); }
}  // namespace

void chain_presets_set_dir(const std::string& dir) {
  std::lock_guard<std::mutex> lk(g_mx);
  g_dir = dir;
}
std::string chain_presets_dir() {
  std::lock_guard<std::mutex> lk(g_mx);
  return g_dir;
}

std::string chain_preset_clean_name(const std::string& name) {
  std::string o;
  for (const unsigned char c : name) {
    if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') continue;
    o += static_cast<char>(c);
  }
  const auto a = o.find_first_not_of(' ');
  if (a == std::string::npos) return {};
  o = o.substr(a, o.find_last_not_of(' ') - a + 1);
  if (o.size() > 60) {  // never cut a UTF-8 sequence in half
    std::size_t n = 60;
    while (n > 0 && (static_cast<unsigned char>(o[n]) & 0xC0) == 0x80) --n;
    o.resize(n);
  }
  if (o == "." || o == "..") return {};
  return o;
}

std::vector<std::string> chain_presets_list() {
  std::lock_guard<std::mutex> lk(g_mx);
  std::vector<std::string> out;
  if (g_dir.empty()) return out;
  std::error_code ec;
  for (fs::directory_iterator it(fs::u8path(g_dir), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2) || it->path().extension() != ".json") continue;
    const auto u = it->path().stem().u8string();
    out.emplace_back(u.begin(), u.end());
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool chain_preset_save(const std::string& name, const std::string& json, std::string& error) {
  const std::string clean = chain_preset_clean_name(name);
  if (clean.empty()) return error = "give the preset a name", false;
  std::lock_guard<std::mutex> lk(g_mx);
  if (g_dir.empty()) return error = "no folder for presets", false;
  std::error_code ec;
  fs::create_directories(fs::u8path(g_dir), ec);
  const fs::path tmp = file_of(clean + ".tmp");
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return error = "cannot write the preset", false;
    o << json;
    if (!o) return error = "cannot write the preset", false;
  }
  fs::rename(tmp, file_of(clean), ec);
  if (ec) {
    fs::remove(tmp, ec);
    return error = "cannot write the preset", false;
  }
  return true;
}

bool chain_preset_read(const std::string& name, std::string& json, std::string& error) {
  const std::string clean = chain_preset_clean_name(name);
  if (clean.empty()) return error = "give the preset a name", false;
  std::lock_guard<std::mutex> lk(g_mx);
  std::ifstream in(file_of(clean), std::ios::binary);
  if (!in) return error = "no such preset", false;
  std::ostringstream ss;
  ss << in.rdbuf();
  json = ss.str();
  return true;
}

bool chain_preset_delete(const std::string& name, std::string& error) {
  const std::string clean = chain_preset_clean_name(name);
  if (clean.empty()) return error = "give the preset a name", false;
  std::lock_guard<std::mutex> lk(g_mx);
  std::error_code ec;
  if (!fs::remove(file_of(clean), ec) || ec) return error = "no such preset", false;
  return true;
}

}  // namespace deckhand
