// Device backup / restore / verify. See export_device.hpp.
#include "deckhand/export/export_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "deckhand/export/export.hpp"
#include "deckhand/export/export_check.hpp"

namespace deckhand::usbexport {
namespace fs = std::filesystem;
namespace {

// ---- SHA-256 -------------------------------------------------------------------------------------------------

struct Sha256 {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::uint8_t buf[64];
  std::size_t fill = 0;
  std::uint64_t total = 0;
  static std::uint32_t rr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const std::uint8_t* p) {
    static const std::uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) | (std::uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t t1 = hh + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      const std::uint32_t t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const void* data, std::size_t n) {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    total += n;
    while (n) {
      const std::size_t k = std::min(n, sizeof buf - fill);
      std::copy(p, p + k, buf + fill);
      fill += k; p += k; n -= k;
      if (fill == 64) { block(buf); fill = 0; }
    }
  }
  std::string hex() {
    const std::uint64_t bits = total * 8;
    const std::uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (fill != 56) update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 64);
  }
};

bool hash_file(const fs::path& p, std::string& hex, std::uint64_t& size, std::string& error) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    error = "cannot read " + p.string();
    return false;
  }
  Sha256 s;
  std::vector<char> buf(1 << 20);
  size = 0;
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = in.gcount();
    if (n > 0) { s.update(buf.data(), static_cast<std::size_t>(n)); size += static_cast<std::uint64_t>(n); }
  }
  if (in.bad()) {
    error = "read error in " + p.string();
    return false;
  }
  hex = s.hex();
  return true;
}

bool copy_file_to(const fs::path& from, const fs::path& to, std::string& error) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    error = "cannot copy " + from.string() + " to " + to.string() + ": " + ec.message();
    return false;
  }
  return true;
}

std::string now_stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
  return b;
}

std::string stamp_pretty(const std::string& s) {  // 20260101-120000 -> 2026-01-01 12:00:00
  if (s.size() != 15) return s;
  return s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2) + " " + s.substr(9, 2) + ":" + s.substr(11, 2) + ":" + s.substr(13, 2);
}

bool safe_rel(const std::string& rel) {
  if (rel.empty() || rel[0] == '/' || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos) return false;
  std::size_t i = 0;
  while (i <= rel.size()) {
    const std::size_t j = rel.find('/', i);
    const std::string part = rel.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (part.empty() || part == "." || part == "..") return false;
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return true;
}

fs::path under(const fs::path& root, const std::string& rel) { return root / fs::path(rel); }

bool is_library_path(const std::string& rel) {
  return rel == "PIONEER/rekordbox/export.pdb" || rel == "PIONEER/rekordbox/exportExt.pdb" || rel.rfind("PIONEER/USBANLZ/", 0) == 0;
}

bool library_exists(const fs::path& stick) {
  std::error_code ec;
  if (fs::exists(stick / "PIONEER/rekordbox/export.pdb", ec) || fs::exists(stick / "PIONEER/rekordbox/exportExt.pdb", ec)) return true;
  const fs::path a = stick / "PIONEER/USBANLZ";
  if (fs::is_directory(a, ec)) {
    for (auto it = fs::recursive_directory_iterator(a, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->is_regular_file(ec)) return true;
  }
  return false;
}

void json_str(std::string& o, const std::string& s) {
  o += '"';
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  o += '"';
}

}  // namespace

std::string sha256_hex_of_file(const std::string& path, std::string& error) {
  std::string hex;
  std::uint64_t size = 0;
  return hash_file(fs::path(path), hex, size, error) ? hex : std::string();
}

// ---- backup --------------------------------------------------------------------------------------------------

namespace {
constexpr const char* kManifestHeader = "DECKHAND-DEVICE-BACKUP 1";

bool write_manifest(const fs::path& file, const std::string& created, const std::vector<ManifestEntry>& entries, std::string& error) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << kManifestHeader << "\n" << "created\t" << created << "\n";
  for (const auto& e : entries) out << (e.audio ? "audio" : "library") << "\t" << e.sha256 << "\t" << e.size << "\t" << e.path << "\n";
  out.flush();
  if (!out) {
    error = "cannot write " + file.string();
    return false;
  }
  return true;
}
}  // namespace

