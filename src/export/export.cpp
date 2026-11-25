// export.pdb / exportExt.pdb writer (DeviceSQL pages) and the USB export driver. Layout per the public DJ Link
// Ecosystem Analysis "rekordbox export" page and the crate-digger Kaitai description; the audit trail and the
// remaining guesses are in docs/EXPORT.md. Pages are streamed to disk, every file is staged under a temporary
// name and moved into place in one commit phase; a failed run removes what it created.
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
#include "deckhand/export/export_check.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand::usbexport {
namespace {

using namespace detail;
namespace fs = std::filesystem;

constexpr std::size_t kPage = 4096;
constexpr std::size_t kHeap = 0x28;
constexpr std::uint32_t kNumTables = 20;
constexpr std::uint32_t kNoNext = 0x03FFFFFF;
constexpr std::size_t kMaxDbPath = 500;  // DB path strings are never truncated; longer paths are an error

enum TableType : std::uint32_t {
  kTracks = 0, kGenres = 1, kArtists = 2, kAlbums = 3, kLabels = 4, kKeys = 5, kColors = 6,
  kPlaylistTree = 7, kPlaylistEntries = 8, kArtwork = 13, kColumns = 16
};
enum ExtTableType : std::uint32_t { kExtTags = 3, kExtTagTracks = 4 };

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

// "about 200 bytes" is where the documentation says the far-name variants start.
constexpr std::size_t kFarNameBytes = 200;

Row artist_row(std::uint32_t id, const std::string& name) {
  Row r;
  r.shift = true;
  const Bytes s = dsql(name);
  const bool far = s.size() > kFarNameBytes;
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
  const bool far = s.size() > kFarNameBytes;
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

// exportExt.pdb tag / tag category row (export page "Tag Rows"). The unknown string is an empty string that sits
// directly after the fixed part; the name follows it.
Row tag_row(std::uint32_t index, std::uint32_t category, std::uint32_t category_pos, std::uint32_t id,
            bool is_category, const std::string& name) {
  Row r;
  const Bytes s = dsql(name);
  const bool far = s.size() > kFarNameBytes;
  put_le16(r.data, far ? 0x0684 : 0x0680);
  put_le16(r.data, 0x20 * index);
  put_le32(r.data, 0);
  put_le32(r.data, 0);
  put_le32(r.data, category);
  put_le32(r.data, category_pos);
  put_le32(r.data, id);
  put_le32(r.data, is_category ? 1 : 0);
  if (far) {
    put_le16(r.data, 3);
    put_le16(r.data, 0x23);  // name
    put_le16(r.data, 0x22);  // always-empty string
    put_u8(r.data, 3);       // the empty string
  } else {
    put_u8(r.data, 3);
    put_u8(r.data, 0x20);  // name
    put_u8(r.data, 0x1F);  // always-empty string
    put_u8(r.data, 3);     // the empty string
  }
  r.data.insert(r.data.end(), s.begin(), s.end());
  pad4(r.data);
  return r;
}

Row tag_track_row(std::uint32_t track_id, std::uint32_t tag_id) {
  Row r;
  put_le32(r.data, 0);
  put_le32(r.data, track_id);
  put_le32(r.data, tag_id);
  put_le32(r.data, 3);
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
  set_le32(b, 0x04, 0x000C0700);  // bitmask: meaning unknown, copied from observed exports (guess)
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
  set_le16(b, 0x54, static_cast<std::uint32_t>(std::min<std::int64_t>(0xFFFF, std::max<std::int64_t>(0, t.duration_ms) / 1000)));
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
    const Bytes s = dsql(str[i], i == 20 || i == 14 ? kMaxDbPath : (i == 19 ? 255 : 254));
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

using Page = std::vector<std::uint8_t>;

void write_header(Page& f, std::uint32_t page, std::uint32_t type, std::uint32_t next, std::uint32_t seq,
                  std::uint32_t nrows, std::uint32_t flags) {
  set_le32(f, 4, page);
  set_le32(f, 8, type);
  set_le32(f, 12, next);
  set_le32(f, 16, seq);
  // num_row_offsets: low 13 bits, num_rows: next 11 bits (little-endian bit order, Kaitai `bit-endian: le`).
  const std::uint32_t packed = (nrows & 0x1FFF) | ((nrows & 0x7FF) << 13);
  f[0x18] = static_cast<std::uint8_t>(packed);
  f[0x19] = static_cast<std::uint8_t>(packed >> 8);
  f[0x1A] = static_cast<std::uint8_t>(packed >> 16);
  f[0x1B] = static_cast<std::uint8_t>(flags);
}

void write_data_page(Page& f, std::uint32_t page, const Table& t, std::size_t begin, std::size_t end,
                     std::uint32_t next, std::uint32_t seq) {
  const std::size_t n = end - begin;
  write_header(f, page, t.type, next, seq, static_cast<std::uint32_t>(n), 0x24);
  std::size_t heap = 0;
  std::vector<std::uint32_t> pf((n + 15) / 16 + 1, 0);
  for (std::size_t k = 0; k < n; ++k) {
    const Row& src = t.rows[begin + k];
    std::copy(src.data.begin(), src.data.end(), f.begin() + kHeap + heap);
    if (src.shift) set_le16(f, kHeap + heap + 2, static_cast<std::uint32_t>(0x20 * k));
    const std::size_t g = k / 16, i = k % 16;
    const std::size_t base = kPage - 36 * g;
    set_le16(f, base - 6 - 2 * i, static_cast<std::uint32_t>(heap));
    pf[g] |= 1u << i;
    set_le16(f, base - 4, pf[g]);
    set_le16(f, base - 2, pf[g]);  // transaction row flags: all rows touched
    heap += src.data.size();
  }
  set_le16(f, 0x1C, static_cast<std::uint32_t>(kPage - kHeap - index_bytes(n) - heap));  // free_size
  set_le16(f, 0x1E, static_cast<std::uint32_t>(heap));                                    // used_size
  set_le16(f, 0x20, static_cast<std::uint32_t>(n));  // transaction_row_count
  set_le16(f, 0x22, 0);
}

// Index page: the index-specific header follows the common 0x20-byte header (page "Index Pages").
void write_index_page(Page& f, std::uint32_t page, std::uint32_t type, std::uint32_t next, std::uint32_t seq) {
  write_header(f, page, type, next, seq, 0, 0x64);
  set_le16(f, 0x20, 0x1FFF);  // unknowna
  set_le16(f, 0x22, 0x1FFF);  // unknownb
  set_le16(f, 0x24, 0x03EC);  // magic
  set_le16(f, 0x26, 0);       // next_offset
  set_le32(f, 0x28, page);
  set_le32(f, 0x2C, next);
  set_le32(f, 0x30, 0x03FFFFFF);
  set_le32(f, 0x34, 0);
  set_le16(f, 0x38, 0);       // num_entries
  set_le16(f, 0x3A, 0x1FFF);  // first_empty
  for (std::size_t p = 0x3C; p + 4 <= kPage; p += 4) set_le32(f, p, 0x1FFFFFF8);
}

// Plans the page layout of a database made of `tables`, then streams it to `path`. Page 0 carries the header.
bool stream_pdb(const fs::path& path, std::vector<Table>& tables, std::uint32_t& total_pages, std::string& error) {
  std::uint32_t next = 1;
  for (auto& t : tables) {
    if (!paginate(t, error)) return false;
    t.index_page = next++;
    next += static_cast<std::uint32_t>(t.pages.size());
    t.last_page = t.pages.empty() ? t.index_page : next - 1;
    t.empty_page = next++;
  }
  total_pages = next;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "export: cannot write " + path.string();
    return false;
  }
  auto emit = [&](const Page& p) { out.write(reinterpret_cast<const char*>(p.data()), static_cast<std::streamsize>(p.size())); };
  Page head(kPage, 0);
  set_le32(head, 4, kPage);
  set_le32(head, 8, static_cast<std::uint32_t>(tables.size()));
  set_le32(head, 12, total_pages);
  set_le32(head, 16, 1);
  set_le32(head, 20, total_pages + 1);  // sequence counter: "next" page sequence number
  for (std::size_t i = 0; i < tables.size(); ++i) {
    const Table& t = tables[i];
    const std::size_t p = 0x1C + 16 * i;
    set_le32(head, p, t.type);
    set_le32(head, p + 4, t.empty_page);
    set_le32(head, p + 8, t.index_page);
    set_le32(head, p + 12, t.last_page);
  }
  emit(head);
  Page pg(kPage);
  for (const Table& t : tables) {
    std::fill(pg.begin(), pg.end(), 0);
    write_index_page(pg, t.index_page, t.type, t.pages.empty() ? t.empty_page : t.index_page + 1, t.index_page);
    emit(pg);
    for (std::size_t k = 0; k < t.pages.size(); ++k) {
      const std::uint32_t page = t.index_page + 1 + static_cast<std::uint32_t>(k);
      const std::uint32_t nx = k + 1 < t.pages.size() ? page + 1 : t.empty_page;
      std::fill(pg.begin(), pg.end(), 0);
      write_data_page(pg, page, t, t.pages[k].first, t.pages[k].second, nx, page);
      emit(pg);
    }
    std::fill(pg.begin(), pg.end(), 0);
    write_header(pg, t.empty_page, t.type, kNoNext, t.empty_page, 0, 0x24);
    set_le16(pg, 0x1C, static_cast<std::uint32_t>(kPage - kHeap));
    emit(pg);
  }
  out.flush();
  if (!out) {
    error = "export: cannot write " + path.string() + " (disk full?)";
    return false;
  }
  return true;
}

// ---- helpers -----------------------------------------------------------------------------------------------

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string lower_ext(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const auto slash = path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  return lower(path.substr(dot + 1));
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
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

const char* const kColorNames[8] = {"Pink", "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple"};

// Column names: undocumented table (type 0x10); contents are a best-effort guess, see docs/EXPORT.md.
const char* const kColumnNames[] = {"GENRE", "ARTIST", "ALBUM", "TRACK", "BPM", "RATING", "YEAR", "REMIXER",
                                    "LABEL", "ORIGINAL ARTIST", "KEY", "CUE", "COLOR", "TIME", "BITRATE",
                                    "FILE NAME", "PLAYLIST", "HOT CUE BANK", "HISTORY", "SEARCH", "COMMENTS",
                                    "DATE ADDED", "DJ PLAY COUNT", "FOLDER", "DEFAULT", "ALPHABET", "MATCHING"};

bool validate_input(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& lists, std::string& error) {
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

// Cheap content check for "is this already the same audio file": size, first and last 64 KiB.
bool same_file_content(const fs::path& a, const fs::path& b) {
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  if (!fa || !fb) return false;
  auto sample = [](std::ifstream& f, std::streamoff pos) {
    std::vector<char> buf(65536);
    f.clear();
    f.seekg(pos);
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, f.gcount())));
    return buf;
  };
  std::error_code ec;
  const auto size = static_cast<std::streamoff>(fs::file_size(a, ec));
  if (ec) return false;
  if (sample(fa, 0) != sample(fb, 0)) return false;
  const std::streamoff tail = std::max<std::streamoff>(0, size - 65536);
  return tail == 0 || sample(fa, tail) == sample(fb, tail);
}

// ---- transaction: stage under temporary names, commit by rename, roll back on failure -----------------------

class Txn {
 public:
  bool mkdirs(const fs::path& dir, std::string& error) {
    if (known_.count(dir.string())) return true;
    std::error_code ec;
    std::vector<fs::path> missing;
    for (fs::path p = dir; !p.empty() && !fs::exists(p, ec); p = p.parent_path()) {
      missing.push_back(p);
      if (p == p.parent_path()) break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
      ec.clear();
      if (!fs::create_directory(*it, ec) || ec) {
        error = "export: cannot create folder " + it->string() + (ec ? ": " + ec.message() : "");
        return false;
      }
      created_dirs_.push_back(*it);
    }
    known_.insert(dir.string());
    return true;
  }

