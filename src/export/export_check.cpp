// Structural validator (validate_export) and file-system name helpers. The validator reads the stick only and
// shares no bookkeeping with the writer; see docs/EXPORT.md for the invariants it enforces.
#include "deckhand/export/export_check.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>

#include "bytes.hpp"
#include "rawio.hpp"

namespace deckhand::usbexport {

using namespace detail;
namespace fs = std::filesystem;

// ---- report ------------------------------------------------------------------------------------------------

std::size_t ValidationReport::errors() const {
  return static_cast<std::size_t>(std::count_if(issues.begin(), issues.end(), [](const Issue& i) { return i.severity == Severity::Error; }));
}
std::size_t ValidationReport::warnings() const { return issues.size() - errors(); }
bool ValidationReport::has(const std::string& code) const {
  return std::any_of(issues.begin(), issues.end(), [&](const Issue& i) { return i.code == code; });
}
void ValidationReport::add(Severity s, std::string code, std::string message) {
  issues.push_back({s, std::move(code), std::move(message)});
}
std::string ValidationReport::text() const {
  std::string s;
  for (const Issue& i : issues) s += std::string(i.severity == Severity::Error ? "ERROR   " : "warning ") + i.code + ": " + i.message + "\n";
  s += std::to_string(errors()) + " errors, " + std::to_string(warnings()) + " warnings; " + std::to_string(tables_checked) +
       " tables, " + std::to_string(tracks_checked) + " tracks, " + std::to_string(files_checked) + " files checked\n";
  return s;
}

// ---- file-system names -------------------------------------------------------------------------------------

namespace {

bool reserved_dos_name(const std::string& stem) {
  std::string u;
  for (char c : stem) u.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  while (!u.empty() && (u.back() == ' ' || u.back() == '.')) u.pop_back();
  if (u == "CON" || u == "PRN" || u == "AUX" || u == "NUL" || u == "CLOCK$") return true;
  return u.size() == 4 && (u.compare(0, 3, "COM") == 0 || u.compare(0, 3, "LPT") == 0) && u[3] >= '0' && u[3] <= '9';
}

// Length of the UTF-8 sequence at s[i] if it is well formed, else 0.
std::size_t utf8_len_at(const std::string& s, std::size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  std::size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
  if (n == 0 || i + n > s.size()) return 0;
  for (std::size_t k = 1; k < n; ++k)
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
  if (n == 3) {
    const unsigned cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6);
    if (cp < 0x800 || (cp >= 0xD800 && cp < 0xE000)) return 0;
  }
  if (n == 4 && (c > 0xF4 || (c == 0xF0 && static_cast<unsigned char>(s[i + 1]) < 0x90) || (c == 0xF4 && static_cast<unsigned char>(s[i + 1]) >= 0x90))) return 0;
  return n;
}

std::size_t utf16_units(const std::string& s) { return utf8_to_utf16(s).size(); }

}  // namespace

std::string sanitize_fs_name(const std::string& name, std::size_t max_bytes, const char* fallback) {
  // Split a short extension off so a long name keeps it.
  std::string stem = name, ext;
  const auto dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0 && name.size() - dot <= 12 && name.find_first_of(" \\/", dot) == std::string::npos) {
    stem = name.substr(0, dot);
    ext = name.substr(dot);
  }
  auto clean = [](const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size();) {
      const std::size_t n = utf8_len_at(in, i);
      if (n == 0 || n == 4) {  // invalid byte or a non-BMP character (emoji): not portable across players
        out.push_back('_');
        i += n == 0 ? 1 : n;
        continue;
      }
      const unsigned char c = static_cast<unsigned char>(in[i]);
      if (n == 1 && (c < 0x20 || c == 0x7F || std::string("/\\:*?\"<>|").find(in[i]) != std::string::npos))
        out.push_back('_');
      else
        out.append(in, i, n);
      i += n;
    }
    return out;
  };
  stem = clean(stem);
  ext = clean(ext);
  while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
  stem.erase(0, stem.find_first_not_of(' ') == std::string::npos ? stem.size() : stem.find_first_not_of(' '));
  while (!ext.empty() && (ext.back() == ' ' || ext.back() == '.')) ext.pop_back();
  if (ext == ".") ext.clear();
  if (stem.empty()) stem = fallback;
  if (reserved_dos_name(stem)) stem = "_" + stem;
  const std::size_t budget = max_bytes > ext.size() + 1 ? max_bytes - ext.size() : 1;
  stem = cap_utf8(stem, budget);
  while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
  if (stem.empty()) stem = "_";
  return stem + ext;
}