bool read_backup_manifest(const std::string& backup_path, std::vector<ManifestEntry>& entries, std::string& error) {
  entries.clear();
  std::ifstream in(fs::path(backup_path) / "manifest.txt", std::ios::binary);
  if (!in) {
    error = "this folder has no manifest.txt: it is not a finished backup";
    return false;
  }
  std::string line;
  std::getline(in, line);
  while (!line.empty() && (line.back() == '\r')) line.pop_back();
  if (line != kManifestHeader) {
    error = "unknown backup manifest format";
    return false;
  }
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("created\t", 0) == 0 || line.empty()) continue;
    std::istringstream ss(line);
    std::string kind, hash, size, path;
    if (!std::getline(ss, kind, '\t') || !std::getline(ss, hash, '\t') || !std::getline(ss, size, '\t') || !std::getline(ss, path)) {
      error = "damaged manifest line";
      return false;
    }
    ManifestEntry e;
    e.audio = kind == "audio";
    e.sha256 = hash;
    try { e.size = std::stoull(size); } catch (...) { error = "damaged manifest line"; return false; }
    e.path = path;
    if ((kind != "audio" && kind != "library") || hash.size() != 64 || !safe_rel(path)) {
      error = "damaged or unsafe manifest line";
      return false;
    }
    entries.push_back(std::move(e));
  }
  return true;
}

