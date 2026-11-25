#pragma once
// Low-level readers shared by the read-back parser, the validator and the comparison harness. They look at
// bytes on disk only (no writer bookkeeping) and never throw on malformed input.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "bytes.hpp"

namespace deckhand::usbexport::detail {

struct View {
  const Bytes& d;
  std::size_t base = 0;
  bool ok(std::size_t off, std::size_t n) const {
    return base + off >= base && base + off + n >= base + off && base + off + n <= d.size();
  }
  std::uint32_t le16(std::size_t o) const { return ok(o, 2) ? d[base + o] | (d[base + o + 1] << 8) : 0; }
  std::uint32_t le32(std::size_t o) const { return ok(o, 4) ? le16(o) | (le16(o + 2) << 16) : 0; }
  std::uint32_t be16(std::size_t o) const { return ok(o, 2) ? (d[base + o] << 8) | d[base + o + 1] : 0; }
  std::uint32_t be32(std::size_t o) const { return ok(o, 4) ? (be16(o) << 16) | be16(o + 2) : 0; }
  std::uint32_t u8(std::size_t o) const { return ok(o, 1) ? d[base + o] : 0; }
};

inline bool read_all(const std::filesystem::path& p, Bytes& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// ---- DeviceSQL strings -------------------------------------------------------------------------------------

enum class StrKind { Bad, ShortAscii, LongAscii, Utf16, Utf8, Isrc };

struct DsqlStr {
  StrKind kind = StrKind::Bad;
  std::string text;         // UTF-8
  std::size_t extent = 0;   // bytes occupied (flag byte included)
  unsigned flag = 0;        // first byte
  bool ok() const { return kind != StrKind::Bad; }
};

// `isrc_slot`: the string is the first pointer of a track row, where kind 0x90 may hold an ISRC instead of UTF-16.
inline DsqlStr dsql_read(const View& v, std::size_t off, bool isrc_slot = false) {
  DsqlStr s;
  if (!v.ok(off, 1)) return s;
  const unsigned lk = v.u8(off);
  s.flag = lk;
  if (lk & 1) {
    const std::size_t len = lk >> 1;
    if (len < 1 || !v.ok(off, len)) return s;
    s.text.assign(reinterpret_cast<const char*>(&v.d[v.base + off + 1]), len - 1);
    s.kind = StrKind::ShortAscii;
    s.extent = len;
    return s;
  }
  const std::size_t len = v.le16(off + 1);
  if (len < 4 || !v.ok(off, len)) return s;
  const std::size_t n = len - 4;
  const std::uint8_t* p = &v.d[v.base + off + 4];
  s.extent = len;
  if (lk == 0x40) {
    s.text.assign(reinterpret_cast<const char*>(p), n);
    s.kind = StrKind::LongAscii;
  } else if (lk == 0x90) {
    if (isrc_slot && n >= 1 && p[0] == 0x03) {
      std::size_t e = 1;
      while (e < n && p[e] != 0) ++e;
      s.text.assign(reinterpret_cast<const char*>(p + 1), e - 1);
      s.kind = StrKind::Isrc;
    } else {
      std::u16string u;
      for (std::size_t i = 0; i + 1 < n; i += 2) u.push_back(static_cast<char16_t>(p[i] | (p[i + 1] << 8)));
      s.text = utf16_to_utf8(u);
      s.kind = StrKind::Utf16;
    }
  } else if (lk == 0x20) {  // "narrow" UTF-8: documented as a possible flag combination, never seen in practice
    s.text.assign(reinterpret_cast<const char*>(p), n);
    s.kind = StrKind::Utf8;
  }
  return s;
}

inline std::string utf16be_z(const View& v, std::size_t off, std::size_t bytes) {
  std::u16string u;
  for (std::size_t i = 0; i + 1 < bytes && v.ok(off + i, 2); i += 2) {
    const char16_t c = static_cast<char16_t>(v.be16(off + i));
    if (c == 0) break;
    u.push_back(c);
  }
  return utf16_to_utf8(u);
}

inline std::string hex_bytes(const Bytes& d, std::size_t from, std::size_t n) {
  static const char* h = "0123456789abcdef";
  std::string s;
  for (std::size_t i = from; i < from + n && i < d.size(); ++i) {
    s.push_back(h[d[i] >> 4]);
    s.push_back(h[d[i] & 15]);
    if (i + 1 < from + n && i + 1 < d.size()) s.push_back(' ');
  }
  return s;
}

// ---- export.pdb / exportExt.pdb page walk ------------------------------------------------------------------

constexpr std::size_t kHeapStart = 0x28;

struct PdbPage {
  std::uint32_t index = 0, type = 0, next = 0, seq = 0, u14 = 0;
  std::uint32_t num_offsets = 0, num_rows = 0;
  unsigned flags = 0, free_size = 0, used_size = 0, trc = 0, tri = 0, u24 = 0, u26 = 0;
  bool is_data() const { return (flags & 0x40) == 0; }
};

struct PdbRow {
  std::uint32_t page = 0, slot = 0, ofs = 0;  // ofs: heap offset (bytes past the page header)
  bool present = false;
  std::size_t abs = 0;  // absolute file offset of the row
};

struct PdbTable {
  std::uint32_t type = 0, empty_candidate = 0, first = 0, last = 0;
  std::vector<std::uint32_t> chain;  // pages reached from first by next_page (first included)
  bool chain_ok = true;
  std::vector<PdbRow> rows;          // every allocated slot (present or not), in page/slot order
};

struct PdbProblem {
  std::string code, message;
};

struct PdbFile {
  Bytes data;
  std::uint32_t page_size = 0, num_pages = 0, num_tables = 0, next_unused = 0, unk10 = 0, seq = 0;
  std::vector<PdbTable> tables;
  std::vector<PdbPage> pages;  // by page index
  std::vector<PdbProblem> problems;
  bool usable = false;  // header parsed; tables may still have problems

  View view(std::size_t abs = 0) const { return View{data, abs}; }
  const PdbTable* table(std::uint32_t type) const {
    for (const auto& t : tables)
      if (t.type == type) return &t;
    return nullptr;
  }
  static std::size_t index_bytes(std::size_t n) {
    if (n == 0) return 0;
    const std::size_t full = (n - 1) / 16;
    return full * 36 + 4 + 2 * (n - full * 16);
  }

  void problem(const char* code, std::string msg) { problems.push_back({code, std::move(msg)}); }

  // Parses the header and walks every table's page chain defensively. Problems are recorded, never thrown.
  bool load(Bytes bytes) {
    data = std::move(bytes);
    const View h{data, 0};
    page_size = h.le32(4);
    num_tables = h.le32(8);
    next_unused = h.le32(12);
    unk10 = h.le32(16);
    seq = h.le32(20);
    if (page_size < 512 || (page_size & (page_size - 1)) != 0 || data.size() % page_size != 0 || data.empty()) {
      problem("pdb.header", "bad page size or file length");
      return false;
    }
    num_pages = static_cast<std::uint32_t>(data.size() / page_size);
    if (h.le32(0) != 0) problem("pdb.header", "first word is not zero");
    if (num_tables == 0 || num_tables > 64 || 0x1C + 16ull * num_tables > page_size) {
      problem("pdb.header", "bad table count");
      return false;
    }
    usable = true;
    pages.resize(num_pages);
    for (std::uint32_t p = 0; p < num_pages; ++p) {
      const View v{data, static_cast<std::size_t>(p) * page_size};
      PdbPage& g = pages[p];
      g.index = v.le32(4);
      g.type = v.le32(8);
      g.next = v.le32(12);
      g.seq = v.le32(16);
      g.u14 = v.le32(0x14);
      const std::uint32_t packed = v.u8(0x18) | (v.u8(0x19) << 8) | (v.u8(0x1A) << 16);
      g.num_offsets = packed & 0x1FFF;
      g.num_rows = packed >> 13;
      g.flags = v.u8(0x1B);
      g.free_size = v.le16(0x1C);
      g.used_size = v.le16(0x1E);
      g.trc = v.le16(0x20);
      g.tri = v.le16(0x22);
      g.u24 = v.le16(0x24);
      g.u26 = v.le16(0x26);
    }
    std::vector<int> owner(num_pages, -1);
    for (std::uint32_t ti = 0; ti < num_tables; ++ti) {
      const std::size_t tp = 0x1C + 16 * static_cast<std::size_t>(ti);
      PdbTable t;
      t.type = h.le32(tp);
      t.empty_candidate = h.le32(tp + 4);
      t.first = h.le32(tp + 8);
      t.last = h.le32(tp + 12);
      const std::string tn = "table " + std::to_string(t.type);
      if (t.first >= num_pages || t.last >= num_pages) {
        problem("pdb.table_range", tn + ": first/last page outside the file");
        t.chain_ok = false;
      } else {
        std::uint32_t pg = t.first;
        bool reached_last = false;
        for (std::uint32_t hops = 0;; ++hops) {
          if (hops > num_pages) {
            problem("pdb.chain_loop", tn + ": page chain does not terminate");
            t.chain_ok = false;
            break;
          }
          if (owner[pg] >= 0) {
            problem("pdb.chain_shared", tn + ": page " + std::to_string(pg) + " already belongs to another chain");
            t.chain_ok = false;
            break;
          }
          owner[pg] = static_cast<int>(ti);
          t.chain.push_back(pg);
          const PdbPage& g = pages[pg];
          if (g.index != pg) {
            problem("pdb.page_index", tn + ": page " + std::to_string(pg) + " records index " + std::to_string(g.index));
            t.chain_ok = false;
          }
          if (g.type != t.type) {
            problem("pdb.page_type", tn + ": page " + std::to_string(pg) + " has type " + std::to_string(g.type));
            t.chain_ok = false;
          }
          if (pg == t.last) {
            reached_last = true;
            break;
          }
          if (g.next >= num_pages) {
            problem("pdb.next_range", tn + ": next_page " + std::to_string(g.next) + " of page " + std::to_string(pg) +
                                          " is outside the file before last_page");
            t.chain_ok = false;
            break;
          }
          pg = g.next;
        }
        if (!reached_last && t.chain_ok) {
          problem("pdb.chain_last", tn + ": last_page not reached");
          t.chain_ok = false;
        }
      }
      for (std::uint32_t pg : t.chain) {
        const PdbPage& g = pages[pg];
        if (!g.is_data()) continue;
        const std::size_t pbase = static_cast<std::size_t>(pg) * page_size;
        const View pv{data, pbase};
        for (std::uint32_t k = 0; k < g.num_offsets; ++k) {
          const std::size_t grp = k / 16, i = k % 16, bs = page_size - 36 * grp;
          if (bs < 6 + 2 * i + 4 || 36 * grp + 36 > page_size) break;
          PdbRow r;
          r.page = pg;
          r.slot = k;
          r.ofs = pv.le16(bs - 6 - 2 * i);
          r.present = ((pv.le16(bs - 4) >> i) & 1) != 0;
          r.abs = pbase + kHeapStart + r.ofs;
          t.rows.push_back(r);
        }
      }
      tables.push_back(std::move(t));
    }
    return true;
  }
};

// ---- ANLZ tag inventory ------------------------------------------------------------------------------------

struct AnlzTag {
  std::string fourcc;
  std::uint32_t len_header = 0, len_tag = 0;
  std::size_t pos = 0;  // file offset
};

struct AnlzInfo {
  Bytes data;
  bool header_ok = false;
  std::uint32_t len_header = 0, len_file = 0;
  std::vector<AnlzTag> tags;
  std::string problem;  // empty = tag lengths walk the file exactly
  bool load(Bytes bytes) {
    data = std::move(bytes);
    const View v{data, 0};
    if (data.size() < 0x1C || std::string(data.begin(), data.begin() + 4) != "PMAI") {
      problem = "missing PMAI header";
      return false;
    }
    len_header = v.be32(4);
    len_file = v.be32(8);
    header_ok = true;
    if (len_header < 0x0C || len_header > data.size()) {
      problem = "bad header length";
      return false;
    }
    if (len_file != data.size()) problem = "len_file does not match the file size";
    std::size_t pos = len_header;
    while (pos < data.size()) {
      if (pos + 12 > data.size()) {
        if (problem.empty()) problem = "truncated tag envelope";
        return false;
      }
      AnlzTag t;
      t.fourcc.assign(data.begin() + pos, data.begin() + pos + 4);
      t.len_header = v.be32(pos + 4);
      t.len_tag = v.be32(pos + 8);
      t.pos = pos;
      if (t.len_tag < 12 || t.len_header < 12 || t.len_header > t.len_tag || pos + t.len_tag > data.size()) {
        if (problem.empty()) problem = "tag " + t.fourcc + " has an impossible length";
        return false;
      }
      tags.push_back(t);
      pos += t.len_tag;
    }
    return problem.empty();
  }
  const AnlzTag* find(const char* cc, std::size_t nth = 0) const {
    for (const auto& t : tags)
      if (t.fourcc == cc && nth-- == 0) return &t;
    return nullptr;
  }
};

}  // namespace deckhand::usbexport::detail