std::vector<std::string> fat_path_problems(const std::string& path) {
  std::vector<std::string> out;
  if (path.size() > 500) out.push_back("path longer than 500 bytes");
  std::size_t pos = !path.empty() && path[0] == '/' ? 1 : 0;
  while (pos < path.size()) {
    std::size_t end = path.find('/', pos);
    if (end == std::string::npos) end = path.size();
    const std::string c = path.substr(pos, end - pos);
    pos = end + 1;
    if (c.empty()) {
      out.push_back("empty path component");
      continue;
    }
    if (c == "." || c == "..") out.push_back("component '" + c + "'");
    if (utf16_units(c) > 255) out.push_back("component longer than 255 characters: " + c.substr(0, 20) + "...");
    for (char ch : c) {
      const unsigned char u = static_cast<unsigned char>(ch);
      if (u < 0x20 || std::string("\\:*?\"<>|").find(ch) != std::string::npos) {
        out.push_back("forbidden character in '" + c + "'");
        break;
      }
    }
    if (c.back() == '.' || c.back() == ' ') out.push_back("trailing dot or space in '" + c + "'");
    const auto dot = c.find('.');
    if (reserved_dos_name(dot == std::string::npos ? c : c.substr(0, dot))) out.push_back("reserved device name '" + c + "'");
  }
  return out;
}

// ---- validator ---------------------------------------------------------------------------------------------