bool backup_device(const std::string& stick_path, const std::string& backup_dir, const BackupOptions& options,
                   std::string& backup_path, std::string& error) {
  error.clear();
  backup_path.clear();
  fs::path created_dir, final_dir;
  try {
    const fs::path stick = stick_path;
    std::error_code ec;
    if (!fs::exists(stick / "PIONEER/rekordbox/export.pdb", ec)) {
      error = "no exported library found on " + stick_path + " (PIONEER/rekordbox/export.pdb is missing)";
      return false;
    }
    ReadExport db;
    std::string rerr;
    if (!read_export(stick_path, db, rerr)) {
      error = "the library on the stick cannot be read: " + rerr;
      return false;
    }
    std::vector<ManifestEntry> list;
    auto add = [&](const std::string& rel, bool audio) {
      ManifestEntry e;
      e.path = rel;
      e.audio = audio;
      list.push_back(std::move(e));
    };
    add("PIONEER/rekordbox/export.pdb", false);
    if (fs::exists(stick / "PIONEER/rekordbox/exportExt.pdb", ec)) add("PIONEER/rekordbox/exportExt.pdb", false);
    const fs::path anlz = stick / "PIONEER/USBANLZ";
    std::vector<std::string> a;
    if (fs::is_directory(anlz, ec)) {
      for (auto it = fs::recursive_directory_iterator(anlz, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) a.push_back(fs::relative(it->path(), stick).generic_string());
    }
    std::sort(a.begin(), a.end());
    for (const auto& r : a) add(r, false);
    if (options.include_audio) {
      std::set<std::string> seen;
      for (const auto& t : db.tracks) {
        std::string rel = t.file_path;
        if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        if (rel.empty() || !safe_rel(rel) || !seen.insert(rel).second) continue;
        if (fs::is_regular_file(under(stick, rel), ec)) add(rel, true);
      }
    }

    const fs::path dir = backup_dir;
    fs::create_directories(dir, ec);
    if (ec) {
      error = "cannot create backup folder " + backup_dir + ": " + ec.message();
      return false;
    }
    const std::string stamp = options.stamp.empty() ? now_stamp() : options.stamp;
    std::string name = "DeckHand-device-" + stamp;
    for (int n = 2; fs::exists(dir / name, ec); ++n) name = "DeckHand-device-" + stamp + "-" + std::to_string(n);
    final_dir = dir / name;
    created_dir = dir / (name + ".dhtmp");
    fs::create_directories(created_dir / "files", ec);
    if (ec) {
      error = "cannot create " + created_dir.string() + ": " + ec.message();
      return false;
    }
    // space check on the backup volume
    std::uint64_t need = 0;
    for (const auto& e : list) need += fs::file_size(under(stick, e.path), ec);
    const auto sp = fs::space(dir, ec);
    if (!ec && need + (1u << 20) > sp.available) {
      error = "not enough free space for the backup (need about " + std::to_string(need / (1024 * 1024) + 1) + " MiB, " +
              std::to_string(sp.available / (1024 * 1024)) + " MiB free)";
      fs::remove_all(created_dir, ec);
      return false;
    }
    for (auto& e : list) {
      std::string herr, hex1, hex2;
      std::uint64_t s1 = 0, s2 = 0;
      if (!hash_file(under(stick, e.path), hex1, s1, herr)) { error = herr; fs::remove_all(created_dir, ec); return false; }
      const fs::path dst = under(created_dir / "files", e.path);
      if (!copy_file_to(under(stick, e.path), dst, error)) { fs::remove_all(created_dir, ec); return false; }
      if (!hash_file(dst, hex2, s2, herr) || hex1 != hex2 || s1 != s2) {
        error = "backup copy of " + e.path + " does not match the original (stick or backup disk problem)";
        fs::remove_all(created_dir, ec);
        return false;
      }
      e.sha256 = hex1;
      e.size = s1;
    }
    if (!write_manifest(created_dir / "manifest.txt", stamp_pretty(stamp), list, error)) {
      fs::remove_all(created_dir, ec);
      return false;
    }
    fs::rename(created_dir, final_dir, ec);
    if (ec) {
      error = "cannot finish the backup folder: " + ec.message();
      fs::remove_all(created_dir, ec);
      return false;
    }
    backup_path = final_dir.string();
    return true;
  } catch (const std::exception& e) {
    error = std::string("backup failed: ") + e.what();
    std::error_code ec;
    if (!created_dir.empty()) fs::remove_all(created_dir, ec);
    return false;
  }
}

std::vector<BackupInfo> list_device_backups(const std::string& backup_dir) {
  std::vector<BackupInfo> out;
  std::error_code ec;
  if (!fs::is_directory(backup_dir, ec)) return out;
  for (auto it = fs::directory_iterator(backup_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (!it->is_directory(ec) || name.rfind("DeckHand-device-", 0) != 0 || name.size() > 4 && name.substr(name.size() - 6) == ".dhtmp") continue;
    BackupInfo b;
    b.path = it->path().string();
    b.name = name;
    std::vector<ManifestEntry> es;
    std::string err;
    b.valid = read_backup_manifest(b.path, es, err);
    for (const auto& e : es) {
      ++b.files;
      b.bytes += e.size;
      b.has_audio = b.has_audio || e.audio;
    }
    std::ifstream in(it->path() / "manifest.txt");
    std::string l;
    while (std::getline(in, l))
      if (l.rfind("created\t", 0) == 0) { b.created = l.substr(8); while (!b.created.empty() && b.created.back() == '\r') b.created.pop_back(); }
    out.push_back(std::move(b));
  }
  std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.name > b.name; });
  return out;
}

bool verify_device_backup(const std::string& backup_path, std::vector<std::string>& problems) {
  problems.clear();
  std::vector<ManifestEntry> es;
  std::string err;
  if (!read_backup_manifest(backup_path, es, err)) {
    problems.push_back(err);
    return false;
  }
  for (const auto& e : es) {
    const fs::path p = under(fs::path(backup_path) / "files", e.path);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { problems.push_back("backup file missing: " + e.path); continue; }
    std::string hex, herr;
    std::uint64_t size = 0;
    if (!hash_file(p, hex, size, herr)) { problems.push_back(herr); continue; }
    if (size != e.size) problems.push_back("backup file has the wrong size: " + e.path);
    else if (hex != e.sha256) problems.push_back("backup file is damaged (checksum differs): " + e.path);
  }
  return problems.empty();
}

// ---- restore -------------------------------------------------------------------------------------------------

