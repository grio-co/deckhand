// Reference-export comparison harness (compare_exports). Parses a vendor-made reference stick and a DeckHand
// stick with the same low-level readers as the validator and reports field-level, structural and ANLZ-tag
// differences, plus a histogram of the values the reference uses for every field DeckHand guesses.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

#include "bytes.hpp"
#include "deckhand/export/export_check.hpp"
#include "rawio.hpp"

namespace deckhand::usbexport {

using namespace detail;
namespace fs = std::filesystem;

namespace {

using Fields = std::map<std::string, std::string>;
struct NRow {
  std::string key;
  Fields f;
};

std::string hx(std::uint32_t v, int w = 0) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%0*X", w, v);
  return b;
}
std::string dec(std::uint32_t v) { return std::to_string(v); }
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string show(const std::string& s) {
  std::string o = s.substr(0, 48);
  for (char& c : o)
    if (static_cast<unsigned char>(c) < 0x20) c = '?';
  return "\"" + o + (s.size() > 48 ? "..." : "") + "\"";
}

struct Side {
  fs::path root;
  PdbFile pdb;
  std::map<std::uint32_t, std::string> artists, genres, labels, keys, colors, albums, track_names, playlist_path;
  std::map<std::uint32_t, std::vector<NRow>> rows;  // by table type
  std::vector<NRow> tracks;                          // key = file name (lower), "title" field used as fallback
};

std::string str_at(const View& r, std::size_t off, bool isrc = false) {
  const DsqlStr s = dsql_read(r, off, isrc);
  if (!s.ok()) return "<bad>";
  const char kind = s.kind == StrKind::ShortAscii ? 'a' : s.kind == StrKind::LongAscii ? 'A' : s.kind == StrKind::Utf16 ? 'u' : s.kind == StrKind::Isrc ? 'i' : 'n';
  return std::string(1, kind) + ":" + s.text;
}
std::string text_at(const View& r, std::size_t off) { return dsql_read(r, off).text; }

bool load_side(const std::string& root, Side& s, std::string& error) {
  s.root = root;
  Bytes db;
  if (!read_all(s.root / "PIONEER/rekordbox/export.pdb", db)) {
    error = "compare_exports: cannot read " + (s.root / "PIONEER/rekordbox/export.pdb").string();
    return false;
  }
  if (!s.pdb.load(std::move(db))) {
    error = "compare_exports: unusable export.pdb in " + root;
    return false;
  }
  const PdbFile& f = s.pdb;
  // Pass 1: dictionaries.
  for (const PdbTable& t : f.tables) {
    for (const PdbRow& row : t.rows) {
      if (!row.present) continue;
      const View r{f.data, row.abs};
      switch (t.type) {
        case 1: s.genres[r.le32(0)] = text_at(r, 4); break;
        case 4: s.labels[r.le32(0)] = text_at(r, 4); break;
        case 5: s.keys[r.le32(0)] = text_at(r, 8); break;
        case 6: s.colors[r.le16(5)] = text_at(r, 8); break;
        case 2: s.artists[r.le32(4)] = text_at(r, (r.le16(0) & 4) ? r.le16(0x0A) : r.u8(9)); break;
        default: break;
      }
    }
  }
  for (const PdbTable& t : f.tables) {
    for (const PdbRow& row : t.rows) {
      if (!row.present) continue;
      const View r{f.data, row.abs};
      if (t.type == 3) s.albums[r.le32(0x0C)] = text_at(r, (r.le16(0) & 4) ? r.le16(0x16) : r.u8(0x15));
    }
  }
  auto name_of = [](const std::map<std::uint32_t, std::string>& m, std::uint32_t id) {
    if (id == 0) return std::string();
    const auto it = m.find(id);
    return it == m.end() ? "<missing " + std::to_string(id) + ">" : it->second;
  };
  // Pass 2: tracks.
  std::map<std::string, int> seen;
  for (const PdbTable& t : f.tables) {
    if (t.type != 0) continue;
    for (const PdbRow& row : t.rows) {
      if (!row.present) continue;
      const View r{f.data, row.abs};
      NRow n;
      Fields& x = n.f;
      x["subtype"] = hx(r.le16(0), 4);
      x["index_shift"] = hx(r.le16(2), 4);
      x["bitmask"] = hx(r.le32(4), 8);
      x["sample_rate"] = dec(r.le32(8));
      x["composer"] = name_of(s.artists, r.le32(0x0C));
      x["file_size"] = dec(r.le32(0x10));
      x["u14"] = hx(r.le32(0x14), 8);
      x["u18"] = dec(r.le16(0x18));
      x["u1a"] = dec(r.le16(0x1A));
      x["artwork_id"] = dec(r.le32(0x1C));
      x["key"] = name_of(s.keys, r.le32(0x20));
      x["original_artist"] = name_of(s.artists, r.le32(0x24));
      x["label"] = name_of(s.labels, r.le32(0x28));
      x["remixer"] = name_of(s.artists, r.le32(0x2C));
      x["bitrate"] = dec(r.le32(0x30));
      x["track_number"] = dec(r.le32(0x34));
      x["tempo"] = dec(r.le32(0x38));
      x["genre"] = name_of(s.genres, r.le32(0x3C));
      x["album"] = name_of(s.albums, r.le32(0x40));
      x["artist"] = name_of(s.artists, r.le32(0x44));
      x["id"] = dec(r.le32(0x48));
      x["disc_number"] = dec(r.le16(0x4C));
      x["play_count"] = dec(r.le16(0x4E));
      x["year"] = dec(r.le16(0x50));
      x["sample_depth"] = dec(r.le16(0x52));
      x["duration"] = dec(r.le16(0x54));
      x["u56"] = hx(r.le16(0x56), 4);
      x["color_id"] = dec(r.u8(0x58));
      x["rating"] = dec(r.u8(0x59));
      x["file_type"] = hx(r.le16(0x5A), 4);
      x["u5c"] = hx(r.le16(0x5C), 4);
      static const char* const names[21] = {"isrc", "lyricist", "str2", "str3", "str4", "message", "publish_track_info",
                                            "autoload_hotcues", "str8", "str9", "date_added", "release_date", "mix_name",
                                            "str13", "analyze_path", "analyze_date", "comment", "title", "str18",
                                            "file_name", "file_path"};
      for (int i = 0; i < 21; ++i) x[std::string("s_") + names[i]] = str_at(r, r.le16(0x5E + 2 * i), i == 0);
      const std::string fname = lower(text_at(r, r.le16(0x5E + 2 * 19)));
      n.key = fname;
      if (seen[n.key]++) n.key += "#" + std::to_string(seen[n.key]);
      s.track_names[r.le32(0x48)] = fname;
      s.tracks.push_back(std::move(n));
    }
  }
  // Pass 3: other tables.
  for (const PdbTable& t : f.tables) {
    if (t.type == 0) continue;
    std::vector<NRow>& out = s.rows[t.type];
    std::size_t ordinal = 0;
    for (const PdbRow& row : t.rows) {
      if (!row.present) continue;
      const View r{f.data, row.abs};
      NRow n;
      switch (t.type) {
        case 1: case 4:
          n.key = text_at(r, 4);
          break;
        case 2:
          n.key = text_at(r, (r.le16(0) & 4) ? r.le16(0x0A) : r.u8(9));
          n.f["subtype"] = hx(r.le16(0), 4);
          n.f["index_shift"] = hx(r.le16(2), 4);
          n.f["u08"] = dec(r.u8(8));
          break;
        case 3:
          n.key = name_of(s.artists, r.le32(8)) + "/" + text_at(r, (r.le16(0) & 4) ? r.le16(0x16) : r.u8(0x15));
          n.f["subtype"] = hx(r.le16(0), 4);
          n.f["index_shift"] = hx(r.le16(2), 4);
          n.f["u04"] = hx(r.le32(4), 8);
          n.f["u10"] = hx(r.le32(0x10), 8);
          n.f["u14"] = dec(r.u8(0x14));
          break;
        case 5:
          n.key = text_at(r, 8);
          n.f["id_equals_id2"] = r.le32(0) == r.le32(4) ? "yes" : "no";
          break;
        case 6:
          n.key = dec(r.le16(5));
          n.f["unknown1"] = hx(r.le32(0), 8) + " " + hx(r.u8(4), 2);
          n.f["u7"] = dec(r.u8(7));
          n.f["name"] = text_at(r, 8);
          break;
        case 7: {
          const std::uint32_t id = r.le32(0x0C), parent = r.le32(0);
          n.f["unknown"] = hx(r.le32(4), 8);
          n.f["sort_order"] = dec(r.le32(8));
          n.f["is_folder"] = r.le32(0x10) ? "yes" : "no";
          const std::string nm = text_at(r, 0x14);
          const auto pit = s.playlist_path.find(parent);
          n.key = (parent && pit != s.playlist_path.end() ? pit->second : std::string()) + "/" + nm;
          s.playlist_path[id] = n.key;
          break;
        }
        case 8:
          n.key = std::to_string(r.le32(8));  // playlist id, resolved after all tree rows are known
          n.f["entry_index"] = dec(r.le32(0));
          n.f["track"] = name_of(s.track_names, r.le32(4));
          break;
        case 16:
          n.key = "#" + std::to_string(ordinal);
          n.f["u0"] = dec(r.le16(0));
          n.f["u1"] = hx(r.le16(2), 4);
          n.f["name"] = text_at(r, 4);
          break;
        case 13:
          n.key = text_at(r, 4);
          break;
        default:
          n.key = "#" + std::to_string(ordinal);
          break;
      }
      ++ordinal;
      out.push_back(std::move(n));
    }
  }
  // Entries: key by playlist path and ordinal within the playlist (sorted by entry_index).
  auto& ent = s.rows[8];
  for (NRow& e : ent) e.key = [&] {
    const auto it = s.playlist_path.find(static_cast<std::uint32_t>(std::stoul(e.key)));
    return it == s.playlist_path.end() ? "<missing playlist " + e.key + ">" : it->second;
  }();
  std::stable_sort(ent.begin(), ent.end(), [](const NRow& a, const NRow& b) {
    if (a.key != b.key) return a.key < b.key;
    return std::stoul(a.f.at("entry_index")) < std::stoul(b.f.at("entry_index"));
  });
  std::map<std::string, std::size_t> ord;
  for (NRow& e : ent) {
    const std::size_t o = ord[e.key]++;
    e.f["entry_index_minus_position"] = std::to_string(static_cast<long>(std::stoul(e.f["entry_index"])) - static_cast<long>(o));
    e.key += "#" + std::to_string(o);
  }
  return true;
}

// ---- table level facts -------------------------------------------------------------------------------------

std::string join(const std::set<std::string>& s) {
  std::string o;
  for (const auto& x : s) o += (o.empty() ? "" : ",") + x;
  return o.empty() ? "-" : o;
}

Fields table_facts(const PdbFile& f, const PdbTable& t) {
  Fields o;
  std::size_t present = 0, offsets = 0;
  std::set<std::string> data_flags, u14, u24, u26, free_ok, trans_ok, trcri;
  std::size_t min_rows = ~std::size_t{0}, max_rows = 0;
  bool aligned = true, contiguous = true;
  for (std::size_t i = 1; i < t.chain.size(); ++i)
    if (t.chain[i] != t.chain[i - 1] + 1) contiguous = false;
  for (std::uint32_t pg : t.chain) {
    const PdbPage& g = f.pages[pg];
    if (pg == t.first) {
      o["index_page_flags"] = hx(g.flags, 2);
      o["index_page_hdr"] = hex_bytes(f.data, static_cast<std::size_t>(pg) * f.page_size + 0x1C, 0x40 - 0x1C);
      o["index_page_hdr(0x00-0x1B)"] = hex_bytes(f.data, static_cast<std::size_t>(pg) * f.page_size + 0x14, 8);
      continue;
    }
    if (!g.is_data()) continue;
    data_flags.insert(hx(g.flags, 2));
    u14.insert(hx(g.u14, 8));
    u24.insert(hx(g.u24, 4));
    u26.insert(hx(g.u26, 4));
    present += g.num_rows;
    offsets += g.num_offsets;
    min_rows = std::min<std::size_t>(min_rows, g.num_rows);
    max_rows = std::max<std::size_t>(max_rows, g.num_rows);
    const std::size_t idx = PdbFile::index_bytes(g.num_offsets);
    free_ok.insert(g.free_size + g.used_size + idx + kHeapStart == f.page_size ? "free+used+index+0x28==page" : "free/used differ");
    trcri.insert((g.trc == g.num_rows ? "trc==rows" : "trc!=rows") + std::string(g.tri == 0 ? ",tri=0" : ",tri=" + hx(g.tri)));
    const View pv{f.data, static_cast<std::size_t>(pg) * f.page_size};
    bool same = true;
    for (std::size_t grp = 0; grp * 16 < g.num_offsets; ++grp)
      same = same && pv.le16(f.page_size - 36 * grp - 4) == pv.le16(f.page_size - 36 * grp - 2);
    trans_ok.insert(same ? "trans flags==presence" : "trans flags differ");
  }
  for (const PdbRow& row : t.rows)
    if (row.ofs % 4) aligned = false;
  o["pages"] = std::to_string(t.chain.size());
  o["rows"] = std::to_string(present);
  o["row_offsets"] = std::to_string(offsets);
  o["rows_per_page"] = present || max_rows ? std::to_string(min_rows) + ".." + std::to_string(max_rows) : "-";
  o["contiguous_pages"] = contiguous ? "yes" : "no";
  o["data_page_flags"] = join(data_flags);
  o["page_hdr_0x14"] = join(u14);
  o["page_hdr_0x24"] = join(u24);
  o["page_hdr_0x26"] = join(u26);
  o["free_used_sizes"] = join(free_ok);
  o["transaction_counts"] = join(trcri);
  o["transaction_row_flags"] = join(trans_ok);
  o["row_offsets_aligned4"] = aligned ? "yes" : "no";
  if (t.last < f.num_pages) {
    const std::uint32_t nx = f.pages[t.last].next;
    std::string rel = nx == t.empty_candidate ? "empty_candidate" : nx >= f.num_pages ? "beyond file (" + hx(nx) + ")" : nx == t.last ? "itself" : "other page";
    o["last_page.next_page"] = rel;
  }
  o["empty_candidate"] = t.empty_candidate > t.last ? "after last_page" : "before last_page";
  return o;
}

// ---- ANLZ --------------------------------------------------------------------------------------------------

std::string tag_desc(const AnlzInfo& a, const AnlzTag& t, bool payload) {
  std::string s = "hdr=" + std::to_string(t.len_header) + " body[" + hex_bytes(a.data, t.pos + 12, t.len_header > 12 ? t.len_header - 12 : 0) + "]";
  if (payload) s += " payload[" + hex_bytes(a.data, t.pos + t.len_header, std::min<std::size_t>(0x38, t.len_tag - t.len_header)) + "]";
  return s;
}

void compare_anlz(const Side& ref, const Side& ours, const std::string& key, const NRow& rt, const NRow& ot,
                  std::map<std::string, std::pair<std::size_t, std::string>>& agg) {
  auto note = [&](const std::string& msg) {
    auto& e = agg[msg];
    if (e.first++ == 0) e.second = key;
  };
  for (const char* ext : {"DAT", "EXT", "2EX"}) {
    auto path_for = [&](const Side& sd, const NRow& r) {
      std::string p = r.f.at("s_analyze_path");
      p = p.size() > 2 ? p.substr(2) : p;
      if (!p.empty() && p[0] == '/') p = p.substr(1);
      fs::path fp = sd.root / p;
      fp.replace_extension(ext);
      return fp;
    };
    Bytes rb, ob;
    const bool rh = read_all(path_for(ref, rt), rb), oh = read_all(path_for(ours, ot), ob);
    const std::string e = std::string(".") + ext;
    if (rh != oh) {
      note(e + ": file only in the " + (rh ? "reference" : "ours"));
      continue;
    }
    if (!rh) continue;
    AnlzInfo ra, oa;
    ra.load(std::move(rb));
    oa.load(std::move(ob));
    if (!ra.problem.empty()) note(e + ": reference file does not parse cleanly: " + ra.problem);
    if (!ra.header_ok || !oa.header_ok) continue;
    if (ra.len_header != oa.len_header) note(e + ": PMAI len_header ref " + std::to_string(ra.len_header) + " ours " + std::to_string(oa.len_header));
    const std::size_t hl = std::min<std::size_t>(std::min(ra.len_header, oa.len_header), ra.data.size());
    const std::string rhd = hex_bytes(ra.data, 12, hl > 12 ? hl - 12 : 0), ohd = hex_bytes(oa.data, 12, hl > 12 ? hl - 12 : 0);
    if (rhd != ohd) note(e + ": PMAI header words 0x0C.. ref [" + rhd + "] ours [" + ohd + "]");
    std::string ro, oo;
    for (const auto& t : ra.tags) ro += (ro.empty() ? "" : " ") + t.fourcc;
    for (const auto& t : oa.tags) oo += (oo.empty() ? "" : " ") + t.fourcc;
    if (ro != oo) note(e + ": tag order ref [" + ro + "] ours [" + oo + "]");
    std::map<std::string, std::size_t> nth_r, nth_o;
    for (const auto& rtag : ra.tags) {
      const std::size_t n = nth_r[rtag.fourcc]++;
      const AnlzTag* otag = oa.find(rtag.fourcc.c_str(), n);
      const bool payload = rtag.fourcc == "PCOB" || rtag.fourcc == "PCO2" || rtag.fourcc == "PQTZ";
      if (!otag) {
        note(e + ": tag " + rtag.fourcc + " only in the reference: " + tag_desc(ra, rtag, true));
        continue;
      }
      const std::string a = tag_desc(ra, rtag, payload), b = tag_desc(oa, *otag, payload);
      if (a != b) note(e + ": tag " + rtag.fourcc + " differs: ref " + a + " | ours " + b);
      if (rtag.len_tag != otag->len_tag && !payload)
        note(e + ": tag " + rtag.fourcc + " len_tag ref " + std::to_string(rtag.len_tag) + " ours " + std::to_string(otag->len_tag) + " (data tags differ with track length)");
    }
    for (const auto& otag : oa.tags) {
      const std::size_t n = nth_o[otag.fourcc]++;
      if (!ra.find(otag.fourcc.c_str(), n)) note(e + ": tag " + otag.fourcc + " only in ours");
    }
  }
}

std::string shape(const std::string& rel) {
  if (rel.rfind("Contents/", 0) == 0) return "Contents/<audio>";
  std::string o;
  for (std::size_t i = 0; i < rel.size();) {
    std::size_t j = i;
    while (j < rel.size() && std::isxdigit(static_cast<unsigned char>(rel[j])) && !std::islower(static_cast<unsigned char>(rel[j]))) ++j;
    if (j - i >= 3) {
      o += "<hex" + std::to_string(j - i) + ">";
      i = j;
    } else {
      o.push_back(rel[i++]);
    }
  }
  return o;
}

std::map<std::string, std::size_t> inventory(const fs::path& root) {
  std::map<std::string, std::size_t> m;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    ++m[shape(it->path().lexically_relative(root).generic_string())];
  }
  return m;
}

}  // namespace