namespace {

// Bytes a present row occupies, from its type's fixed part and string pointers. 0 = unknown row type.
std::size_t row_extent(const PdbFile& f, std::uint32_t type, bool ext, const PdbRow& row, bool& ok) {
  ok = true;
  const View r{f.data, row.abs};
  auto str_end = [&](std::size_t off, bool isrc = false) {
    const DsqlStr s = dsql_read(r, off, isrc);
    if (!s.ok()) {
      ok = false;
      return std::size_t{0};
    }
    return off + s.extent;
  };
  std::size_t end = 0;
  if (!ext) {
    switch (type) {
      case 0: {
        end = 0x88;
        if (r.le16(0) != 0x24) ok = false;
        for (int i = 0; i < 21; ++i) end = std::max(end, str_end(r.le16(0x5E + 2 * i), i == 0));
        return end;
      }
      case 1: case 4: case 13: case 16: case 11: case 17: return std::max<std::size_t>(4, str_end(4));
      case 5: case 6: return str_end(8);
      case 2: {
        const bool far = (r.le16(0) & 4) != 0;
        return std::max<std::size_t>(far ? 12 : 10, str_end(far ? r.le16(0x0A) : r.u8(9)));
      }
      case 3: {
        const bool far = (r.le16(0) & 4) != 0;
        return std::max<std::size_t>(far ? 0x18 : 0x16, str_end(far ? r.le16(0x16) : r.u8(0x15)));
      }
      case 7: return str_end(0x14);
      case 8: case 12: case 18: return 12;
      default: return 0;
    }
  }
  switch (type) {
    case 3: {
      const bool far = (r.le16(0) & 4) != 0;
      return std::max<std::size_t>(far ? 0x22 : 0x1F, str_end(far ? r.le16(0x1E) : r.u8(0x1D)));
    }
    case 4: return 16;
    default: return 0;
  }
}

struct Ctx {
  const fs::path root;
  ValidationReport& rep;
  const ValidateOptions& opt;
};

std::string tname(std::uint32_t type, bool ext) { return std::string(ext ? "ext table " : "table ") + std::to_string(type); }

// Page-level checks shared by export.pdb and exportExt.pdb.
void check_pages(Ctx& c, const PdbFile& f, bool ext) {
  for (const auto& p : f.problems) c.rep.add(Severity::Error, p.code, std::string(ext ? "exportExt.pdb: " : "export.pdb: ") + p.message);
  if (!f.usable) return;
  if (f.next_unused < f.num_pages) c.rep.add(Severity::Warning, "pdb.next_unused", "next_unused_page points inside the file");
  std::set<std::uint32_t> types, chained;
  for (const PdbTable& t : f.tables) {
    ++c.rep.tables_checked;
    const std::string tn = tname(t.type, ext);
    if (!types.insert(t.type).second) c.rep.add(Severity::Error, "pdb.table_dup", tn + " listed twice");
    if (t.empty_candidate >= f.num_pages) c.rep.add(Severity::Error, "pdb.empty_candidate", tn + ": empty_candidate outside the file");
    for (std::uint32_t pg : t.chain) chained.insert(pg);
    chained.insert(t.empty_candidate);
    if (!t.chain.empty() && t.first < f.num_pages) {
      if ((f.pages[t.first].flags & 0x40) == 0) c.rep.add(Severity::Error, "pdb.index_page_flag", tn + ": first page is not an index page");
      for (std::size_t i = 1; i < t.chain.size(); ++i)
        if (f.pages[t.chain[i]].flags & 0x40) c.rep.add(Severity::Error, "pdb.data_page_flag", tn + ": data page carries the index flag");
    }
    // Per page.
    for (std::uint32_t pg : t.chain) {
      const PdbPage& g = f.pages[pg];
      if (g.seq >= f.seq) c.rep.add(Severity::Error, "pdb.page_seq", tn + ": page " + std::to_string(pg) + " sequence is not below the database sequence");
      if (!g.is_data()) continue;
      const std::size_t pbase = static_cast<std::size_t>(pg) * f.page_size;
      const View pv{f.data, pbase};
      const std::size_t idx = PdbFile::index_bytes(g.num_offsets);
      if (kHeapStart + idx > f.page_size) {
        c.rep.add(Severity::Error, "pdb.index_overflow", tn + ": page " + std::to_string(pg) + " row index does not fit");
        continue;
      }
      std::size_t present = 0;
      const std::size_t groups = g.num_offsets ? (g.num_offsets - 1) / 16 + 1 : 0;
      for (std::size_t grp = 0; grp < groups; ++grp) {
        const std::uint32_t mask = pv.le16(f.page_size - 36 * grp - 4);
        for (std::size_t i = 0; i < 16; ++i) {
          const bool bit = (mask >> i) & 1;
          if (bit && grp * 16 + i >= g.num_offsets)
            c.rep.add(Severity::Error, "pdb.presence_mask", tn + ": page " + std::to_string(pg) + " marks a row beyond num_row_offsets");
          present += bit;
        }
      }
      if (present != g.num_rows)
        c.rep.add(Severity::Error, "pdb.row_count", tn + ": page " + std::to_string(pg) + " num_rows " + std::to_string(g.num_rows) +
                                                     " but " + std::to_string(present) + " rows are present");
      if (g.num_rows > g.num_offsets) c.rep.add(Severity::Error, "pdb.row_count", tn + ": more rows than offsets on page " + std::to_string(pg));
      const std::size_t heap_cap = f.page_size - kHeapStart - idx;
      // Rows: bounds and overlap.
      std::vector<std::pair<std::size_t, std::size_t>> spans;  // start, end
      std::size_t max_end = 0;
      for (const PdbRow& row : t.rows) {
        if (row.page != pg || !row.present) continue;
        if (row.ofs % 4) c.rep.add(Severity::Warning, "pdb.row_align", tn + ": row offset not 4-byte aligned on page " + std::to_string(pg));
        if (row.ofs >= heap_cap) {
          c.rep.add(Severity::Error, "pdb.row_bounds", tn + ": row offset " + std::to_string(row.ofs) + " outside the heap of page " + std::to_string(pg));
          continue;
        }
        bool ok = true;
        const std::size_t ext_len = row_extent(f, t.type, ext, row, ok);
        if (!ok) {
          c.rep.add(Severity::Error, "pdb.row_bad", tn + ": malformed row (subtype or string) on page " + std::to_string(pg));
          continue;
        }
        if (ext_len == 0) {
          spans.emplace_back(row.ofs, row.ofs);
          continue;
        }
        if (row.ofs + ext_len > heap_cap) {
          c.rep.add(Severity::Error, "pdb.row_bounds", tn + ": row runs past the heap on page " + std::to_string(pg));
          continue;
        }
        spans.emplace_back(row.ofs, row.ofs + ext_len);
        max_end = std::max(max_end, row.ofs + ext_len);
      }
      std::sort(spans.begin(), spans.end());
      for (std::size_t i = 1; i < spans.size(); ++i)
        if (spans[i].first < spans[i - 1].second || (spans[i].first == spans[i - 1].first && spans[i].second > spans[i].first))
          c.rep.add(Severity::Error, "pdb.row_overlap", tn + ": rows overlap on page " + std::to_string(pg));
      if (g.used_size < max_end || g.used_size > heap_cap)
        c.rep.add(Severity::Error, "pdb.used_size", tn + ": page " + std::to_string(pg) + " used_size " + std::to_string(g.used_size) +
                                                        " does not cover its rows (" + std::to_string(max_end) + ")");
      if (g.free_size + g.used_size != heap_cap)
        c.rep.add(Severity::Error, "pdb.free_size", tn + ": page " + std::to_string(pg) + " free_size + used_size != heap capacity");
    }
  }
  for (std::uint32_t pg = 1; pg < f.num_pages; ++pg)
    if (!chained.count(pg)) c.rep.add(Severity::Warning, "pdb.orphan_page", "page " + std::to_string(pg) + " belongs to no table");
}

struct TrackInfo {
  std::uint32_t id = 0, artist = 0, album = 0, genre = 0, key = 0, label = 0, color = 0, artwork = 0;
  std::uint32_t composer = 0, orig = 0, remixer = 0, file_size = 0, duration_s = 0;
  std::string file_path, analyze_path, file_name, title;
};

void check_anlz_file(Ctx& c, const fs::path& path, const char* kind, const TrackInfo& t, bool required) {
  Bytes data;
  if (!read_all(path, data)) {
    if (required) c.rep.add(Severity::Error, std::string("anlz.missing_") + kind, "track " + std::to_string(t.id) + ": cannot read " + path.string());
    return;
  }
  ++c.rep.files_checked;
  AnlzInfo a;
  const bool ok = a.load(std::move(data));
  const std::string who = "track " + std::to_string(t.id) + " " + kind + ": ";
  if (!ok) {
    c.rep.add(Severity::Error, "anlz.structure", who + (a.problem.empty() ? "unparsable" : a.problem));
    if (!a.header_ok) return;
  }
  const bool is_dat = std::string(kind) == "DAT", is_ext = std::string(kind) == "EXT";
  auto need = [&](const char* cc, bool err = true) {
    if (!a.find(cc)) c.rep.add(err ? Severity::Error : Severity::Warning, "anlz.tag_missing", who + "no " + cc + " tag");
  };
  need("PPTH");
  if (is_dat) {
    need("PQTZ"); need("PWAV"); need("PWV2"); need("PVBR", false); need("PCOB", false);
  } else if (is_ext) {
    need("PWV3"); need("PWV5"); need("PCO2", false);
  }
  if (const AnlzTag* p = a.find("PPTH")) {
    const View tv{a.data, p->pos};
    const std::string path_in = utf16be_z(tv, p->len_header, tv.be32(0x0C));
    if (path_in != t.file_path) c.rep.add(Severity::Error, "anlz.ppth", who + "PPTH '" + path_in + "' differs from the track path '" + t.file_path + "'");
  }
  const std::uint32_t dur_ms = t.duration_s ? t.duration_s * 1000 + 999 : 0;
  if (const AnlzTag* p = a.find("PQTZ")) {
    const View tv{a.data, p->pos};
    const std::size_t n = tv.be32(0x14);
    if (p->len_header != 0x18 || static_cast<std::uint64_t>(p->len_header) + 8ull * n != p->len_tag) {
      c.rep.add(Severity::Error, "anlz.beatgrid_length", who + "PQTZ length does not match its beat count");
    } else {
      std::int64_t last_time = -1;
      unsigned last_bar = 0;
      for (std::size_t i = 0; i < n; ++i) {
        const std::size_t e = p->len_header + 8 * i;
        const unsigned bar = tv.be16(e), tempo = tv.be16(e + 2);
        const std::int64_t time = tv.be32(e + 4);
        if (time <= last_time) {
          c.rep.add(Severity::Error, "anlz.beatgrid_order", who + "beat times are not strictly increasing at beat " + std::to_string(i));
          break;
        }
        if (bar < 1 || bar > 4 || (last_bar && bar != last_bar % 4 + 1)) {
          c.rep.add(Severity::Error, "anlz.beatgrid_bar", who + "beat numbers do not cycle 1-4 at beat " + std::to_string(i));
          break;
        }
        if (tempo == 0) {
          c.rep.add(Severity::Warning, "anlz.beatgrid_tempo", who + "zero tempo at beat " + std::to_string(i));
          break;
        }
        if (dur_ms && time > dur_ms) {
          c.rep.add(Severity::Warning, "anlz.beatgrid_range", who + "beat after the end of the track");
          break;
        }
        last_time = time;
        last_bar = bar;
      }
    }
  }
  auto cues = [&](const char* cc, std::size_t count_off, std::size_t time_off, std::size_t loop_off, std::size_t type_off,
                  std::size_t min_len, const char* magic, std::size_t count_w) {
    for (std::size_t nth = 0; const AnlzTag* p = a.find(cc, nth); ++nth) {
      const View tv{a.data, p->pos};
      const std::size_t n = count_w == 2 ? tv.be16(count_off) : 0;
      std::size_t e = p->len_header;
      std::set<std::uint32_t> hot_seen;
      for (std::size_t i = 0; i < n; ++i) {
        if (e + min_len > p->len_tag || std::string(a.data.begin() + p->pos + e, a.data.begin() + p->pos + e + 4) != magic) {
          c.rep.add(Severity::Error, "anlz.cue_entry", who + cc + " entry " + std::to_string(i) + " is malformed");
          return;
        }
        const std::size_t len = tv.be32(e + 8);
        if (len < min_len || e + len > p->len_tag) {
          c.rep.add(Severity::Error, "anlz.cue_entry", who + cc + " entry " + std::to_string(i) + " has an impossible length");
          return;
        }
        const std::uint32_t hot = tv.be32(e + 0x0C), time = tv.be32(e + time_off), loop = tv.be32(e + loop_off);
        if (hot > 8) c.rep.add(Severity::Error, "anlz.hot_slot", who + "hot cue number " + std::to_string(hot) + " is outside 1..8");
        if (hot && !hot_seen.insert(hot).second) c.rep.add(Severity::Error, "anlz.hot_slot", who + "hot cue " + std::to_string(hot) + " appears twice");
        if (dur_ms && time > dur_ms) c.rep.add(Severity::Error, "anlz.cue_range", who + "cue at " + std::to_string(time) + " ms is beyond the track end");
        if (tv.u8(e + type_off) == 2) {
          if (loop <= time || (dur_ms && loop > dur_ms)) c.rep.add(Severity::Error, "anlz.loop_range", who + "loop end is not inside (start, track end]");
        }
        e += len;
      }
      if (n && e != p->len_tag) c.rep.add(Severity::Error, "anlz.cue_entry", who + cc + " entries do not fill the tag");
    }
  };
  if (is_dat) cues("PCOB", 0x12, 0x20, 0x24, 0x1C, 0x28, "PCPT", 2);
  if (is_ext) cues("PCO2", 0x10, 0x14, 0x18, 0x10, 0x2C, "PCP2", 2);
  // Waveform sizes.
  auto tag_u32 = [&](const char* cc, std::size_t off) -> std::int64_t {
    const AnlzTag* p = a.find(cc);
    return p ? static_cast<std::int64_t>(View{a.data, p->pos}.be32(off)) : -1;
  };
  auto want = [&](const char* cc, std::size_t off, std::int64_t value) {
    const std::int64_t got = tag_u32(cc, off);
    if (got >= 0 && got != value) c.rep.add(Severity::Error, "anlz.waveform_size", who + cc + " has " + std::to_string(got) + " entries, expected " + std::to_string(value));
  };
  if (is_dat) { want("PWAV", 0x0C, 400); want("PWV2", 0x0C, 100); }
  if (is_ext) {
    want("PWV4", 0x10, 1200);
    const std::int64_t n3 = tag_u32("PWV3", 0x10);
    if (n3 >= 0) {
      if (t.duration_s && std::llabs(n3 - static_cast<std::int64_t>(t.duration_s) * 150) > 150)
        c.rep.add(Severity::Warning, "anlz.waveform_length", who + "PWV3 length does not match the track duration");
      want("PWV5", 0x10, n3);
    }
  }
  if (!is_dat && !is_ext) {
    want("PWV6", 0x10, 1200);
    const std::int64_t n7 = tag_u32("PWV7", 0x10);
    if (n7 >= 0 && t.duration_s && std::llabs(n7 - static_cast<std::int64_t>(t.duration_s) * 150) > 150)
      c.rep.add(Severity::Warning, "anlz.waveform_length", who + "PWV7 length does not match the track duration");
  }
}

std::string parent_of(const std::string& p) {
  const auto s = p.find_last_of('/');
  return s == std::string::npos ? std::string() : p.substr(0, s);
}

}  // namespace

