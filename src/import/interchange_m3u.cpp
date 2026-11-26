// M3U8 playlist export and import (UTF-8 text; #EXTM3U / #EXTINF; relative or absolute paths, file:// URLs).
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "deckhand/import/interchange.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {
namespace fs = std::filesystem;

namespace {

fs::path from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string to_utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
std::string generic_utf8(const fs::path& p) {
  const auto u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
bool drive_path(const std::string& p) {
  return p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':';
}
std::string stem_of(const std::string& p) {
  const auto at = p.find_last_of("/\\");
  const std::string b = at == std::string::npos ? p : p.substr(at + 1);
  const auto dot = b.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}
std::string one_line(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  return s;
}

// A name that is safe as a file or directory name on every file system.
std::string safe_name(const std::string& name) {
  std::string out;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c < 0x20 || c == 0x7F || std::string("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string::npos) out += '_';
    else out += static_cast<char>(c);
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  std::size_t lead = 0;
  while (lead < out.size() && out[lead] == ' ') ++lead;
  out.erase(0, lead);
  if (out.size() > 100) {  // cap in bytes without cutting a UTF-8 sequence
    std::size_t n = 100;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
    out.resize(n);
  }
  const std::string up = lower(out);
  static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                   "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  for (const char* r : reserved)
    if (up == r) out = "_" + out;
  if (out.empty()) out = "playlist";
  return out;
}

// Playlists below (and including) a node, in tree order; for a plain playlist just itself.
void collect_lists(const std::vector<PlaylistNode>& all, std::int64_t id, std::vector<std::int64_t>& out) {
  for (const PlaylistNode& p : all) {
    if (p.id != id) continue;
    if (!p.is_folder) {
      out.push_back(id);
      return;
    }
    for (const PlaylistNode& c : all)
      if (c.parent == id) collect_lists(all, c.id, out);
    return;
  }
}

std::string m3u_text(Library& lib, const std::vector<std::int64_t>& list_ids, const std::string& name, const fs::path& base_dir,
                     bool relative, int& tracks) {
  std::string out = "#EXTM3U\n";
  if (!name.empty()) out += "#PLAYLIST:" + one_line(name) + "\n";
  for (const auto lid : list_ids) {
    for (const auto tid : lib.playlist_track_ids(lid)) {
      const auto t = lib.track(tid);
      if (!t) continue;
      const std::string label = t->artist.empty() ? t->title : t->artist + " - " + t->title;
      out += "#EXTINF:" + std::to_string(t->duration_ms > 0 ? (t->duration_ms + 500) / 1000 : -1) + "," + one_line(label) + "\n";
      std::string p = t->path;
      if (relative) {
        std::error_code ec;
        const fs::path abs = from_utf8(t->path);
        if (abs.is_absolute() && base_dir.is_absolute()) {
          const fs::path rel = abs.lexically_relative(base_dir);
          if (!rel.empty() && rel.is_relative()) p = generic_utf8(rel);
        }
      }
      out += p + "\n";
      ++tracks;
    }
  }
  return out;
}

bool write_text(const fs::path& target, const std::string& data, std::string& error) {
  std::error_code ec;
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
  fs::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      error = "cannot write " + to_utf8(target);
      return false;
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) {
      fs::remove(tmp, ec);
      error = "cannot write " + to_utf8(target);
      return false;
    }
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    error = "cannot replace " + to_utf8(target);
    return false;
  }
  return true;
}

void export_tree(Library& lib, const std::vector<PlaylistNode>& all, std::int64_t parent, const fs::path& dir,
                 bool relative, InterchangeExportStats& st, int depth) {
  if (depth > 64) return;
  std::set<std::string> used;  // case-insensitive names in this directory
  const auto unique = [&](const std::string& base, const std::string& ext) {
    std::string name = base;
    for (int n = 2; used.count(lower(name + ext)); ++n) name = base + " (" + std::to_string(n) + ")";
    used.insert(lower(name + ext));
    return name + ext;
  };
  for (const PlaylistNode& p : all) {
    if (p.parent != parent || !st.error.empty()) continue;
    const std::string safe = safe_name(p.name);
    if (p.is_folder) {
      const fs::path sub = dir / from_utf8(unique(safe, ""));
      std::error_code ec;
      fs::create_directories(sub, ec);
      ++st.playlists;
      export_tree(lib, all, p.id, sub, relative, st, depth + 1);
    } else {
      const fs::path file = dir / from_utf8(unique(safe, ".m3u8"));
      const std::string text = m3u_text(lib, {p.id}, p.name, dir, relative, st.tracks);
      if (!write_text(file, text, st.error)) return;
      ++st.playlists;
      ++st.files;
    }
  }
}

struct Entry {
  std::string path, title, artist;
  std::int64_t duration_ms = 0;
};

}  // namespace