bool restore_device(const std::string& backup_path, const std::string& stick_path, const RestoreOptions& options,
                    RestoreResult& result, std::string& error) {
  error.clear();
  result = {};
  std::vector<fs::path> staged;
  try {
    std::vector<ManifestEntry> es;
    if (!read_backup_manifest(backup_path, es, error)) return false;
    std::vector<std::string> bad;
    if (!verify_device_backup(backup_path, bad)) {
      error = "the backup does not verify (" + bad.front() + "); nothing was written";
      return false;
    }
    const fs::path stick = stick_path, files = fs::path(backup_path) / "files";
    std::error_code ec;
    if (!fs::is_directory(stick, ec)) {
      error = "the destination does not exist: " + stick_path;
      return false;
    }
    if (library_exists(stick) && !options.force) {
      error = "the stick already has a library (PIONEER folder); restoring would replace it. Use force to replace it.";
      return false;
    }
    std::vector<const ManifestEntry*> todo;
    std::uint64_t need = 0;
    for (const auto& e : es) {
      if (e.audio && !options.restore_audio) continue;
      const fs::path dst = under(stick, e.path);
      if (e.audio && fs::exists(dst, ec)) {
        std::string hex, herr;
        std::uint64_t size = 0;
        if (hash_file(dst, hex, size, herr) && hex == e.sha256) continue;  // already there
        if (!options.force) {
          error = "an audio file on the stick differs from the backup and was not replaced: " + e.path + " (use force)";
          return false;
        }
      }
      todo.push_back(&e);
      need += e.size;
    }
    const auto sp = fs::space(stick, ec);
    if (!ec && need + (1u << 20) > sp.available) {
      error = "not enough free space on the stick (need about " + std::to_string(need / (1024 * 1024) + 1) + " MiB, " +
              std::to_string(sp.available / (1024 * 1024)) + " MiB free)";
      return false;
    }
    // stage + verify every file under a temporary name first
    for (const ManifestEntry* e : todo) {
      const fs::path dst = under(stick, e->path), tmp = fs::path(dst.string() + ".dhtmp");
      staged.push_back(tmp);
      if (!copy_file_to(under(files, e->path), tmp, error)) throw std::runtime_error(error);
      std::string hex, herr;
      std::uint64_t size = 0;
      if (!hash_file(tmp, hex, size, herr) || hex != e->sha256 || size != e->size) {
        error = "copy of " + e->path + " to the stick does not match the backup (stick problem?)";
        throw std::runtime_error(error);
      }
    }
    // commit: library files last so a half restore never leaves a database without its audio
    std::vector<const ManifestEntry*> order;
    for (const ManifestEntry* e : todo) if (e->audio) order.push_back(e);
    for (const ManifestEntry* e : todo) if (!e->audio && e->path.find("USBANLZ") != std::string::npos) order.push_back(e);
    for (const ManifestEntry* e : todo) if (!e->audio && e->path.find("USBANLZ") == std::string::npos) order.push_back(e);
    for (const ManifestEntry* e : order) {
      const fs::path dst = under(stick, e->path), tmp = fs::path(dst.string() + ".dhtmp");
      fs::remove(dst, ec);
      fs::rename(tmp, dst, ec);
      if (ec) {
        error = "cannot move " + e->path + " into place: " + ec.message();
        throw std::runtime_error(error);
      }
      ++result.files;
      result.bytes += e->size;
    }
    staged.clear();
    // force: library files of the replaced export that the backup does not contain
    if (options.force) {
      std::set<std::string> keep;
      for (const auto& e : es) keep.insert(e.path);
      std::vector<fs::path> stale;
      const fs::path a = stick / "PIONEER/USBANLZ";
      if (fs::is_directory(a, ec))
        for (auto it = fs::recursive_directory_iterator(a, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
          if (it->is_regular_file(ec) && !keep.count(fs::relative(it->path(), stick).generic_string())) stale.push_back(it->path());
      for (const char* n : {"PIONEER/rekordbox/export.pdb", "PIONEER/rekordbox/exportExt.pdb"})
        if (!keep.count(n) && fs::exists(stick / n, ec)) stale.push_back(stick / n);
      for (const auto& p : stale) {
        if (!is_library_path(fs::relative(p, stick).generic_string())) continue;
        if (fs::remove(p, ec)) ++result.removed_stale;
      }
    }
    // final read-back of what was restored
    for (const ManifestEntry* e : order) {
      std::string hex, herr;
      std::uint64_t size = 0;
      if (!hash_file(under(stick, e->path), hex, size, herr) || hex != e->sha256) {
        error = "after restoring, " + e->path + " does not match the backup";
        return false;
      }
    }
    return true;
  } catch (const std::exception& ex) {
    if (error.empty()) error = std::string("restore failed: ") + ex.what();
    std::error_code ec;
    for (const auto& t : staged) fs::remove(t, ec);
    return false;
  }
}

// ---- verify --------------------------------------------------------------------------------------------------

std::string DeviceReport::text() const {
  std::string s;
  if (ok && problems.empty()) {
    s = "This stick looks fine: " + std::to_string(tracks) + " tracks and " + std::to_string(playlists) + " playlists read back, " +
        std::to_string(files_checked) + " files present.\n";
    return s;
  }
  for (const auto& p : problems) s += std::string(p.error ? "PROBLEM: " : "note: ") + p.message + "\n";
  s += ok ? "The stick should work, but see the notes above.\n" : "Do not rely on this stick until the problems above are fixed (export again).\n";
  return s;
}

std::string DeviceReport::to_json() const {
  std::string o = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"tracks\":" + std::to_string(tracks) +
                  ",\"playlists\":" + std::to_string(playlists) + ",\"files_checked\":" + std::to_string(files_checked) + ",\"problems\":[";
  for (std::size_t i = 0; i < problems.size(); ++i) {
    if (i) o += ',';
    o += std::string("{\"severity\":\"") + (problems[i].error ? "error" : "warning") + "\",\"code\":";
    json_str(o, problems[i].code);
    o += ",\"message\":";
    json_str(o, problems[i].message);
    o += '}';
  }
  o += "]}";
  return o;
}

namespace {
std::string plain(const Issue& i) {
  if (i.severity == Severity::Warning) return i.message;
  const std::string& c = i.code;
  if (c == "audio.missing") return "A track's audio file is missing from the stick: " + i.message;
  if (c == "audio.size") return "A track's audio file has the wrong size (partly copied or damaged): " + i.message;
  if (c == "anlz.missing_DAT" || c == "anlz.missing_EXT") return "A track's analysis file (waveform/beat grid) is missing: " + i.message;
  if (c.rfind("anlz.", 0) == 0) return "A track's analysis file is damaged: " + i.message;
  if (c.rfind("pdb.", 0) == 0) return "The track database is damaged: " + i.message;
  if (c.rfind("fk.", 0) == 0 || c.rfind("dup.", 0) == 0) return "The track database is inconsistent: " + i.message;
  if (c.rfind("fat.", 0) == 0) return "A file name will not work on a FAT/exFAT stick: " + i.message;
  if (c == "export.leftover") return "A half-finished export left temporary files on the stick: " + i.message;
  return i.message;
}
}  // namespace

bool verify_device(const std::string& stick_path, DeviceReport& report, const VerifyOptions& options) {
  report = {};
  auto add = [&](bool err, const std::string& code, const std::string& msg) { report.problems.push_back({err, code, msg}); };
  try {
    std::error_code ec;
    if (!fs::exists(fs::path(stick_path) / "PIONEER/rekordbox/export.pdb", ec)) {
      add(true, "library.missing", "There is no exported library on this stick (PIONEER/rekordbox/export.pdb is missing).");
      return false;
    }
    ReadExport db;
    std::string err;
    if (!read_export(stick_path, db, err)) {
      add(true, "library.unreadable", "The track database cannot be read back: " + err);
      return false;
    }
    report.tracks = db.tracks.size();
    for (const auto& p : db.playlists) if (!p.is_folder) ++report.playlists;
    ValidationReport v;
    ValidateOptions vo;
    vo.check_audio = options.check_audio;
    validate_export(stick_path, v, vo);
    report.files_checked = v.files_checked;
    for (const auto& i : v.issues) add(i.severity == Severity::Error, i.code, plain(i));
    if (report.tracks == 0) add(false, "library.empty", "The library holds no tracks.");
    report.ok = std::none_of(report.problems.begin(), report.problems.end(), [](const DeviceProblem& p) { return p.error; });
    return report.ok;
  } catch (const std::exception& e) {
    add(true, "verify.failed", std::string("The check itself failed: ") + e.what());
    report.ok = false;
    return false;
  }
}

}  // namespace deckhand::usbexport