bool validate_export(const std::string& root_str, ValidationReport& rep, const ValidateOptions& opt) {
  try {
    const fs::path root = root_str;
    Ctx c{root, rep, opt};
    Bytes db;
    if (!read_all(root / "PIONEER/rekordbox/export.pdb", db)) {
      rep.add(Severity::Error, "pdb.missing", "cannot read PIONEER/rekordbox/export.pdb");
      return false;
    }
    PdbFile f;
    f.load(std::move(db));
    check_pages(c, f, false);
    if (!f.usable) return rep.ok();

    // Collect rows by table.
    std::map<std::uint32_t, std::set<std::uint32_t>> ids;  // table type -> ids present
    std::map<std::uint32_t, std::uint32_t> album_artist;
    std::vector<TrackInfo> tracks;
    struct Node { std::uint32_t parent = 0; bool folder = false; };
    std::map<std::uint32_t, Node> tree;
    struct Entry { std::uint32_t idx, track, list; };
    std::vector<Entry> entries;
    for (const PdbTable& t : f.tables) {
      if (!t.chain_ok) continue;
      for (const PdbRow& row : t.rows) {
        if (!row.present) continue;
        const View r{f.data, row.abs};
        switch (t.type) {
          case 0: {
            TrackInfo ti;
            ti.id = r.le32(0x48); ti.artist = r.le32(0x44); ti.album = r.le32(0x40); ti.genre = r.le32(0x3C);
            ti.key = r.le32(0x20); ti.label = r.le32(0x28); ti.color = r.u8(0x58); ti.artwork = r.le32(0x1C);
            ti.composer = r.le32(0x0C); ti.orig = r.le32(0x24); ti.remixer = r.le32(0x2C);
            ti.file_size = r.le32(0x10); ti.duration_s = r.le16(0x54);
            ti.analyze_path = dsql_read(r, r.le16(0x5E + 28)).text;
            ti.title = dsql_read(r, r.le16(0x5E + 34)).text;
            ti.file_name = dsql_read(r, r.le16(0x5E + 38)).text;
            ti.file_path = dsql_read(r, r.le16(0x5E + 40)).text;
            tracks.push_back(std::move(ti));
            break;
          }
          case 1: case 4: case 13: case 17: case 11:
            if (!ids[t.type].insert(r.le32(0)).second) rep.add(Severity::Error, "dup.id", tname(t.type, false) + ": duplicate id " + std::to_string(r.le32(0)));
            break;
          case 5:
            if (!ids[t.type].insert(r.le32(0)).second) rep.add(Severity::Error, "dup.id", tname(t.type, false) + ": duplicate id " + std::to_string(r.le32(0)));
            break;
          case 2:
            if (!ids[t.type].insert(r.le32(4)).second) rep.add(Severity::Error, "dup.id", tname(t.type, false) + ": duplicate id " + std::to_string(r.le32(4)));
            break;
          case 3:
            if (!ids[t.type].insert(r.le32(0x0C)).second) rep.add(Severity::Error, "dup.id", tname(t.type, false) + ": duplicate id " + std::to_string(r.le32(0x0C)));
            album_artist[r.le32(0x0C)] = r.le32(8);
            break;
          case 6:
            ids[t.type].insert(r.le16(5));
            break;
          case 7: {
            const std::uint32_t id = r.le32(0x0C);
            if (!tree.emplace(id, Node{r.le32(0), r.le32(0x10) != 0}).second) rep.add(Severity::Error, "dup.id", "playlist tree: duplicate id " + std::to_string(id));
            break;
          }
          case 8:
            entries.push_back({r.le32(0), r.le32(4), r.le32(8)});
            break;
          default:
            break;
        }
      }
    }
    // Foreign keys.
    std::set<std::uint32_t> track_ids;
    for (const TrackInfo& t : tracks) {
      if (!track_ids.insert(t.id).second) rep.add(Severity::Error, "dup.id", "duplicate track id " + std::to_string(t.id));
      auto fk = [&](std::uint32_t id, std::uint32_t table, const char* what) {
        if (id != 0 && !ids[table].count(id)) rep.add(Severity::Error, std::string("fk.track_") + what, "track " + std::to_string(t.id) + " references missing " + what + " id " + std::to_string(id));
      };
      fk(t.artist, 2, "artist"); fk(t.composer, 2, "composer"); fk(t.orig, 2, "original_artist"); fk(t.remixer, 2, "remixer");
      fk(t.album, 3, "album"); fk(t.genre, 1, "genre"); fk(t.key, 5, "key"); fk(t.label, 4, "label"); fk(t.artwork, 13, "artwork");
      if (t.color != 0 && !ids[6].count(t.color)) rep.add(Severity::Error, "fk.track_color", "track " + std::to_string(t.id) + " references missing colour " + std::to_string(t.color));
    }
    for (const auto& [id, artist] : album_artist)
      if (artist != 0 && !ids[2].count(artist)) rep.add(Severity::Error, "fk.album_artist", "album " + std::to_string(id) + " references missing artist " + std::to_string(artist));
    for (const auto& [id, n] : tree) {
      if (n.parent == 0) continue;
      const auto it = tree.find(n.parent);
      if (it == tree.end()) rep.add(Severity::Error, "fk.playlist_parent", "playlist " + std::to_string(id) + " has missing parent " + std::to_string(n.parent));
      else if (!it->second.folder) rep.add(Severity::Error, "fk.playlist_parent", "playlist " + std::to_string(id) + " has a parent that is not a folder");
      int depth = 0;
      for (std::uint32_t cur = n.parent; cur != 0 && tree.count(cur); cur = tree[cur].parent)
        if (++depth > 64) {
          rep.add(Severity::Error, "fk.playlist_cycle", "playlist folder cycle at " + std::to_string(id));
          break;
        }
    }
    std::map<std::uint32_t, std::vector<std::uint32_t>> per_list;
    for (const Entry& e : entries) {
      if (!track_ids.count(e.track)) rep.add(Severity::Error, "fk.entry_track", "playlist " + std::to_string(e.list) + " entry references missing track " + std::to_string(e.track));
      const auto it = tree.find(e.list);
      if (it == tree.end() || it->second.folder) rep.add(Severity::Error, "fk.entry_playlist", "playlist entry belongs to missing playlist/folder " + std::to_string(e.list));
      per_list[e.list].push_back(e.idx);
    }
    for (auto& [list, idx] : per_list) {
      std::sort(idx.begin(), idx.end());
      for (std::size_t i = 1; i < idx.size(); ++i)
        if (idx[i] == idx[i - 1]) {
          rep.add(Severity::Error, "dup.entry_index", "playlist " + std::to_string(list) + " has a repeated entry index");
          break;
        }
    }

    // Paths, audio, analysis files.
    std::map<std::string, std::string> path_spelling, dir_spelling;  // lower -> first spelling
    std::map<std::string, std::uint32_t> analyze_owner;
    std::set<std::string> referenced_anlz;
    for (const TrackInfo& t : tracks) {
      ++rep.tracks_checked;
      if (t.file_path.empty() || t.file_path[0] != '/') rep.add(Severity::Error, "path.absolute", "track " + std::to_string(t.id) + " file path is not rooted at the stick");
      for (const std::string& p : fat_path_problems(t.file_path)) rep.add(Severity::Error, "fat.path", "track " + std::to_string(t.id) + ": " + p);
      for (const std::string& p : fat_path_problems(t.analyze_path)) rep.add(Severity::Error, "fat.path", "track " + std::to_string(t.id) + " analysis path: " + p);
      auto lower = [](std::string s) { for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; };
      const auto sp = path_spelling.emplace(lower(t.file_path), t.file_path);
      if (sp.first->second != t.file_path) rep.add(Severity::Error, "fat.case_collision", "paths differ only by case: " + sp.first->second + " / " + t.file_path);
      for (std::string d = parent_of(t.file_path); !d.empty(); d = parent_of(d)) {
        const auto ds = dir_spelling.emplace(lower(d), d);
        if (ds.first->second != d) {
          rep.add(Severity::Error, "fat.case_collision", "folders differ only by case: " + ds.first->second + " / " + d);
          break;
        }
      }
      const auto ao = analyze_owner.emplace(t.analyze_path, t.id);
      if (!ao.second) rep.add(Severity::Error, "anlz.dup_path", "tracks " + std::to_string(ao.first->second) + " and " + std::to_string(t.id) + " share an analysis path");
      referenced_anlz.insert(lower(t.analyze_path));
      if (!t.file_path.empty() && t.file_name != fs::path(t.file_path).filename().string())
        rep.add(Severity::Error, "path.file_name", "track " + std::to_string(t.id) + ": file name string differs from the path");
      if (opt.check_audio) {
        std::error_code ec;
        const fs::path ap = root / (t.file_path.empty() ? std::string() : t.file_path.substr(1));
        const auto sz = fs::is_regular_file(ap, ec) ? fs::file_size(ap, ec) : static_cast<std::uintmax_t>(-1);
        ++rep.files_checked;
        if (sz == static_cast<std::uintmax_t>(-1) || ec) rep.add(Severity::Error, "audio.missing", "track " + std::to_string(t.id) + ": audio file missing: " + t.file_path);
        else if (static_cast<std::uint32_t>(sz) != t.file_size) rep.add(Severity::Error, "audio.size", "track " + std::to_string(t.id) + ": audio size " + std::to_string(sz) + " != recorded " + std::to_string(t.file_size));
      }
      if (opt.check_anlz) {
        const std::string rel = t.analyze_path.empty() || t.analyze_path[0] != '/' ? t.analyze_path : t.analyze_path.substr(1);
        fs::path dat = root / rel, ex = dat, ex2 = dat;
        ex.replace_extension(".EXT");
        ex2.replace_extension(".2EX");
        check_anlz_file(c, dat, "DAT", t, true);
        check_anlz_file(c, ex, "EXT", t, true);
        check_anlz_file(c, ex2, "2EX", t, false);
        referenced_anlz.insert(lower(parent_of(t.analyze_path) + "/" + ex.filename().string()));
        referenced_anlz.insert(lower(parent_of(t.analyze_path) + "/" + ex2.filename().string()));
      }
    }

    // Leftovers and orphans.
    std::error_code ec;
    if (fs::exists(root / "PIONEER", ec)) {
      for (fs::recursive_directory_iterator it(root / "PIONEER", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.size() > 6 && (name.compare(name.size() - 6, 6, ".dhtmp") == 0 || name.compare(name.size() - 6, 6, ".dhold") == 0))
          rep.add(Severity::Error, "export.leftover", "temporary file left behind: " + it->path().string());
        if (opt.check_anlz && it->path().extension() != ".pdb" && name.rfind("ANLZ", 0) == 0) {
          std::string rel = "/" + it->path().lexically_relative(root).generic_string();
          for (char& ch : rel) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          if (!referenced_anlz.count(rel)) rep.add(Severity::Warning, "anlz.orphan", "analysis file no track points to: " + rel);
        }
      }
    }

    // exportExt.pdb, when present.
    Bytes edb;
    if (read_all(root / "PIONEER/rekordbox/exportExt.pdb", edb)) {
      PdbFile e;
      e.load(std::move(edb));
      check_pages(c, e, true);
      if (e.usable) {
        std::set<std::uint32_t> tag_ids, cat_ids;
        std::vector<std::pair<std::uint32_t, bool>> tag_cat;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> links;
        std::vector<std::uint32_t> tag_category_of;
        for (const PdbTable& t : e.tables) {
          if (!t.chain_ok) continue;
          for (const PdbRow& row : t.rows) {
            if (!row.present) continue;
            const View r{e.data, row.abs};
            if (t.type == 3) {
              const std::uint32_t id = r.le32(0x14);
              const bool is_cat = r.le32(0x18) != 0;
              if (!tag_ids.insert(id).second) rep.add(Severity::Error, "dup.id", "exportExt tag id " + std::to_string(id) + " duplicated");
              if (is_cat) cat_ids.insert(id);
              else tag_category_of.push_back(r.le32(0x0C));
            } else if (t.type == 4) {
              links.emplace_back(r.le32(4), r.le32(8));
            }
          }
        }
        for (std::uint32_t cat : tag_category_of)
          if (!cat_ids.count(cat)) rep.add(Severity::Error, "fk.tag_category", "tag references missing category " + std::to_string(cat));
        for (const auto& [track, tag] : links) {
          if (!track_ids.count(track)) rep.add(Severity::Error, "fk.tagtrack_track", "tag link references missing track " + std::to_string(track));
          if (!tag_ids.count(tag) || cat_ids.count(tag)) rep.add(Severity::Error, "fk.tagtrack_tag", "tag link references missing tag " + std::to_string(tag));
        }
      }
    }
    return rep.ok();
  } catch (const std::exception& ex) {
    rep.add(Severity::Error, "validator.exception", ex.what());
  } catch (...) {
    rep.add(Severity::Error, "validator.exception", "unknown failure");
  }
  return false;
}

}  // namespace deckhand::usbexport