bool compare_exports(const std::string& reference_root, const std::string& ours_root, CompareReport& rep, std::string& error) {
  error.clear();
  rep = CompareReport{};
  try {
    Side ref, ours;
    if (!load_side(reference_root, ref, error) || !load_side(ours_root, ours, error)) return false;
    std::map<std::string, CompareReport::Histogram> hist;
    auto record = [&](const std::string& field, const std::string& ref_value, const std::string& our_value) {
      auto& h = hist[field];
      h.field = field;
      ++h.reference[ref_value];
      if (h.ours.empty()) h.ours = our_value;
    };

    // ---- header and table structure
    auto hdr = [&](const char* what, const std::string& a, const std::string& b) {
      if (a != b) rep.structure.push_back(std::string("file header ") + what + ": reference " + a + " | ours " + b);
    };
    hdr("page size", dec(ref.pdb.page_size), dec(ours.pdb.page_size));
    hdr("table count", dec(ref.pdb.num_tables), dec(ours.pdb.num_tables));
    hdr("word 0x10", hx(ref.pdb.unk10), hx(ours.pdb.unk10));
    hdr("next_unused_page - page count", std::to_string(static_cast<long>(ref.pdb.next_unused) - static_cast<long>(ref.pdb.num_pages)),
        std::to_string(static_cast<long>(ours.pdb.next_unused) - static_cast<long>(ours.pdb.num_pages)));
    hdr("sequence - page count", std::to_string(static_cast<long>(ref.pdb.seq) - static_cast<long>(ref.pdb.num_pages)),
        std::to_string(static_cast<long>(ours.pdb.seq) - static_cast<long>(ours.pdb.num_pages)));
    {
      std::string ro, oo;
      for (const auto& t : ref.pdb.tables) ro += (ro.empty() ? "" : " ") + hx(t.type);
      for (const auto& t : ours.pdb.tables) oo += (oo.empty() ? "" : " ") + hx(t.type);
      if (ro != oo) rep.structure.push_back("table type order: reference [" + ro + "] | ours [" + oo + "]");
      auto order = [](const PdbFile& f) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> v;
        for (const auto& t : f.tables) v.emplace_back(t.first, t.type);
        std::sort(v.begin(), v.end());
        std::string s;
        for (const auto& [p, ty] : v) s += (s.empty() ? "" : " ") + hx(ty);
        return s;
      };
      hdr("page order of tables", order(ref.pdb), order(ours.pdb));
    }
    for (const PdbTable& rt : ref.pdb.tables) {
      const PdbTable* ot = ours.pdb.table(rt.type);
      const std::string tn = "table " + hx(rt.type);
      if (!ot) {
        rep.structure.push_back(tn + ": present in the reference only");
        continue;
      }
      const Fields rf = table_facts(ref.pdb, rt), of = table_facts(ours.pdb, *ot);
      for (const auto& [k, v] : rf) {
        const auto it = of.find(k);
        const std::string ov = it == of.end() ? "-" : it->second;
        if (v != ov) rep.structure.push_back(tn + " " + k + ": reference " + v + " | ours " + ov);
        if (k == "data_page_flags" || k == "page_hdr_0x24" || k == "page_hdr_0x26" || k == "page_hdr_0x14" || k == "transaction_counts" ||
            k == "last_page.next_page" || k == "index_page_hdr" || k == "free_used_sizes" || k == "transaction_row_flags")
          record("pdb." + tn + "." + k, v, ov);
      }
    }
    for (const PdbTable& ot : ours.pdb.tables)
      if (!ref.pdb.table(ot.type)) rep.structure.push_back("table " + hx(ot.type) + ": present in ours only");

    // ---- tracks
    std::map<std::string, const NRow*> ref_by_key;
    for (const NRow& r : ref.tracks) ref_by_key[r.key] = &r;
    std::map<std::string, const NRow*> ref_by_title;
    for (const NRow& r : ref.tracks) ref_by_title.emplace(lower(r.f.at("s_title")), &r);
    std::set<const NRow*> used;
    std::map<std::string, std::pair<std::size_t, std::string>> anlz_agg;
    static const std::set<std::string> skip = {"id", "s_date_added", "s_analyze_date", "artwork_id"};
    static const std::set<std::string> guessed = {"subtype", "bitmask", "u14", "u18", "u1a", "disc_number", "play_count", "sample_depth",
                                                  "u56", "u5c", "file_type", "s_isrc", "s_lyricist", "s_str2", "s_str3", "s_str4",
                                                  "s_message", "s_publish_track_info", "s_autoload_hotcues", "s_str8", "s_str9",
                                                  "s_release_date", "s_mix_name", "s_str13", "s_str18", "composer", "original_artist",
                                                  "label", "remixer", "index_shift"};
    for (const NRow& o : ours.tracks) {
      const NRow* r = nullptr;
      const auto k = ref_by_key.find(o.key);
      if (k != ref_by_key.end() && !used.count(k->second)) r = k->second;
      if (!r) {
        const auto t = ref_by_title.find(lower(o.f.at("s_title")));
        if (t != ref_by_title.end() && !used.count(t->second)) r = t->second;
      }
      if (!r) {
        rep.unmatched_ours.push_back(o.key);
        continue;
      }
      used.insert(r);
      ++rep.matched_tracks;
      for (const auto& [name, rv] : r->f) {
        const std::string& ov = o.f.at(name);
        if (guessed.count(name)) record("track." + name, rv, ov);
        if (skip.count(name) || rv == ov) continue;
        rep.fields.push_back({"track '" + o.key + "'", name, rv, ov});
      }
      compare_anlz(ref, ours, o.key, *r, o, anlz_agg);
    }
    for (const NRow& r : ref.tracks)
      if (!used.count(&r)) rep.unmatched_reference.push_back(r.key);
    for (const auto& [msg, e] : anlz_agg)
      rep.anlz.push_back(msg + (e.first > 1 ? "  (" + std::to_string(e.first) + " tracks, e.g. " + e.second + ")" : "  (" + e.second + ")"));

    // ---- other tables
    for (const auto& [type, rrows] : ref.rows) {
      const auto oit = ours.rows.find(type);
      const std::string tn = "table " + hx(type);
      if (oit == ours.rows.end()) continue;
      std::map<std::string, const NRow*> om;
      for (const NRow& o : oit->second) om.emplace(o.key, &o);
      std::set<std::string> rkeys;
      std::size_t only_ref = 0;
      std::string sample;
      for (const NRow& r : rrows) {
        rkeys.insert(r.key);
        const auto it = om.find(r.key);
        if (it == om.end()) {
          if (++only_ref <= 3) sample += (sample.empty() ? "" : ", ") + show(r.key);
          continue;
        }
        for (const auto& [name, rv] : r.f) {
          const std::string ov = it->second->f.count(name) ? it->second->f.at(name) : "-";
          record(tn + "." + name, rv, ov);
          if (rv != ov) rep.fields.push_back({tn + " '" + r.key + "'", name, rv, ov});
        }
      }
      std::size_t only_ours = 0;
      for (const NRow& o : oit->second)
        if (!rkeys.count(o.key)) ++only_ours;
      if (only_ref || only_ours) {
        // Dictionary contents legitimately differ when the playlists differ; report counts, not an error.
        rep.structure.push_back(tn + " rows: " + std::to_string(only_ref) + " only in the reference (e.g. " + sample + "), " +
                                std::to_string(only_ours) + " only in ours");
      }
    }
    for (const auto& [type, orows] : ours.rows)
      if (!ref.rows.count(type) && !orows.empty()) rep.structure.push_back("table " + hx(type) + " has " + std::to_string(orows.size()) + " rows in ours and none in the reference");

    // ---- file inventory
    {
      const auto ri = inventory(ref.root), oi = inventory(ours.root);
      for (const auto& [shape_name, n] : ri) {
        const auto it = oi.find(shape_name);
        if (it == oi.end()) rep.structure.push_back("file kind only in the reference: " + shape_name + " (" + std::to_string(n) + ")");
      }
      for (const auto& [shape_name, n] : oi)
        if (!ri.count(shape_name)) rep.structure.push_back("file kind only in ours: " + shape_name + " (" + std::to_string(n) + ")");
    }
    for (auto& [name, h] : hist) rep.histograms.push_back(std::move(h));
    return true;
  } catch (const std::exception& e) {
    error = std::string("compare_exports: ") + e.what();
  } catch (...) {
    error = "compare_exports: unknown failure";
  }
  return false;
}