  // Registers `final` and returns the temporary path to write to.
  fs::path stage(const fs::path& final) {
    entries_.push_back({final, fs::path(final.string() + ".dhtmp"), false, false});
    return entries_.back().tmp;
  }

  bool write(const fs::path& final, const Bytes& data, std::string& error) {
    if (!mkdirs(final.parent_path(), error)) return false;
    const fs::path tmp = stage(final);
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f || !f.flush()) {
      error = "export: cannot write " + final.string() + " (disk full or not writable?)";
      return false;
    }
    return true;
  }

  bool copy(const fs::path& src, const fs::path& final, std::string& error) {
    if (!mkdirs(final.parent_path(), error)) return false;
    const fs::path tmp = stage(final);
    std::error_code ec;
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      error = "export: cannot copy " + src.string() + ": " + ec.message();
      return false;
    }
    return true;
  }

  bool commit(int fail_after, std::string& error) {
    int done = 0;
    for (Entry& e : entries_) {
      std::error_code ec;
      if (fail_after >= 0 && done >= fail_after) {
        error = "export: simulated failure while finishing " + e.final.string();
        return false;
      }
      if (fs::exists(e.final, ec)) {
        fs::rename(e.final, e.final.string() + ".dhold", ec);
        if (ec) {
          error = "export: cannot replace " + e.final.string() + ": " + ec.message();
          return false;
        }
        e.had_old = true;
      }
      fs::rename(e.tmp, e.final, ec);
      if (ec) {
        if (e.had_old) fs::rename(e.final.string() + ".dhold", e.final, ec);
        e.had_old = false;
        error = "export: cannot move " + e.final.string() + " into place: " + ec.message();
        return false;
      }
      e.committed = true;
      ++done;
    }
    std::error_code ec;
    for (const Entry& e : entries_)
      if (e.had_old) fs::remove(e.final.string() + ".dhold", ec);
    entries_.clear();
    created_dirs_.clear();
    return true;
  }

  void rollback() {
    std::error_code ec;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
      if (it->committed) {
        fs::remove(it->final, ec);
        if (it->had_old) fs::rename(it->final.string() + ".dhold", it->final, ec);
      } else {
        fs::remove(it->tmp, ec);
      }
    }
    for (auto it = created_dirs_.rbegin(); it != created_dirs_.rend(); ++it) fs::remove(*it, ec);  // only if empty
    entries_.clear();
    created_dirs_.clear();
  }

 private:
  struct Entry {
    fs::path final, tmp;
    bool committed = false, had_old = false;
  };
  std::vector<Entry> entries_;
  std::vector<fs::path> created_dirs_;
  std::set<std::string> known_;
};