InterchangeExportStats export_playlist_m3u8(Library& lib, std::int64_t playlist_id, const std::string& path, bool relative_paths) {
  InterchangeExportStats st;
  try {
    const auto all = lib.playlists();
    std::vector<std::int64_t> ids;
    collect_lists(all, playlist_id, ids);
    std::string name;
    for (const PlaylistNode& p : all)
      if (p.id == playlist_id) name = p.name;
    if (name.empty() && ids.empty()) {
      st.error = "no such playlist";
      return st;
    }
    const fs::path target = from_utf8(path);
    std::error_code ec;
    fs::path dir = target.parent_path();
    if (dir.empty()) dir = ".";
    dir = fs::absolute(dir, ec).lexically_normal();
    if (!dir.empty() && !dir.has_filename()) dir = dir.parent_path();
    const std::string text = m3u_text(lib, ids, name, dir, relative_paths, st.tracks);
    if (!write_text(target, text, st.error)) return st;
    st.playlists = 1;
    st.files = 1;
    st.ok = true;
  } catch (const std::exception& e) {
    st.ok = false;
    st.error = std::string("export failed: ") + e.what();
  }
  return st;
}

InterchangeExportStats export_all_playlists_m3u8(Library& lib, const std::string& dir, bool relative_paths) {
  InterchangeExportStats st;
  try {
    std::error_code ec;
    fs::path root = fs::absolute(from_utf8(dir), ec).lexically_normal();
    if (!root.empty() && !root.has_filename()) root = root.parent_path();
    fs::create_directories(root, ec);
    if (!fs::is_directory(root, ec)) {
      st.error = "cannot create " + dir;
      return st;
    }
    export_tree(lib, lib.playlists(), 0, root, relative_paths, st, 0);
    st.ok = st.error.empty();
  } catch (const std::exception& e) {
    st.ok = false;
    st.error = std::string("export failed: ") + e.what();
  }
  return st;
}