std::string CompareReport::text() const {
  std::string s = "matched tracks: " + std::to_string(matched_tracks) + "\n";
  auto list = [&](const char* title, const std::vector<std::string>& v) {
    if (v.empty()) return;
    s += std::string("\n== ") + title + " (" + std::to_string(v.size()) + ")\n";
    for (const auto& x : v) s += "  " + x + "\n";
  };
  list("tracks only in the reference", unmatched_reference);
  list("tracks only in ours", unmatched_ours);
  list("structure", structure);
  if (!fields.empty()) {
    s += "\n== field differences (" + std::to_string(fields.size()) + ")\n";
    for (const auto& d : fields) s += "  " + d.where + " " + d.field + ": reference " + d.reference + " | ours " + d.ours + "\n";
  }
  list("analysis files", anlz);
  if (!histograms.empty()) {
    s += "\n== values the reference uses for fields DeckHand guesses\n";
    for (const auto& h : histograms) {
      s += "  " + h.field + "  ours=" + h.ours + "  reference:";
      std::size_t shown = 0;
      for (const auto& [v, n] : h.reference) {
        if (++shown > 8) {
          s += " ...";
          break;
        }
        s += " " + v + " x" + std::to_string(n);
      }
      s += "\n";
    }
  }
  return s;
}

}  // namespace deckhand::usbexport
