// Read-back parser for export.pdb and ANLZ files. Written from the public format description only; it
// shares nothing with the writer except primitive byte/UTF helpers (bytes.hpp).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

#include "bytes.hpp"
#include "deckhand/export/export.hpp"

namespace deckhand::usbexport {
namespace {

using detail::Bytes;
namespace fs = std::filesystem;

struct View {
  const Bytes& d;
  std::size_t base = 0;
  bool ok(std::size_t off, std::size_t n) const { return base + off + n <= d.size() && base + off + n >= base + off; }
  std::uint32_t le16(std::size_t o) const { return ok(o, 2) ? d[base + o] | (d[base + o + 1] << 8) : 0; }
  std::uint32_t le32(std::size_t o) const { return ok(o, 4) ? le16(o) | (le16(o + 2) << 16) : 0; }
  std::uint32_t be16(std::size_t o) const { return ok(o, 2) ? (d[base + o] << 8) | d[base + o + 1] : 0; }
  std::uint32_t be32(std::size_t o) const { return ok(o, 4) ? (be16(o) << 16) | be16(o + 2) : 0; }
  std::uint32_t u8(std::size_t o) const { return ok(o, 1) ? d[base + o] : 0; }
};

bool read_all(const fs::path& p, Bytes& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// DeviceSQL string at v+off. Returns false when malformed.
bool dsql(const View& v, std::size_t off, std::string& out) {
  out.clear();
  if (!v.ok(off, 1)) return false;
  const unsigned lk = v.u8(off);
  if (lk & 1) {  // short ASCII
    const std::size_t len = lk >> 1;
    if (len < 1 || !v.ok(off, len)) return false;
    out.assign(reinterpret_cast<const char*>(&v.d[v.base + off + 1]), len - 1);
    return true;
  }
  const std::size_t len = v.le16(off + 1);
  if (len < 4 || !v.ok(off, len)) return false;
  const std::size_t n = len - 4;
  const std::uint8_t* p = &v.d[v.base + off + 4];
  if (lk == 0x40) {
    out.assign(reinterpret_cast<const char*>(p), n);
    return true;
  }
  if (lk == 0x90) {
    std::u16string u;
    for (std::size_t i = 0; i + 1 < n; i += 2) u.push_back(static_cast<char16_t>(p[i] | (p[i + 1] << 8)));
    out = detail::utf16_to_utf8(u);
    return true;
  }
  return false;
}

std::string utf16be_z(const View& v, std::size_t off, std::size_t bytes) {
  std::u16string u;
  for (std::size_t i = 0; i + 1 < bytes && v.ok(off + i, 2); i += 2) {
    const char16_t c = static_cast<char16_t>(v.be16(off + i));
    if (c == 0) break;
    u.push_back(c);
  }
  return detail::utf16_to_utf8(u);
}

struct RawTrack {
  ReadTrack rt;
  std::uint32_t artist = 0, album = 0, genre = 0, key = 0, label = 0;
};

bool parse_anlz(const fs::path& path, ReadTrack& rt, int kind, std::string& error) {  // 0 DAT, 1 EXT, 2 2EX
  Bytes data;
  if (!read_all(path, data)) {
    error = "read_export: cannot read " + path.string();
    return false;
  }
  const View v{data, 0};
  if (data.size() < 0x1C || std::string(data.begin(), data.begin() + 4) != "PMAI" || v.be32(8) != data.size()) {
    error = "read_export: bad ANLZ header in " + path.string();
    return false;
  }
  std::size_t pos = v.be32(4);
  while (pos + 12 <= data.size()) {
    const std::string cc(data.begin() + pos, data.begin() + pos + 4);
    const std::size_t lh = v.be32(pos + 4), lt = v.be32(pos + 8);
    if (lt < 12 || pos + lt > data.size()) {
      error = "read_export: bad tag length in " + path.string();
      return false;
    }
    const View t{data, pos};
    if (cc == "PPTH") {
      (kind == 0 ? rt.ppth_dat : rt.ppth_ext) = utf16be_z(t, lh, t.be32(0x0C));
    } else if (cc == "PVBR") {
      rt.pvbr_entries = (lt - lh) / 4;
    } else if (cc == "PQTZ") {
      const std::size_t n = t.be32(0x14);
      if (lh + n * 8 > lt) {
        error = "read_export: PQTZ overruns its tag";
        return false;
      }
      rt.track.beats.clear();
      for (std::size_t i = 0; i < n; ++i) {
        const std::size_t e = lh + i * 8;
        ExportBeat b;
        b.beat_in_bar = static_cast<int>(t.be16(e));
        b.bpm = t.be16(e + 2) / 100.0;
        b.time_ms = t.be32(e + 4);
        rt.track.beats.push_back(b);
      }
    } else if (cc == "PCOB") {
      const std::size_t n = t.be16(0x12);
      std::size_t e = lh;
      for (std::size_t i = 0; i < n && e + 0x28 <= lt; ++i) {
        const std::size_t len = t.be32(e + 8);
        if (len < 0x28 || e + len > lt || std::string(data.begin() + pos + e, data.begin() + pos + e + 4) != "PCPT") break;
        ExportCue c;
        c.slot = static_cast<int>(t.be32(e + 0xC));
        c.hot = c.slot > 0;
        c.time_ms = t.be32(e + 0x20);
        if (t.u8(e + 0x1C) == 2) c.loop_end_ms = t.be32(e + 0x24);
        rt.dat_cues.push_back(c);
        e += len;
      }
    } else if (cc == "PCO2") {
      const std::size_t n = t.be16(0x10);
      std::size_t e = lh;
      for (std::size_t i = 0; i < n && e + 0x28 <= lt; ++i) {
        const std::size_t len = t.be32(e + 8);
        if (len < 0x28 || e + len > lt || std::string(data.begin() + pos + e, data.begin() + pos + e + 4) != "PCP2") break;
        ExportCue c;
        c.slot = static_cast<int>(t.be32(e + 0xC));
        c.hot = c.slot > 0;
        c.time_ms = t.be32(e + 0x14);
        if (t.u8(e + 0x10) == 2) c.loop_end_ms = t.be32(e + 0x18);
        c.color_id = static_cast<std::uint8_t>(t.u8(e + 0x1C));
        const std::size_t lc = t.be32(e + 0x28);
        if (lc && e + 0x2C + lc <= e + len) c.label = utf16be_z(t, e + 0x2C, lc);
        const std::size_t col = e + 0x2C + lc;
        if (col + 4 <= e + len && (t.u8(col + 1) || t.u8(col + 2) || t.u8(col + 3))) {
          c.has_rgb = true;
          c.r = static_cast<std::uint8_t>(t.u8(col + 1));
          c.g = static_cast<std::uint8_t>(t.u8(col + 2));
          c.b = static_cast<std::uint8_t>(t.u8(col + 3));
        }
        rt.track.cues.push_back(c);
        e += len;
      }
    } else if (cc == "PWAV" || cc == "PWV2") {
      const std::size_t n = t.be32(0x0C);
      if (lh + n > lt) {
        error = "read_export: preview overruns its tag";
        return false;
      }
      auto& dst = cc == "PWAV" ? rt.pwav : rt.pwv2;
      dst.assign(data.begin() + pos + lh, data.begin() + pos + lh + n);
    } else if (cc == "PWV3") {
      const std::size_t n = t.be32(0x10);
      if (t.be32(0x0C) != 1 || lh + n > lt) {
        error = "read_export: bad PWV3";
        return false;
      }
      rt.pwv3.assign(data.begin() + pos + lh, data.begin() + pos + lh + n);
    } else if (cc == "PWV4") {
      rt.pwv4_columns = t.be32(0x10);
    } else if (cc == "PWV5") {
      const std::size_t n = t.be32(0x10);
      if (t.be32(0x0C) != 2 || lh + n * 2 > lt) {
        error = "read_export: bad PWV5";
        return false;
      }
      rt.pwv5.clear();
      for (std::size_t i = 0; i < n; ++i) rt.pwv5.push_back(static_cast<std::uint16_t>(t.be16(lh + 2 * i)));
    } else if (cc == "PWV6") {
      rt.pwv6_columns = t.be32(0x10);
    } else if (cc == "PWV7") {
      rt.pwv7_entries = t.be32(0x10);
    }
    pos += lt;
  }
  return true;
}

}  // namespace

bool read_export(const std::string& root_str, ReadExport& out, std::string& error) {
  error.clear();
  out = ReadExport{};
  try {
    const fs::path root = root_str;
    Bytes db;
    if (!read_all(root / "PIONEER/rekordbox/export.pdb", db)) {
      error = "read_export: cannot read export.pdb";
      return false;
    }
    const View h{db, 0};
    const std::size_t page = h.le32(4), ntab = h.le32(8);
    if (page < 512 || db.size() % page != 0 || ntab == 0 || ntab > 64 || 0x1C + 16 * ntab > page) {
      error = "read_export: bad pdb header";
      return false;
    }
    out.page_size = static_cast<std::uint32_t>(page);
    out.num_pages = static_cast<std::uint32_t>(db.size() / page);
    out.table_rows.assign(ntab, 0);

    std::map<std::uint32_t, std::map<std::uint32_t, std::string>> names;  // type -> id -> name
    std::vector<RawTrack> raw;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::string>> albums;  // id -> artist, name
    struct Entry { std::uint32_t idx, track, list; };
    std::vector<Entry> entries;

    for (std::size_t ti = 0; ti < ntab; ++ti) {
      const std::size_t tp = 0x1C + 16 * ti;
      const std::uint32_t type = h.le32(tp), first = h.le32(tp + 8), last = h.le32(tp + 12);
      std::uint32_t pg = first;
      for (std::size_t hops = 0; hops <= out.num_pages; ++hops) {
        if (pg >= out.num_pages) {
          error = "read_export: page index out of range";
          return false;
        }
        const View p{db, pg * page};
        if (p.le32(4) != pg || p.le32(8) != type) {
          error = "read_export: page header mismatch";
          return false;
        }
        if ((p.u8(0x1B) & 0x40) == 0) {
          const std::uint32_t packed = p.u8(0x18) | (p.u8(0x19) << 8) | (p.u8(0x1A) << 16);
          const std::size_t noff = packed & 0x1FFF;
          for (std::size_t k = 0; k < noff; ++k) {
            const std::size_t g = k / 16, i = k % 16, bs = page - 36 * g;
            if (bs < 6 + 2 * i || ((p.le16(bs - 4) >> i) & 1) == 0) continue;
            const std::size_t ro = 0x28 + p.le16(bs - 6 - 2 * i);
            const View r{db, pg * page + ro};
            if (ro >= page) {
              error = "read_export: row offset outside page";
              return false;
            }
            ++out.table_rows[ti];
            std::string s;
            switch (type) {
              case 0: {
                RawTrack rt;
                ExportTrack& t = rt.rt.track;
                if (r.le16(0) != 0x24) {
                  error = "read_export: unexpected track subtype";
                  return false;
                }
                t.sample_rate = static_cast<int>(r.le32(8));
                t.key = {};
                rt.key = r.le32(0x20);
                rt.label = r.le32(0x28);
                t.bitrate = static_cast<int>(r.le32(0x30));
                t.track_number = static_cast<int>(r.le32(0x34));
                t.bpm = r.le32(0x38) / 100.0;
                rt.genre = r.le32(0x3C);
                rt.album = r.le32(0x40);
                rt.artist = r.le32(0x44);
                t.id = static_cast<int>(r.le32(0x48));
                t.year = static_cast<int>(r.le16(0x50));
                t.duration_ms = static_cast<std::int64_t>(r.le16(0x54)) * 1000;
                t.color_id = static_cast<int>(r.u8(0x58));
                t.rating = static_cast<int>(r.u8(0x59));
                rt.rt.file_type = r.le16(0x5A);
                auto str = [&](int idx) {
                  std::string o;
                  if (!dsql(r, r.le16(0x5E + 2 * idx), o)) o = "\x01<bad>";
                  return o;
                };
                rt.rt.analyze_path = str(14);
                t.comment = str(16);
                t.title = str(17);
                rt.rt.file_path = str(20);
                t.source_path = rt.rt.file_path;
                raw.push_back(std::move(rt));
                break;
              }
              case 1: case 4:
                if (dsql(r, 4, s)) names[type][r.le32(0)] = s;
                break;
              case 5:
                if (dsql(r, 8, s)) names[type][r.le32(0)] = s;
                break;
              case 2: {
                const bool far = (r.le16(0) & 4) != 0;
                if (dsql(r, far ? r.le16(0x0A) : r.u8(9), s)) names[type][r.le32(4)] = s;
                break;
              }
              case 3: {
                const bool far = (r.le16(0) & 4) != 0;
                if (dsql(r, far ? r.le16(0x16) : r.u8(0x15), s)) albums[r.le32(0x0C)] = {r.le32(8), s};
                break;
              }
              case 6:
                if (dsql(r, 8, s)) out.colors.push_back(s);
                break;
              case 7: {
                ExportPlaylist pl;
                pl.parent_id = static_cast<int>(r.le32(0));
                pl.id = static_cast<int>(r.le32(0x0C));
                pl.is_folder = r.le32(0x10) != 0;
                dsql(r, 0x14, pl.name);
                out.playlists.push_back(std::move(pl));
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
        if (pg == last) break;
        pg = p.le32(12);
      }
    }

    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.idx < b.idx; });
    for (const Entry& e : entries)
      for (auto& pl : out.playlists)
        if (static_cast<std::uint32_t>(pl.id) == e.list) pl.track_ids.push_back(static_cast<int>(e.track));

    for (RawTrack& r : raw) {
      ExportTrack& t = r.rt.track;
      t.artist = names[2][r.artist];
      t.genre = names[1][r.genre];
      t.key = names[5][r.key];
      if (r.album && albums.count(r.album)) t.album = albums[r.album].second;
      const std::string rel = (!r.rt.analyze_path.empty() && r.rt.analyze_path[0] == '/') ? r.rt.analyze_path.substr(1) : r.rt.analyze_path;
      const fs::path dat = root / rel;
      if (!parse_anlz(dat, r.rt, 0, error)) return false;
      fs::path ext = dat, ex2 = dat;
      ext.replace_extension(".EXT");
      ex2.replace_extension(".2EX");
      if (fs::exists(ext)) {
        r.rt.has_ext = true;
        if (!parse_anlz(ext, r.rt, 1, error)) return false;
      }
      if (fs::exists(ex2)) {
        r.rt.has_2ex = true;
        if (!parse_anlz(ex2, r.rt, 2, error)) return false;
      }
      out.tracks.push_back(std::move(r.rt));
    }
    return true;
  } catch (const std::exception& e) {
    error = std::string("read_export: ") + e.what();
  } catch (...) {
    error = "read_export: unknown failure";
  }
  return false;
}

}  // namespace deckhand::usbexport
