// export.pdb writer (DeviceSQL pages) and the USB export driver. Layout per the public DJ Link Ecosystem
// Analysis "rekordbox export" page and the crate-digger Kaitai description; guesses are in docs/EXPORT.md.
#include "deckhand/export/export.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "anlz_writer.hpp"
#include "bytes.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand::usbexport {
namespace {

using namespace detail;
namespace fs = std::filesystem;

constexpr std::size_t kPage = 4096;
constexpr std::size_t kHeap = 0x28;
constexpr std::uint32_t kNumTables = 20;
constexpr std::uint32_t kNoNext = 0x03FFFFFF;

enum TableType : std::uint32_t {
  kTracks = 0, kGenres = 1, kArtists = 2, kAlbums = 3, kLabels = 4, kKeys = 5, kColors = 6,
  kPlaylistTree = 7, kPlaylistEntries = 8, kArtwork = 13, kColumns = 16
};

// ---- DeviceSQL strings -------------------------------------------------------------------------------------

Bytes dsql(const std::string& s, std::size_t cap = 254) {
  Bytes b;
  const std::string c = cap_utf8(s, cap);
  const bool ascii = std::all_of(c.begin(), c.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
  if (ascii && c.size() <= 126) {
    put_u8(b, ((c.size() + 1) << 1) | 1);
    b.insert(b.end(), c.begin(), c.end());
  } else if (ascii) {
    put_u8(b, 0x40);
    put_le16(b, static_cast<std::uint32_t>(c.size() + 4));
    put_u8(b, 0);
    b.insert(b.end(), c.begin(), c.end());
  } else {
    const std::u16string u = utf8_to_utf16(c);
    put_u8(b, 0x90);
    put_le16(b, static_cast<std::uint32_t>(u.size() * 2 + 4));
    put_u8(b, 0);
    for (char16_t ch : u) put_le16(b, ch);
  }
  return b;
}

void pad4(Bytes& b) {
  while (b.size() % 4) b.push_back(0);
}

// ---- rows --------------------------------------------------------------------------------------------------

struct Row {
  Bytes data;
  bool shift = false;  // patch index_shift (offset 2) with 0x20 * position in page
};

Row id_name_row(std::uint32_t id, const std::string& name) {
  Row r;
  put_le32(r.data, id);
  Bytes s = dsql(name);
  r.data.insert(r.data.end(), s.begin(), s.end());
  pad4(r.data);
  return r;
}

Row key_row(std::uint32_t id, const std::string& name) {
  Row r;
  put_le32(r.data, id);
  put_le32(r.data, id);
  Bytes s = dsql(name);
  r.data.insert(r.data.end(), s.begin(), s.end());
  pad4(r.data);
  return r;
}

Row color_row(std::uint32_t id, const std::string& name) {
  Row r;
  put_le32(r.data, 0);  // unknown1 (5 bytes: u32 + u8)
  put_u8(r.data, 1);
  put_le16(r.data, id);
  put_u8(r.data, 0);
  Bytes s = dsql(name);
  r.data.insert(r.data.end(), s.begin(), s.end());
  pad4(r.data);
  return r;
}

Row artist_row(std::uint32_t id, const std::string& name) {
  Row r;
  r.shift = true;
  const Bytes s = dsql(name);
  const bool far = 10 + s.size() > 255;
  put_le16(r.data, far ? 0x64 : 0x60);
  put_le16(r.data, 0);
  put_le32(r.data, id);
  if (far) {
    put_le16(r.data, 3);
    put_le16(r.data, 12);
  } else {
    put_u8(r.data, 3);
    put_u8(r.data, 10);
  }
  r.data.insert(r.data.end(), s.begin(), s.end());
  pad4(r.data);
  return r;
}

Row album_row(std::uint32_t id, std::uint32_t artist_id, const std::string& name) {
  Row r;
  r.shift = true;
  const Bytes s = dsql(name);
  const bool far = 0x16 + s.size() > 255;
  put_le16(r.data, far ? 0x84 : 0x80);
  put_le16(r.data, 0);
  put_le32(r.data, 0);
  put_le32(r.data, artist_id);
  put_le32(r.data, id);
  put_le32(r.data, 0);
  if (far) {
    put_le16(r.data, 3);
    put_le16(r.data, 0x18);
  } else {
    put_u8(r.data, 3);
    put_u8(r.data, 0x16);
  }
  r.data.insert(r.data.end(), s.begin(), s.end());
  pad4(r.data);
  return r;
}

struct TrackRec {
  const ExportTrack* t;
  std::uint32_t artist = 0, album = 0, genre = 0, key = 0;
  std::uint64_t file_size = 0;
  std::string file_path, file_name, analyze_path, date;
  unsigned file_type = 0;
};

Row track_row(const TrackRec& r) {
  const ExportTrack& t = *r.t;
  Row row;
  row.shift = true;
  Bytes& b = row.data;
  b.assign(0x88, 0);
  set_le16(b, 0x00, 0x24);
  set_le32(b, 0x04, 0x000C0700);  // bitmask: unknown, copied from observed exports (guess)
  set_le32(b, 0x08, static_cast<std::uint32_t>(std::max(0, t.sample_rate)));
  set_le32(b, 0x10, static_cast<std::uint32_t>(r.file_size));
  set_le16(b, 0x18, 19048);
  set_le16(b, 0x1A, 30967);
  set_le32(b, 0x20, r.key);
  set_le32(b, 0x30, static_cast<std::uint32_t>(std::max(0, t.bitrate)));
  set_le32(b, 0x34, static_cast<std::uint32_t>(std::max(0, t.track_number)));
  set_le32(b, 0x38, static_cast<std::uint32_t>(std::max(0.0, t.bpm) * 100.0 + 0.5));
  set_le32(b, 0x3C, r.genre);
  set_le32(b, 0x40, r.album);
  set_le32(b, 0x44, r.artist);
  set_le32(b, 0x48, static_cast<std::uint32_t>(t.id));
  set_le16(b, 0x50, static_cast<std::uint32_t>(std::max(0, t.year)));
  set_le16(b, 0x52, 16);
  set_le16(b, 0x54, static_cast<std::uint32_t>(std::max<std::int64_t>(0, t.duration_ms) / 1000));
  set_le16(b, 0x56, 0x29);
  b[0x58] = static_cast<std::uint8_t>(std::clamp(t.color_id, 0, 8));
  b[0x59] = static_cast<std::uint8_t>(std::clamp(t.rating, 0, 5));
  set_le16(b, 0x5A, r.file_type);
  set_le16(b, 0x5C, 3);
  std::string str[21];
  str[2] = "1";
  str[10] = r.date;
  str[14] = r.analyze_path;
  str[15] = r.date;
  str[16] = t.comment;
  str[17] = t.title;
  str[19] = r.file_name;
  str[20] = r.file_path;
  for (int i = 0; i < 21; ++i) {
    set_le16(b, 0x5E + 2 * i, static_cast<std::uint32_t>(b.size()));
    const Bytes s = dsql(str[i], i == 20 || i == 14 ? 400 : 254);
    b.insert(b.end(), s.begin(), s.end());
  }
  pad4(b);
  return row;
}

// ---- pages -------------------------------------------------------------------------------------------------

struct Table {
  std::uint32_t type = 0;
  std::vector<Row> rows;
  std::vector<std::pair<std::size_t, std::size_t>> pages;  // row ranges
  std::uint32_t index_page = 0, last_page = 0, empty_page = 0;
};

std::size_t index_bytes(std::size_t n) {
  if (n == 0) return 0;
  const std::size_t full = (n - 1) / 16;
  return full * 36 + 4 + 2 * (n - full * 16);
}

bool paginate(Table& t, std::string& error) {
  std::size_t begin = 0;
  while (begin < t.rows.size()) {
    std::size_t end = begin, heap = 0;
    while (end < t.rows.size()) {
      const std::size_t n = end - begin + 1;
      if (kHeap + heap + t.rows[end].data.size() + index_bytes(n) > kPage) break;
      heap += t.rows[end].data.size();
      ++end;
    }
    if (end == begin) {
      error = "export: a database row does not fit in one page";
      return false;
    }
    t.pages.emplace_back(begin, end);
    begin = end;
  }
  return true;
}

void write_header(Bytes& f, std::size_t page, std::uint32_t type, std::uint32_t next, std::uint32_t seq,
                  std::uint32_t nrows, std::uint32_t flags) {
  const std::size_t o = page * kPage;
  set_le32(f, o + 4, static_cast<std::uint32_t>(page));
  set_le32(f, o + 8, type);
  set_le32(f, o + 12, next);
  set_le32(f, o + 16, seq);
  const std::uint32_t packed = (nrows & 0x1FFF) | ((nrows & 0x7FF) << 13);  // num_row_offsets | num_rows << 13
  f[o + 0x18] = static_cast<std::uint8_t>(packed);
  f[o + 0x19] = static_cast<std::uint8_t>(packed >> 8);
  f[o + 0x1A] = static_cast<std::uint8_t>(packed >> 16);
  f[o + 0x1B] = static_cast<std::uint8_t>(flags);
}

void write_data_page(Bytes& f, std::size_t page, const Table& t, std::size_t begin, std::size_t end,
                     std::uint32_t next, std::uint32_t seq) {
  const std::size_t o = page * kPage;
  const std::size_t n = end - begin;
  write_header(f, page, t.type, next, seq, static_cast<std::uint32_t>(n), 0x24);
  std::size_t heap = 0;
  std::vector<std::uint32_t> pf((n + 15) / 16 + 1, 0);
  for (std::size_t k = 0; k < n; ++k) {
    Bytes row = t.rows[begin + k].data;
    if (t.rows[begin + k].shift) set_le16(row, 2, static_cast<std::uint32_t>(0x20 * k));
    std::copy(row.begin(), row.end(), f.begin() + o + kHeap + heap);
    const std::size_t g = k / 16, i = k % 16;
    const std::size_t base = kPage - 36 * g;
    set_le16(f, o + base - 6 - 2 * i, static_cast<std::uint32_t>(heap));
    pf[g] |= 1u << i;
    set_le16(f, o + base - 4, pf[g]);
    set_le16(f, o + base - 2, pf[g]);  // transaction row flags: all rows touched
    heap += row.size();
  }
  set_le16(f, o + 0x1C, static_cast<std::uint32_t>(kPage - kHeap - index_bytes(n) - heap));  // free_size
  set_le16(f, o + 0x1E, static_cast<std::uint32_t>(heap));                                    // used_size
  set_le16(f, o + 0x20, static_cast<std::uint32_t>(n));  // transaction_row_count
  set_le16(f, o + 0x22, 0);
}

void write_index_page(Bytes& f, std::size_t page, std::uint32_t type, std::uint32_t next, std::uint32_t seq) {
  const std::size_t o = page * kPage;
  write_header(f, page, type, next, seq, 0, 0x64);
  set_le16(f, o + 0x20, 0x1FFF);
  set_le16(f, o + 0x22, 0x1FFF);
  set_le16(f, o + 0x28, 0x1FFF);
  set_le16(f, o + 0x2A, 0x1FFF);
  set_le16(f, o + 0x2C, 0x03EC);
  set_le16(f, o + 0x2E, 0);
  set_le32(f, o + 0x30, static_cast<std::uint32_t>(page));
  set_le32(f, o + 0x34, next);
  set_le32(f, o + 0x38, 0x03FFFFFF);
  set_le32(f, o + 0x3C, 0);
  set_le16(f, o + 0x40, 0);       // num_entries
  set_le16(f, o + 0x42, 0x1FFF);  // first_empty
  for (std::size_t p = o + 0x44; p + 4 <= o + kPage; p += 4) set_le32(f, p, 0x1FFFFFF8);
}

// ---- helpers -----------------------------------------------------------------------------------------------

std::string sanitize_component(const std::string& s, const char* fallback) {
  std::string out;
  for (char c : cap_utf8(s, 100)) {
    const unsigned char u = static_cast<unsigned char>(c);
    out.push_back(u < 0x20 || std::string("/\\:*?\"<>|").find(c) != std::string::npos ? '_' : c);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  return out.empty() ? fallback : out;
}

std::string lower_ext(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const auto slash = path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string e = path.substr(dot + 1);
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

unsigned file_type_of(const std::string& ext) {
  if (ext == "mp3") return 1;
  if (ext == "m4a" || ext == "aac" || ext == "mp4") return 4;
  if (ext == "flac") return 5;
  if (ext == "wav") return 0x0B;
  if (ext == "aif" || ext == "aiff") return 0x0C;
  return 0;
}

std::string hex(std::uint32_t v, int width) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%0*X", width, v);
  return buf;
}

std::string analyze_dir(int id) {
  const std::uint32_t uid = static_cast<std::uint32_t>(id);
  std::uint32_t h = uid * 2246822519u;
  h ^= h >> 15;
  return "P" + hex(h & 0xFFF, 3) + "/" + hex(uid * 2654435761u, 8);  // odd multiplier: unique per id
}

std::string today() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  localtime_r(&now, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

bool write_file(const fs::path& p, const Bytes& data, std::string& error) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (f) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!f || !f.flush()) {
    error = "export: cannot write " + p.string();
    return false;
  }
  return true;
}

const char* const kColorNames[8] = {"Pink", "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple"};

// Column names: undocumented table (type 0x10); contents are a best-effort guess, see docs/EXPORT.md.
const char* const kColumnNames[] = {"GENRE", "ARTIST", "ALBUM", "TRACK", "BPM", "RATING", "YEAR", "REMIXER",
                                    "LABEL", "ORIGINAL ARTIST", "KEY", "CUE", "COLOR", "TIME", "BITRATE",
                                    "FILE NAME", "PLAYLIST", "HOT CUE BANK", "HISTORY", "SEARCH", "COMMENTS",
                                    "DATE ADDED", "DJ PLAY COUNT", "FOLDER", "DEFAULT", "ALPHABET", "MATCHING"};

bool validate(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& lists, std::string& error) {
  std::set<int> ids;
  for (const auto& t : tracks) {
    if (t.id <= 0 || !ids.insert(t.id).second) {
      error = "export: track ids must be unique and > 0";
      return false;
    }
  }
  std::map<int, const ExportPlaylist*> byid;
  for (const auto& p : lists) {
    if (p.id <= 0 || !byid.emplace(p.id, &p).second) {
      error = "export: playlist ids must be unique and > 0";
      return false;
    }
  }
  for (const auto& p : lists) {
    if (p.parent_id != 0) {
      const auto it = byid.find(p.parent_id);
      if (it == byid.end() || !it->second->is_folder) {
        error = "export: playlist parent is not a folder: " + p.name;
        return false;
      }
    }
    int depth = 0;
    for (int cur = p.parent_id; cur != 0; cur = byid[cur]->parent_id) {
      if (++depth > 64) {
        error = "export: playlist folder cycle at " + p.name;
        return false;
      }
    }
    if (!p.is_folder) {
      for (int id : p.track_ids) {
        if (!ids.count(id)) {
          error = "export: playlist '" + p.name + "' references an unknown track id";
          return false;
        }
      }
    }
  }
  return true;
}

bool export_impl(const std::vector<ExportTrack>& tracks_in, const std::vector<ExportPlaylist>& lists,
                 const ExportOptions& opt, std::string& error) {
  if (opt.destination.empty()) {
    error = "export: no destination";
    return false;
  }
  if (!validate(tracks_in, lists, error)) return false;
  std::vector<const ExportTrack*> tracks;
  for (const auto& t : tracks_in) tracks.push_back(&t);
  std::sort(tracks.begin(), tracks.end(), [](auto* a, auto* b) { return a->id < b->id; });

  const fs::path root = opt.destination;
  std::error_code ec;
  const std::string date = today();

  // Dictionary tables.
  std::map<std::string, std::uint32_t> artists, genres, keys;
  std::map<std::pair<std::uint32_t, std::string>, std::uint32_t> albums;
  auto intern = [](std::map<std::string, std::uint32_t>& m, const std::string& s) -> std::uint32_t {
    if (s.empty()) return 0;
    return m.emplace(s, static_cast<std::uint32_t>(m.size() + 1)).first->second;
  };

  std::vector<TrackRec> recs;
  std::set<std::string> used_paths;
  std::vector<fs::path> copy_from, copy_to;
  for (const ExportTrack* t : tracks) {
    TrackRec r;
    r.t = t;
    r.artist = intern(artists, t->artist);
    r.genre = intern(genres, t->genre);
    r.key = intern(keys, t->key);
    if (!t->album.empty()) {
      r.album = albums.emplace(std::make_pair(r.artist, t->album), static_cast<std::uint32_t>(albums.size() + 1))
                    .first->second;
    }
    std::error_code fe;
    const fs::path src = t->source_path;
    if (!t->source_path.empty()) {
      const auto sz = fs::file_size(src, fe);
      r.file_size = fe ? 0 : static_cast<std::uint64_t>(sz);
    }
    const std::string ext = lower_ext(t->source_path);
    r.file_type = file_type_of(ext);
    std::string fname = src.filename().string();
    if (fname.empty()) fname = "track" + std::to_string(t->id) + (ext.empty() ? "" : "." + ext);
    fname = sanitize_component(fname, "track");
    std::string rel;
    const fs::path rel_under = fs::path(t->source_path).lexically_relative(root);
    if (!opt.copy_audio && !rel_under.empty() && *rel_under.begin() != "..") {
      rel = rel_under.generic_string();  // already on the stick
      r.file_name = fs::path(rel).filename().string();
    } else {
      const std::string dir = "Contents/" + sanitize_component(t->artist, "Unknown Artist") + "/" +
                              sanitize_component(t->album, "Unknown Album") + "/";
      rel = dir + fname;
      for (int n = 2; !used_paths.insert([&] {
             std::string k = rel;
             for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
             return k;
           }()).second; ++n) {
        const fs::path p = fname;
        rel = dir + p.stem().string() + " (" + std::to_string(n) + ")" + p.extension().string();
      }
      r.file_name = fs::path(rel).filename().string();
      if (!opt.copy_audio) DH_LOG(Warn, "export: %s is not on the stick; DB points at %s", t->source_path.c_str(), rel.c_str());
    }
    r.file_path = "/" + rel;
    r.analyze_path = "/PIONEER/USBANLZ/" + analyze_dir(t->id) + "/ANLZ0000.DAT";
    r.date = date;
    if (opt.copy_audio && !t->source_path.empty()) {
      copy_from.push_back(src);
      copy_to.push_back(root / rel);
    }
    recs.push_back(std::move(r));
  }

  // Tables.
  std::vector<Table> tables(kNumTables);
  for (std::uint32_t i = 0; i < kNumTables; ++i) tables[i].type = i;
  for (const auto& r : recs) tables[kTracks].rows.push_back(track_row(r));
  auto by_id = [](const std::map<std::string, std::uint32_t>& m) {
    std::vector<std::pair<std::uint32_t, std::string>> v;
    for (const auto& [k, id] : m) v.emplace_back(id, k);
    std::sort(v.begin(), v.end());
    return v;
  };
  for (const auto& [id, n] : by_id(genres)) tables[kGenres].rows.push_back(id_name_row(id, n));
  for (const auto& [id, n] : by_id(artists)) tables[kArtists].rows.push_back(artist_row(id, n));
  {
    std::vector<std::pair<std::uint32_t, const std::pair<const std::pair<std::uint32_t, std::string>, std::uint32_t>*>> v;
    for (const auto& e : albums) v.emplace_back(e.second, &e);
    std::sort(v.begin(), v.end());
    for (const auto& [id, e] : v) tables[kAlbums].rows.push_back(album_row(id, e->first.first, e->first.second));
  }
  for (const auto& [id, n] : by_id(keys)) tables[kKeys].rows.push_back(key_row(id, n));
  for (std::uint32_t i = 0; i < 8; ++i) tables[kColors].rows.push_back(color_row(i + 1, kColorNames[i]));
  {
    std::map<int, std::uint32_t> sibling;
    for (const auto& p : lists) {
      Row r;
      put_le32(r.data, static_cast<std::uint32_t>(p.parent_id));
      put_le32(r.data, 0);
      put_le32(r.data, sibling[p.parent_id]++);
      put_le32(r.data, static_cast<std::uint32_t>(p.id));
      put_le32(r.data, p.is_folder ? 1 : 0);
      const Bytes s = dsql(p.name);
      r.data.insert(r.data.end(), s.begin(), s.end());
      pad4(r.data);
      tables[kPlaylistTree].rows.push_back(std::move(r));
      if (p.is_folder) continue;
      std::uint32_t idx = 1;
      for (int tid : p.track_ids) {
        Row e;
        put_le32(e.data, idx++);
        put_le32(e.data, static_cast<std::uint32_t>(tid));
        put_le32(e.data, static_cast<std::uint32_t>(p.id));
        tables[kPlaylistEntries].rows.push_back(std::move(e));
      }
    }
  }
  for (std::uint32_t i = 0; i < sizeof(kColumnNames) / sizeof(kColumnNames[0]); ++i) {
    Row r;
    put_le16(r.data, i + 1);
    put_le16(r.data, 0x80 + i);  // guess
    std::string name = kColumnNames[i];
    if (i < 4) name = "\xEF\xBF\xBA" + name + "\xEF\xBF\xBB";  // U+FFFA ... U+FFFB (guess)
    const Bytes s = dsql(name);
    r.data.insert(r.data.end(), s.begin(), s.end());
    pad4(r.data);
    tables[kColumns].rows.push_back(std::move(r));
  }

  // Page plan: per table an index page, data pages, then an empty "candidate" page.
  std::uint32_t next = 1;
  for (auto& t : tables) {
    if (!paginate(t, error)) return false;
    t.index_page = next++;
    next += static_cast<std::uint32_t>(t.pages.size());
    t.last_page = t.pages.empty() ? t.index_page : next - 1;
    t.empty_page = next++;
  }
  const std::uint32_t total = next;
  Bytes f(static_cast<std::size_t>(total) * kPage, 0);
  set_le32(f, 4, kPage);
  set_le32(f, 8, kNumTables);
  set_le32(f, 12, total);
  set_le32(f, 16, 1);
  set_le32(f, 20, total + 1);  // sequence counter
  for (std::uint32_t i = 0; i < kNumTables; ++i) {
    const Table& t = tables[i];
    const std::size_t p = 0x1C + 16 * i;
    set_le32(f, p, t.type);
    set_le32(f, p + 4, t.empty_page);
    set_le32(f, p + 8, t.index_page);
    set_le32(f, p + 12, t.last_page);
    write_index_page(f, t.index_page, t.type, t.pages.empty() ? t.empty_page : t.index_page + 1, t.index_page);
    for (std::size_t k = 0; k < t.pages.size(); ++k) {
      const std::uint32_t page = t.index_page + 1 + static_cast<std::uint32_t>(k);
      const std::uint32_t nx = k + 1 < t.pages.size() ? page + 1 : t.empty_page;
      write_data_page(f, page, t, t.pages[k].first, t.pages[k].second, nx, page);
    }
    write_header(f, t.empty_page, t.type, kNoNext, t.empty_page, 0, 0x24);
    set_le16(f, static_cast<std::size_t>(t.empty_page) * kPage + 0x1C, static_cast<std::uint32_t>(kPage - kHeap));
  }
  if (!write_file(root / "PIONEER/rekordbox/export.pdb", f, error)) return false;

  // Analysis files.
  for (const auto& r : recs) {
    const AnlzFiles a = build_anlz(*r.t, r.file_path, r.file_size, opt);
    const fs::path base = root / ("PIONEER/USBANLZ/" + analyze_dir(r.t->id)) / "ANLZ0000";
    if (!write_file(base.string() + ".DAT", a.dat, error) || !write_file(base.string() + ".EXT", a.ext, error)) return false;
    if (!a.ex2.empty() && !write_file(base.string() + ".2EX", a.ex2, error)) return false;
  }

  for (std::size_t i = 0; i < copy_from.size(); ++i) {
    fs::create_directories(copy_to[i].parent_path(), ec);
    fs::copy_file(copy_from[i], copy_to[i], fs::copy_options::overwrite_existing, ec);
    if (ec) {
      error = "export: cannot copy " + copy_from[i].string() + ": " + ec.message();
      return false;
    }
  }
  DH_LOG(Info, "export: wrote %zu tracks, %zu playlists, %u pages to %s", recs.size(), lists.size(), total,
         opt.destination.c_str());
  return true;
}

}  // namespace

bool export_usb(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& playlists,
                const ExportOptions& options, std::string& error) {
  error.clear();
  try {
    return export_impl(tracks, playlists, options, error);
  } catch (const std::exception& e) {
    error = std::string("export: ") + e.what();
  } catch (...) {
    error = "export: unknown failure";
  }
  return false;
}

}  // namespace deckhand::usbexport