InterchangeStats import_playlist_m3u8(Library& lib, const std::string& path, const M3uImportOptions& opt) {
  InterchangeStats st;
  st.dry_run = opt.dry_run;
  try {
    std::string text;
    {
      std::ifstream f(from_utf8(path), std::ios::binary);
      if (!f) {
        st.error = "cannot open " + path;
        return st;
      }
      text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    if (text.compare(0, 2, "\xFF\xFE") == 0 || text.compare(0, 2, "\xFE\xFF") == 0) {
      st.error = "UTF-16 playlists are not supported; save the file as UTF-8";
      return st;
    }
    std::string norm;
    norm.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {  // CRLF and lone CR become LF
      if (text[i] == '\r') {
        norm += '\n';
        if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
      } else {
        norm += text[i];
      }
    }
    const fs::path base = fs::absolute(from_utf8(path)).parent_path();

    std::string list_name;
    std::vector<Entry> entries;
    Entry pending;
    bool have_info = false;
    std::size_t pos = 0;
    while (pos <= norm.size()) {
      auto end = norm.find('\n', pos);
      if (end == std::string::npos) end = norm.size();
      std::string line = norm.substr(pos, end - pos);
      pos = end + 1;
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
      std::size_t lead = 0;
      while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
      line.erase(0, lead);
      if (line.empty()) continue;
      if (line[0] == '#') {
        if (line.rfind("#EXTINF:", 0) == 0) {
          pending = Entry{};
          have_info = true;
          const std::string rest = line.substr(8);
          const auto comma = rest.find(',');
          const std::string dur = rest.substr(0, comma);
          char* e = nullptr;
          const double secs = std::strtod(dur.c_str(), &e);
          if (e != dur.c_str() && secs > 0 && secs < 1e6) pending.duration_ms = static_cast<std::int64_t>(secs * 1000.0 + 0.5);
          if (comma != std::string::npos) {
            std::string label = rest.substr(comma + 1);
            const auto dash = label.find(" - ");
            if (dash != std::string::npos) {
              pending.artist = label.substr(0, dash);
              pending.title = label.substr(dash + 3);
            } else {
              pending.title = label;
            }
          }
        } else if (line.rfind("#PLAYLIST:", 0) == 0) {
          list_name = line.substr(10);
        }
        continue;
      }
      Entry e = have_info ? pending : Entry{};
      have_info = false;
      std::string p = line;
      if (lower(p.substr(0, 7)) == "file://") {
        p = interchange_url_to_path(p);
      } else if (p.find("://") != std::string::npos) {
        ++st.playlist_entries_unresolved;  // a stream address
        if (st.warnings.size() < 50) st.warnings.push_back("skipped a stream address: " + p);
        continue;
      } else if (p.find('\\') != std::string::npos && (drive_path(p) || p.find('/') == std::string::npos)) {
        for (char& c : p)
          if (c == '\\') c = '/';
      }
      if (!(p[0] == '/' || drive_path(p))) {
        p = generic_utf8((base / from_utf8(p)).lexically_normal());
      } else if (p[0] == '/') {
        p = generic_utf8(from_utf8(p).lexically_normal());
      }
      e.path = p;
      entries.push_back(std::move(e));
    }

    std::string name = !opt.playlist_name.empty() ? opt.playlist_name : !list_name.empty() ? list_name : stem_of(path);
    if (name.empty()) name = "Imported playlist";

    Transaction tx(lib.db());
    std::vector<std::int64_t> ids;
    std::set<std::string> missing_seen;
    for (const Entry& e : entries) {
      ++st.tracks_in_file;
      std::error_code ec;
      const bool exists = fs::exists(from_utf8(e.path), ec);
      if (!exists) {
        ++st.tracks_missing_file;
        if (st.missing_paths.size() < 200) st.missing_paths.push_back(e.path);
      }
      auto id = lib.track_id_for_path(e.path);
      if (id) {
        ++st.tracks_matched_by_path;
      } else if (opt.add_missing_tracks) {
        NewTrack nt;
        nt.path = e.path;
        nt.title = e.title.empty() ? stem_of(e.path) : e.title;
        nt.artist = e.artist;
        nt.duration_ms = e.duration_ms;
        id = lib.upsert_track(nt);
        ++st.tracks_added;
      } else {
        ++st.playlist_entries_unresolved;
        continue;
      }
      ids.push_back(*id);
    }

    std::optional<std::int64_t> existing;
    for (const PlaylistNode& p : lib.playlists())
      if (p.parent == opt.parent && !p.is_folder && p.name == name) {
        existing = p.id;
        break;
      }
    if (existing) {
      ++st.playlists_merged;
      if (opt.merge) {
        auto have = lib.playlist_track_ids(*existing);
        std::set<std::int64_t> present(have.begin(), have.end());
        bool any = false;
        for (const auto id : ids)
          if (present.insert(id).second) {
            have.push_back(id);
            ++st.playlist_entries_added;
            any = true;
          }
        if (any) lib.set_playlist_tracks(*existing, have);
      }
    } else {
      const std::int64_t pid = lib.create_playlist(opt.parent, name, false);
      lib.set_playlist_tracks(pid, ids);
      ++st.playlists_created;
      st.playlist_entries_added += static_cast<int>(ids.size());
    }
    if (!opt.dry_run) tx.commit();
    st.ok = true;
  } catch (const std::exception& e) {
    const bool dry = st.dry_run;
    st = InterchangeStats{};
    st.dry_run = dry;
    st.error = std::string("import failed, nothing was changed: ") + e.what();
  }
  return st;
}

}  // namespace deckhand