std::uintmax_t available_space(const fs::path& root) {
  std::error_code ec;
  fs::path q = root;
  while (!q.empty() && !fs::exists(q, ec) && q != q.parent_path()) q = q.parent_path();
  if (q.empty()) q = ".";
  const auto info = fs::space(q, ec);
  return ec ? ~std::uintmax_t{0} : info.available;
}

struct CopyPlan {
  fs::path from, to;
  std::uint64_t size = 0;
};

bool export_impl(const std::vector<ExportTrack>& tracks_in, const std::vector<ExportPlaylist>& lists,
                 const ExportOptions& opt, Txn& txn, std::string& error) {
  if (opt.destination.empty()) {
    error = "export: no destination";
    return false;
  }
  if (!validate_input(tracks_in, lists, error)) return false;
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
  recs.reserve(tracks.size());
  std::set<std::string> used_paths;                 // case-insensitive destination paths taken in this run
  std::map<std::string, std::string> artist_dirs;   // lower -> first spelling (FAT is case-insensitive)
  std::map<std::string, std::string> album_dirs;
  std::map<std::string, std::string> source_rel;    // same source file exported twice shares one copy
  std::vector<CopyPlan> copies;
  std::uint64_t audio_bytes = 0;
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
    fname = sanitize_fs_name(fname, 255, "track");
    std::string rel;
    const fs::path rel_under = fs::path(t->source_path).lexically_relative(root);
    bool reuse_existing = false;
    if (!opt.copy_audio && !t->source_path.empty() && !rel_under.empty() && *rel_under.begin() != "..") {
      rel = rel_under.generic_string();  // already on the stick
      r.file_name = fs::path(rel).filename().string();
    } else {
      const auto known = t->source_path.empty() ? source_rel.end() : source_rel.find(t->source_path);
      if (known != source_rel.end()) {
        rel = known->second;
        r.file_name = fs::path(rel).filename().string();
      } else {
        auto canon = [](std::map<std::string, std::string>& m, const std::string& k, const std::string& v) {
          return m.emplace(lower(k), v).first->second;
        };
        const std::string adir = canon(artist_dirs, sanitize_fs_name(t->artist, 60, "Unknown Artist"),
                                       sanitize_fs_name(t->artist, 60, "Unknown Artist"));
        const std::string aname = sanitize_fs_name(t->album, 60, "Unknown Album");
        const std::string bdir = canon(album_dirs, adir + "/" + aname, aname);
        const std::string dir = "Contents/" + adir + "/" + bdir + "/";
        const fs::path fp = fname;
        std::string stem = fp.stem().string();
        const std::string fext = fp.extension().string();
        rel = dir + fname;
        for (int n = 2;; ++n) {
          const bool taken = used_paths.count(lower(rel)) != 0;
          bool clash_on_disk = false;
          if (!taken) {
            std::error_code xe;
            const fs::path dest = root / rel;
            if (fs::exists(dest, xe)) {
              // Never touch a pre-existing file: reuse it only when it is the same audio, otherwise pick a new name.
              if (!t->source_path.empty() && fs::is_regular_file(dest, xe) && fs::file_size(dest, xe) == r.file_size &&
                  same_file_content(src, dest))
                reuse_existing = true;
              else
                clash_on_disk = true;
            }
          }
          if (!taken && !clash_on_disk) break;
          const std::string suffix = " (" + std::to_string(n) + ")";
          rel = dir + sanitize_fs_name(stem + suffix + fext, 255, "track");
          if (n > 100000) {
            error = "export: cannot find a free file name for " + t->source_path;
            return false;
          }
        }
        used_paths.insert(lower(rel));
        r.file_name = fs::path(rel).filename().string();
        if (!t->source_path.empty()) source_rel[t->source_path] = rel;
        if (!opt.copy_audio) {
          DH_LOG(Warn, "export: %s is not on the stick; DB points at %s", t->source_path.c_str(), rel.c_str());
        } else if (!t->source_path.empty() && !reuse_existing) {
          copies.push_back({src, root / rel, r.file_size});
          audio_bytes += r.file_size;
        }
      }
    }
    if (rel.size() + 1 > kMaxDbPath) {
      error = "export: file path too long for the database: " + rel;
      return false;
    }
    r.file_path = "/" + rel;
    r.analyze_path = "/PIONEER/USBANLZ/" + analyze_dir(t->id) + "/ANLZ0000.DAT";
    r.date = date;
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

  // Optional exportExt.pdb (experimental).
  std::vector<Table> ext_tables;
  if (opt.write_ext_db) {
    ext_tables.resize(2);
    ext_tables[0].type = kExtTags;
    ext_tables[1].type = kExtTagTracks;
    std::map<std::string, std::uint32_t> tag_ids;  // name -> id, in first-use order
    std::vector<std::string> tag_names;
    for (const ExportTrack* t : tracks)
      for (const std::string& n : t->tags)
        if (!n.empty() && tag_ids.emplace(n, 1000000u + static_cast<std::uint32_t>(tag_names.size())).second)
          tag_names.push_back(n);
    if (!tag_names.empty()) {
      std::uint32_t index = 0;
      ext_tables[0].rows.push_back(tag_row(index++, 0, 0, 1, true, opt.tag_category.empty() ? "Tags" : opt.tag_category));
      std::sort(tag_names.begin(), tag_names.end());
      for (std::uint32_t k = 0; k < tag_names.size(); ++k)
        ext_tables[0].rows.push_back(tag_row(index++, 1, k, tag_ids[tag_names[k]], false, tag_names[k]));
      for (const ExportTrack* t : tracks) {
        std::set<std::uint32_t> done;
        for (const std::string& n : t->tags)
          if (!n.empty() && done.insert(tag_ids[n]).second)
            ext_tables[1].rows.push_back(tag_track_row(static_cast<std::uint32_t>(t->id), tag_ids[n]));
      }
    }
  }

  // Free space: audio plus an upper estimate of the database and analysis files.
  {
    std::uint64_t need = audio_bytes + 4u * kPage * kNumTables;
    for (const Table& t : tables) {
      std::uint64_t b = 0;
      for (const Row& r : t.rows) b += r.data.size() + 4;
      need += b + b / 8 + kPage;
    }
    for (const auto& r : recs) {
      const std::uint64_t n = std::max<std::uint64_t>(r.t->waveform.size(), static_cast<std::uint64_t>(std::max<std::int64_t>(0, r.t->duration_ms)) * 150 / 1000);
      need += 6 * n + 24000 + 40ull * r.t->cues.size() + 8ull * r.t->beats.size() + 400;
    }
    const std::uintmax_t avail =
        opt.available_bytes_override >= 0 ? static_cast<std::uintmax_t>(opt.available_bytes_override) : available_space(root);
    if (need > avail) {
      error = "export: not enough free space on the destination (need about " + std::to_string(need / (1024 * 1024) + 1) +
              " MiB, " + std::to_string(avail / (1024 * 1024)) + " MiB free)";
      return false;
    }
  }

  // Stage everything.
  if (!txn.mkdirs(root, error)) return false;
  for (const auto& c : copies)
    if (!txn.copy(c.from, c.to, error)) return false;
  for (const auto& r : recs) {
    const AnlzFiles a = build_anlz(*r.t, r.file_path, r.file_size, opt);
    const fs::path base = root / ("PIONEER/USBANLZ/" + analyze_dir(r.t->id)) / "ANLZ0000";
    if (!txn.write(base.string() + ".DAT", a.dat, error) || !txn.write(base.string() + ".EXT", a.ext, error)) return false;
    if (!a.ex2.empty() && !txn.write(base.string() + ".2EX", a.ex2, error)) return false;
  }
  std::uint32_t total = 0;
  {
    const fs::path pdb = root / "PIONEER/rekordbox/export.pdb";
    if (!txn.mkdirs(pdb.parent_path(), error)) return false;
    if (!stream_pdb(txn.stage(pdb), tables, total, error)) return false;
    if (opt.write_ext_db) {
      std::uint32_t ext_total = 0;
      if (!stream_pdb(txn.stage(root / "PIONEER/rekordbox/exportExt.pdb"), ext_tables, ext_total, error)) return false;
    }
  }
  if (!txn.commit(opt.fail_after_commits, error)) return false;
  DH_LOG(Info, "export: wrote %zu tracks, %zu playlists, %u pages to %s", recs.size(), lists.size(), total,
         opt.destination.c_str());
  return true;
}

}  // namespace

bool export_usb(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& playlists,
                const ExportOptions& options, std::string& error) {
  error.clear();
  Txn txn;
  try {
    if (export_impl(tracks, playlists, options, txn, error)) return true;
  } catch (const std::exception& e) {
    error = std::string("export: ") + e.what();
  } catch (...) {
    error = "export: unknown failure";
  }
  try {
    txn.rollback();
  } catch (...) {
  }
  return false;
}

}  // namespace deckhand::usbexport
