#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

#include "check.hpp"
#include "deckhand/export/export.hpp"
#include "deckhand/export/export_check.hpp"
#include "../src/export/rawio.hpp"

#include <chrono>
#include <cstdio>
#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace deckhand::usbexport;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  CHECK(f.good());
  return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
std::uint32_t le16(const Bytes& b, std::size_t o) { return b[o] | (b[o + 1] << 8); }
std::uint32_t le32(const Bytes& b, std::size_t o) { return le16(b, o) | (le16(b, o + 2) << 16); }
std::uint32_t be16(const Bytes& b, std::size_t o) { return (b[o] << 8) | b[o + 1]; }
std::uint32_t be32(const Bytes& b, std::size_t o) { return (be16(b, o) << 16) | be16(b, o + 2); }

ExportTrack make_track(int i, const fs::path& src_dir) {
  ExportTrack t;
  t.id = i;
  const fs::path p = src_dir / ("song" + std::to_string(i) + (i % 3 == 0 ? ".flac" : ".mp3"));
  std::ofstream(p, std::ios::binary) << "audio" << i;
  t.source_path = p.string();
  t.title = i % 7 == 0 ? "Caf\xC3\xA9 \xE6\x9D\xB1\xE4\xBA\xAC \xF0\x9F\x8E\xA7 #" + std::to_string(i) : "Track " + std::to_string(i);
  if (i % 11 == 0) t.title += std::string(150, 'x');  // forces the long ASCII encoding
  t.artist = "Artist " + std::to_string(i % 40) + (i % 5 == 0 ? " \xC3\x9C" : "");
  t.album = "Album " + std::to_string(i % 25);
  t.genre = "Genre " + std::to_string(i % 6);
  t.key = i % 2 ? "8A" : "Am";
  t.comment = i % 4 == 0 ? "comment \xE2\x82\xAC" : "";
  t.bpm = 100.0 + (i % 60) + 0.25;
  t.duration_ms = 180000 + i * 1000 + 500;
  t.sample_rate = 44100;
  t.bitrate = 320;
  t.year = 1990 + i % 30;
  t.rating = i % 6;
  t.track_number = i % 12;
  t.color_id = i % 9;
  for (int b = 0; b < 16; ++b) t.beats.push_back({std::int64_t(b) * 500 + 20, t.bpm, b % 4 + 1});
  ExportCue m;
  m.time_ms = 1000 + i;
  m.label = i % 2 ? "Drop \xE6\x9D\xB1" : "";
  m.color_id = 3;
  t.cues.push_back(m);
  ExportCue loop;
  loop.time_ms = 8000;
  loop.loop_end_ms = 12000;
  t.cues.push_back(loop);
  for (int s = 1; s <= 3; ++s) {
    ExportCue h;
    h.hot = true;
    h.slot = s;
    h.time_ms = 2000 * s;
    h.label = s == 2 ? "Hot" : "";
    h.has_rgb = true;
    h.r = 10 * s;
    h.g = 200;
    h.b = 7;
    t.cues.push_back(h);
  }
  const std::size_t n = std::size_t(t.duration_ms) * 150 / 1000;
  for (std::size_t k = 0; k < n; ++k)
    t.waveform.push_back({std::uint8_t((k * 3 + i) & 0xFF), std::uint8_t((k * 5) & 0xFF), std::uint8_t((k * 7) & 0xFF)});
  return t;
}

void check_structure(const fs::path& root) {
  const Bytes db = slurp(root / "PIONEER/rekordbox/export.pdb");
  CHECK(le32(db, 0) == 0);
  CHECK(le32(db, 4) == 4096);
  CHECK(db.size() % 4096 == 0);
  const std::uint32_t ntab = le32(db, 8), pages = db.size() / 4096;
  CHECK(ntab == 20);
  CHECK(le32(db, 12) == pages);
  std::set<std::uint32_t> seen_types;
  std::size_t multi_page_tables = 0;
  for (std::uint32_t t = 0; t < ntab; ++t) {
    const std::size_t tp = 0x1C + 16 * t;
    const std::uint32_t type = le32(db, tp), first = le32(db, tp + 8), last = le32(db, tp + 12);
    CHECK(seen_types.insert(type).second);
    CHECK(first < pages && last < pages && first <= last);
    // The index page comes first; data pages follow by next_page until last_page.
    CHECK((db[first * 4096 + 0x1B] & 0x40) != 0);
    std::uint32_t pg = first;
    std::size_t data_pages = 0;
    for (int hop = 0; hop < 100000; ++hop) {
      const std::size_t o = std::size_t(pg) * 4096;
      CHECK(le32(db, o) == 0);
      CHECK(le32(db, o + 4) == pg);
      CHECK(le32(db, o + 8) == type);
      const std::uint32_t flags = db[o + 0x1B];
      if ((flags & 0x40) == 0) {
        ++data_pages;
        const std::uint32_t packed = db[o + 0x18] | (db[o + 0x19] << 8) | (db[o + 0x1A] << 16);
        const std::uint32_t noff = packed & 0x1FFF, nrows = packed >> 13;
        CHECK(noff == nrows);
        const std::uint32_t used = le16(db, o + 0x1E);
        std::uint32_t present = 0;
        std::uint32_t max_end = 0;
        for (std::uint32_t k = 0; k < noff; ++k) {
          const std::size_t base = 4096 - 36 * (k / 16);
          const std::size_t ofs = le16(db, o + base - 6 - 2 * (k % 16));
          CHECK(0x28 + ofs < 4096 - (36 * (k / 16)));
          CHECK(ofs % 4 == 0);
          if ((le16(db, o + base - 4) >> (k % 16)) & 1) ++present;
          max_end = std::max<std::uint32_t>(max_end, ofs);
        }
        CHECK(present == nrows);
        CHECK(used <= 4096 - 0x28);
        // The heap and the row index must not overlap.
        const std::size_t index_size = noff ? 36 * ((noff - 1) / 16) + 4 + 2 * (noff - 16 * ((noff - 1) / 16)) : 0;
        CHECK(0x28 + used + index_size <= 4096);
      }
      if (pg == last) break;
      pg = le32(db, o + 12);
      CHECK(pg < pages);
    }
    if (data_pages > 1) ++multi_page_tables;
  }
  CHECK(multi_page_tables >= 2);  // tracks and playlist entries span several pages

  // ANLZ structure of the first track.
  fs::path anlz;
  for (const auto& e : fs::recursive_directory_iterator(root / "PIONEER/USBANLZ"))
    if (e.path().extension() == ".DAT") { anlz = e.path(); break; }
  CHECK(!anlz.empty());
  for (const char* ext : {".DAT", ".EXT", ".2EX"}) {
    fs::path p = anlz;
    p.replace_extension(ext);
    const Bytes a = slurp(p);
    CHECK(std::string(a.begin(), a.begin() + 4) == "PMAI");
    CHECK(be32(a, 4) == 0x1C);
    CHECK(be32(a, 8) == a.size());
    std::size_t pos = 0x1C;
    std::vector<std::string> tags;
    while (pos < a.size()) {
      CHECK(pos + 12 <= a.size());
      tags.emplace_back(a.begin() + pos, a.begin() + pos + 4);
      const std::uint32_t lh = be32(a, pos + 4), lt = be32(a, pos + 8);
      CHECK(lh >= 0x10 && lh <= lt && pos + lt <= a.size());
      if (tags.back() == "PWAV") { CHECK(lh == 0x14 && lt == 0x14 + 400); CHECK(be32(a, pos + 0x10) == 0x00100000); }
      if (tags.back() == "PWV2") CHECK(lt == 0x14 + 100);
      if (tags.back() == "PQTZ") {
        CHECK(lh == 0x18 && be32(a, pos + 0x14) == 16 && lt == 0x18 + 16 * 8);
        CHECK(be16(a, pos + 0x18) == 1);              // beat number
        CHECK(be32(a, pos + 0x1C) == 20);             // time in ms of beat 1 (bytes 4..7 of entry 0)
      }
      if (tags.back() == "PCOB") CHECK(lh == 0x18);
      if (tags.back() == "PWV4") CHECK(lt == 0x18 + 7200);
      if (tags.back() == "PWV6") CHECK(lt == 0x14 + 3600);
      pos += lt;
    }
    CHECK(pos == a.size());
    if (std::string(ext) == ".DAT") {
      const std::vector<std::string> want = {"PPTH", "PVBR", "PQTZ", "PWAV", "PWV2", "PCOB", "PCOB"};
      CHECK(tags == want);
    } else if (std::string(ext) == ".EXT") {
      CHECK(std::find(tags.begin(), tags.end(), "PCO2") != tags.end());
      CHECK(std::find(tags.begin(), tags.end(), "PWV3") != tags.end());
      CHECK(std::find(tags.begin(), tags.end(), "PWV5") != tags.end());
    }
  }
}


void expect_valid(const fs::path& root, bool check_audio = true) {
  ValidationReport rep;
  ValidateOptions vo;
  vo.check_audio = check_audio;
  const bool ok = validate_export(root.string(), rep, vo);
  if (!ok || !rep.ok()) std::fprintf(stderr, "%s", rep.text().c_str());
  CHECK(ok && rep.ok());
}

void write_bytes(const fs::path& p, const Bytes& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  CHECK(f.good());
}
void put16(Bytes& b, std::size_t o, std::uint32_t v) { b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF; }
void put32(Bytes& b, std::size_t o, std::uint32_t v) { put16(b, o, v & 0xFFFF); put16(b, o + 2, v >> 16); }

// Minimal independent locators for the corruption tests.
struct Pdb {
  Bytes b;
  std::uint32_t first(std::uint32_t type) const {
    for (std::uint32_t t = 0; t < le32(b, 8); ++t)
      if (le32(b, 0x1C + 16 * t) == type) return le32(b, 0x1C + 16 * t + 8);
    CHECK(false);
    return 0;
  }
  std::size_t ptr(std::uint32_t type) const {
    for (std::uint32_t t = 0; t < le32(b, 8); ++t)
      if (le32(b, 0x1C + 16 * t) == type) return 0x1C + 16 * t;
    CHECK(false);
    return 0;
  }
  // The n-th data page of a table (0 = first page after the index page).
  std::size_t data_page(std::uint32_t type, int n) const {
    std::uint32_t pg = first(type);
    for (int i = 0; i <= n; ++i) pg = le32(b, std::size_t(pg) * 4096 + 12);
    return std::size_t(pg) * 4096;
  }
};

std::size_t find_bytes(const Bytes& b, const std::string& pat, std::size_t from = 0) {
  const Bytes pb(pat.begin(), pat.end());
  const auto it = std::search(b.begin() + from, b.end(), pb.begin(), pb.end());
  return it == b.end() ? std::string::npos : std::size_t(it - b.begin());
}

void copy_tree(const fs::path& from, const fs::path& to) {
  fs::remove_all(to);
  fs::copy(from, to, fs::copy_options::recursive);
}

fs::path first_anlz(const fs::path& root, const char* ext) {
  std::set<fs::path> v;
  for (const auto& e : fs::recursive_directory_iterator(root / "PIONEER/USBANLZ"))
    if (e.path().extension() == ext) v.insert(e.path());
  CHECK(!v.empty());
  return *v.begin();
}

// Finds a tag in an ANLZ file (big-endian envelope) and returns its offset.
std::size_t anlz_tag(const Bytes& a, const char* cc) {
  std::size_t pos = be32(a, 4);
  while (pos + 12 <= a.size()) {
    if (std::string(a.begin() + pos, a.begin() + pos + 4) == cc) return pos;
    pos += be32(a, pos + 8);
  }
  return std::string::npos;
}

void negative_tests(const fs::path& good, const fs::path& work) {
  expect_valid(good);
  struct Case {
    const char* code;
    std::function<void(const fs::path&)> mutate;
  };
  auto pdb_edit = [](const std::function<void(Pdb&)>& fn) {
    return [fn](const fs::path& r) {
      Pdb p;
      p.b = slurp(r / "PIONEER/rekordbox/export.pdb");
      fn(p);
      write_bytes(r / "PIONEER/rekordbox/export.pdb", p.b);
    };
  };
  auto anlz_edit = [](const char* ext, const std::function<void(Bytes&)>& fn) {
    return [=](const fs::path& r) {
      const fs::path f = first_anlz(r, ext);
      Bytes a = slurp(f);
      fn(a);
      write_bytes(f, a);
    };
  };
  const std::vector<Case> cases = {
      {"pdb.chain_shared", pdb_edit([](Pdb& p) { const std::size_t d = p.data_page(0, 0); put32(p.b, d + 12, std::uint32_t(d / 4096)); })},
      {"pdb.next_range", pdb_edit([](Pdb& p) { put32(p.b, p.data_page(0, 0) + 12, 0xFFFF); })},
      {"pdb.table_range", pdb_edit([](Pdb& p) { put32(p.b, p.ptr(1) + 12, 99999); })},
      {"pdb.row_bounds", pdb_edit([](Pdb& p) { put16(p.b, p.data_page(0, 0) + 4096 - 6, 0x0FF0); })},
      {"pdb.row_overlap", pdb_edit([](Pdb& p) { const std::size_t d = p.data_page(0, 0); put16(p.b, d + 4096 - 8, le16(p.b, d + 4096 - 6)); })},
      {"pdb.row_count", pdb_edit([](Pdb& p) { const std::size_t d = p.data_page(0, 0); put16(p.b, d + 4096 - 4, le16(p.b, d + 4096 - 4) & ~2u); })},
      {"pdb.presence_mask", pdb_edit([](Pdb& p) {
         const std::size_t d = p.data_page(1, 0);  // genres: a handful of rows
         put16(p.b, d + 4096 - 4, le16(p.b, d + 4096 - 4) | 0x8000u);
       })},
      {"pdb.free_size", pdb_edit([](Pdb& p) { const std::size_t d = p.data_page(0, 0); put16(p.b, d + 0x1C, le16(p.b, d + 0x1C) + 4); })},
      {"pdb.page_type", pdb_edit([](Pdb& p) { put32(p.b, p.data_page(0, 1) + 8, 5); })},
      {"pdb.page_seq", pdb_edit([](Pdb& p) { put32(p.b, p.data_page(0, 0) + 16, 0x7FFFFFFF); })},
      {"fk.track_artist", pdb_edit([](Pdb& p) { put32(p.b, p.data_page(0, 0) + 0x28 + 0x44, 9999); })},
      {"fk.track_genre", pdb_edit([](Pdb& p) { put32(p.b, p.data_page(0, 0) + 0x28 + 0x3C, 9999); })},
      {"dup.id", pdb_edit([](Pdb& p) {
         const std::size_t d = p.data_page(0, 0);
         const std::size_t r1 = d + 0x28 + le16(p.b, d + 4096 - 8);
         put32(p.b, r1 + 0x48, le32(p.b, d + 0x28 + 0x48));
       })},
      {"fk.entry_track", pdb_edit([](Pdb& p) { put32(p.b, p.data_page(8, 0) + 0x28 + 4, 424242); })},
      {"fk.playlist_parent", pdb_edit([](Pdb& p) { put32(p.b, p.data_page(7, 0) + 0x28 + 0, 77); })},
      {"anlz.missing_DAT", [](const fs::path& r) { fs::remove(first_anlz(r, ".DAT")); }},
      {"anlz.missing_EXT", [](const fs::path& r) { fs::remove(first_anlz(r, ".EXT")); }},
      {"anlz.structure", anlz_edit(".EXT", [](Bytes& a) { a.resize(a.size() - 3); })},
      {"anlz.ppth", anlz_edit(".DAT", [](Bytes& a) { const std::size_t t = anlz_tag(a, "PPTH"); a[t + 0x10 + 3] ^= 1; })},
      {"anlz.beatgrid_order", anlz_edit(".DAT", [](Bytes& a) { const std::size_t t = anlz_tag(a, "PQTZ"); for (int k = 0; k < 4; ++k) a[t + 0x18 + 8 * 3 + 4 + k] = 0; })},
      {"anlz.beatgrid_bar", anlz_edit(".DAT", [](Bytes& a) { const std::size_t t = anlz_tag(a, "PQTZ"); a[t + 0x18 + 8 * 2 + 1] = 4; })},
      {"anlz.cue_range", anlz_edit(".EXT", [](Bytes& a) { const std::size_t t = anlz_tag(a, "PCO2"); a[t + 0x14 + 0x14] = 0x7F; })},
      {"anlz.hot_slot", anlz_edit(".DAT", [](Bytes& a) {
         const std::size_t t = anlz_tag(a, "PCOB");  // memory list: claim the first entry is hot cue 12
         a[t + 0x18 + 0x0F] = 12;
       })},
      {"anlz.waveform_size", anlz_edit(".DAT", [](Bytes& a) { const std::size_t t = anlz_tag(a, "PWAV"); a[t + 0x0F] = 1; })},
      {"audio.missing", [](const fs::path& r) {
         for (const auto& e : fs::recursive_directory_iterator(r / "Contents"))
           if (e.is_regular_file()) { fs::remove(e.path()); return; }
       }},
      {"audio.size", [](const fs::path& r) {
         for (const auto& e : fs::recursive_directory_iterator(r / "Contents"))
           if (e.is_regular_file()) { std::ofstream(e.path(), std::ios::binary | std::ios::app) << "extra"; return; }
       }},
      {"fat.path", pdb_edit([](Pdb& p) {
         const std::size_t at = find_bytes(p.b, "/Contents/Artist");
         CHECK(at != std::string::npos);
         p.b[at + 9] = ':';  // joins "Contents" and the artist folder with a forbidden character, same length
       })},
      {"fat.case_collision", pdb_edit([](Pdb& p) {
         const std::size_t a = find_bytes(p.b, "/Contents/Artist 1/");
         CHECK(a != std::string::npos);
         const std::size_t b = find_bytes(p.b, "/Contents/Artist 1/", a + 1);
         CHECK(b != std::string::npos);
         p.b[b + 10] = 'a';  // "artist 1" vs "Artist 1" for the same folder
       })},
      {"export.leftover", [](const fs::path& r) { std::ofstream(r / "PIONEER/rekordbox/export.pdb.dhtmp") << "x"; }},
  };
  for (const Case& c : cases) {
    copy_tree(good, work);
    c.mutate(work);
    ValidationReport rep;
    const bool ok = validate_export(work.string(), rep);
    if (ok || !rep.has(c.code)) std::fprintf(stderr, "negative case %s not flagged:\n%s", c.code, rep.text().c_str());
    CHECK(!ok && !rep.ok() && rep.has(c.code));
  }
  copy_tree(good, work);
  expect_valid(work);  // the copy itself is fine
}

}  // namespace

int main() {
  const fs::path tmp = fs::temp_directory_path() / "deckhand_export_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "src");
  const fs::path stick = tmp / "stick";

  std::vector<ExportTrack> tracks;
  for (int i = 1; i <= 300; ++i) tracks.push_back(make_track(i * 2, tmp / "src"));  // ids not contiguous
  // Same artist/album/file name twice: the writer must disambiguate the destination path.
  tracks[1].artist = tracks[0].artist;
  tracks[1].album = tracks[0].album;
  fs::copy_file(tracks[0].source_path, tmp / "src" / "dup.mp3");
  tracks[0].source_path = (tmp / "src" / "dup.mp3").string();
  fs::create_directories(tmp / "src2");
  fs::copy_file(tmp / "src" / "dup.mp3", tmp / "src2" / "dup.mp3");
  tracks[1].source_path = (tmp / "src2" / "dup.mp3").string();

  std::vector<ExportPlaylist> lists;
  lists.push_back({1, "Folder \xE6\x9D\xB1", 0, true, {}});
  lists.push_back({2, "Sub folder", 1, true, {}});
  ExportPlaylist all{3, "Everything", 2, false, {}};
  for (const auto& t : tracks) all.track_ids.push_back(t.id);
  lists.push_back(all);
  lists.push_back({4, "Short", 0, false, {600, 2, 100}});
  lists.push_back({5, "Empty", 1, false, {}});

  ExportOptions opt;
  opt.destination = stick.string();
  std::string err;
  CHECK(export_usb(tracks, lists, opt, err));
  CHECK(err.empty());

  check_structure(stick);
  expect_valid(stick);

  ReadExport rx;
  CHECK(read_export(stick.string(), rx, err));
  CHECK(rx.page_size == 4096);
  CHECK(rx.tracks.size() == 300);
  CHECK(rx.table_rows[0] == 300);
  {
    std::set<std::string> g, a, k;
    std::set<std::pair<std::string, std::string>> al;
    for (const auto& t : tracks) {
      g.insert(t.genre);
      a.insert(t.artist);
      k.insert(t.key);
      al.insert({t.artist, t.album});
    }
    CHECK(rx.table_rows[1] == g.size());
    CHECK(rx.table_rows[2] == a.size());
    CHECK(rx.table_rows[3] == al.size());
    CHECK(rx.table_rows[5] == k.size());
  }
  CHECK(rx.table_rows[6] == 8);
  CHECK(rx.table_rows[8] == 300 + 3);  // playlist entries
  CHECK(rx.colors.size() == 8 && rx.colors[0] == "Pink");

  std::map<int, const ReadTrack*> by_id;
  for (const auto& r : rx.tracks) by_id[r.track.id] = &r;
  CHECK(by_id.size() == 300);
  std::set<std::string> paths;
  for (const auto& in : tracks) {
    const ReadTrack& r = *by_id.at(in.id);
    const ExportTrack& o = r.track;
    CHECK(o.title == in.title);
    CHECK(o.artist == in.artist);
    CHECK(o.album == in.album);
    CHECK(o.genre == in.genre);
    CHECK(o.key == in.key);
    CHECK(o.comment == in.comment);
    CHECK(o.bpm == in.bpm);
    CHECK(o.duration_ms == in.duration_ms / 1000 * 1000);
    CHECK(o.year == in.year && o.rating == in.rating && o.bitrate == in.bitrate);
    CHECK(o.sample_rate == in.sample_rate && o.track_number == in.track_number && o.color_id == in.color_id);
    CHECK(r.file_type == (in.id % 3 == 0 ? 5u : 1u));
    CHECK(r.file_path.rfind("/Contents/", 0) == 0);
    CHECK(paths.insert(r.file_path).second);  // unique
    CHECK(fs::exists(stick / r.file_path.substr(1)));
    CHECK(r.ppth_dat == r.file_path && r.ppth_ext == r.file_path);
    CHECK(r.analyze_path.rfind("/PIONEER/USBANLZ/P", 0) == 0);
    CHECK(r.has_ext && r.has_2ex);

    // Beat grid (DAT PQTZ).
    CHECK(o.beats.size() == in.beats.size());
    for (std::size_t i = 0; i < in.beats.size(); ++i) {
      CHECK(o.beats[i].time_ms == in.beats[i].time_ms);
      CHECK(o.beats[i].beat_in_bar == in.beats[i].beat_in_bar);
      CHECK(o.beats[i].bpm == in.bpm);
    }
    // Cues: PCOB (DAT) and PCO2 (EXT) must agree with the input.
    CHECK(r.dat_cues.size() == in.cues.size());
    CHECK(o.cues.size() == in.cues.size());
    for (const ExportCue& c : in.cues) {
      auto match = [&](const std::vector<ExportCue>& v) -> const ExportCue* {
        for (const auto& x : v)
          if (x.hot == c.hot && x.slot == c.slot && x.time_ms == c.time_ms) return &x;
        return nullptr;
      };
      const ExportCue* a = match(r.dat_cues);
      const ExportCue* b = match(o.cues);
      CHECK(a && b);
      CHECK(a->loop_end_ms == c.loop_end_ms && b->loop_end_ms == c.loop_end_ms);
      CHECK(b->label == c.label);
      if (!c.hot) CHECK(b->color_id == c.color_id);
      if (c.hot) CHECK(b->has_rgb && b->r == c.r && b->g == c.g && b->b == c.b);
    }
    // Waveforms.
    CHECK(r.pwav.size() == 400 && r.pwv2.size() == 100);
    CHECK(r.pwv3.size() == in.waveform.size());
    CHECK(r.pwv5.size() == in.waveform.size());
    CHECK(r.pwv4_columns == 1200 && r.pwv6_columns == 1200 && r.pwv7_entries == in.waveform.size());
    for (std::size_t k = 0; k < in.waveform.size(); k += 97) {
      const auto& s = in.waveform[k];
      const unsigned pk = std::max({s.low, s.mid, s.high});
      CHECK((r.pwv3[k] & 0x1F) == (pk * 31 + 127) / 255);
      CHECK(((r.pwv5[k] >> 2) & 0x1F) == (pk * 31 + 127) / 255);
    }
    CHECK(r.pvbr_entries == 400);
  }
  // Duplicate destination names are disambiguated.
  CHECK(by_id.at(tracks[0].id)->file_path != by_id.at(tracks[1].id)->file_path);

  // Playlist tree and ordering.
  CHECK(rx.playlists.size() == lists.size());
  std::map<int, const ExportPlaylist*> pl;
  for (const auto& p : rx.playlists) pl[p.id] = &p;
  CHECK(pl.at(1)->is_folder && pl.at(1)->parent_id == 0 && pl.at(1)->name == lists[0].name);
  CHECK(pl.at(2)->is_folder && pl.at(2)->parent_id == 1);
  CHECK(!pl.at(3)->is_folder && pl.at(3)->parent_id == 2 && pl.at(3)->track_ids == all.track_ids);
  CHECK(pl.at(4)->track_ids == (std::vector<int>{600, 2, 100}));
  CHECK(pl.at(5)->parent_id == 1 && pl.at(5)->track_ids.empty());

  // String encodings: short ASCII, long ASCII (>126), UTF-16LE.
  {
    const Bytes db = slurp(stick / "PIONEER/rekordbox/export.pdb");
    bool short_ascii = false, long_ascii = false, utf16 = false;
    for (std::size_t i = 0; i + 4 < db.size(); ++i) {
      if (db[i] == 0x40 && le16(db, i + 1) > 130 && db[i + 3] == 0 && db[i + 4] == 'T' && db[i + 5] == 'r') long_ascii = true;
      if (db[i] == 0x90 && db[i + 3] == 0 && db[i + 4] == 'C' && db[i + 5] == 0 && db[i + 6] == 'a') utf16 = true;
      if (db[i] == ((7u << 1) | 1) && db[i + 1] == 'T' && db[i + 2] == 'r') short_ascii = true;  // "Track 1" is not 6 chars
    }
    (void)short_ascii;
    CHECK(long_ascii);
    CHECK(utf16);
    // "Am" key as a short ASCII string: length byte (3 << 1) | 1.
    bool key = false;
    for (std::size_t i = 0; i + 3 < db.size(); ++i)
      if (db[i] == 0x07 && db[i + 1] == 'A' && db[i + 2] == 'm') key = true;
    CHECK(key);
  }

  // No audio copy: DB keeps the on-stick relative path when the source already lives on the stick.
  {
    const fs::path stick2 = tmp / "stick2";
    fs::create_directories(stick2 / "Music");
    std::ofstream(stick2 / "Music/a.mp3") << "x";
    ExportTrack t;
    t.id = 5;
    t.source_path = (stick2 / "Music/a.mp3").string();
    t.title = "On stick";
    ExportOptions o2;
    o2.destination = stick2.string();
    o2.copy_audio = false;
    o2.write_3band = false;
    o2.write_color_preview = false;
    CHECK(export_usb({t}, {}, o2, err));
    expect_valid(stick2);
    ReadExport r2;
    CHECK(read_export(stick2.string(), r2, err));
    CHECK(r2.tracks.size() == 1 && r2.tracks[0].file_path == "/Music/a.mp3");
    CHECK(!r2.tracks[0].has_2ex && r2.tracks[0].pwv4_columns == 0);
    CHECK(r2.tracks[0].pwv3.size() == 0);  // no duration, no waveform
  }

  // Errors are reported, not thrown.
  {
    ExportTrack a, b;
    a.id = b.id = 1;
    ExportOptions o3;
    o3.destination = (tmp / "bad").string();
    CHECK(!export_usb({a, b}, {}, o3, err) && !err.empty());
    CHECK(!export_usb({a}, {ExportPlaylist{9, "x", 77, false, {}}}, o3, err));
    CHECK(!export_usb({a}, {ExportPlaylist{9, "x", 0, false, {123}}}, o3, err));
    ReadExport rx3;
    CHECK(!read_export((tmp / "nothing").string(), rx3, err) && !err.empty());
  }

  // ---- validator: negative cases on a small export with several tables spanning pages -------------------------
  {
    const fs::path nsrc = tmp / "nsrc";
    fs::create_directories(nsrc);
    std::vector<ExportTrack> nt;
    for (int i = 1; i <= 60; ++i) {
      ExportTrack t = make_track(i, nsrc);
      t.artist = "Artist " + std::to_string(i % 20);
      t.waveform.resize(std::min<std::size_t>(t.waveform.size(), 3000));
      t.duration_ms = 20000;
      t.waveform.resize(3000);
      nt.push_back(std::move(t));
    }
    std::vector<ExportPlaylist> nl = {{1, "Folder", 0, true, {}}, {2, "All", 1, false, {}}};
    for (const auto& t : nt) nl[1].track_ids.push_back(t.id);
    nl.push_back({3, "Some", 0, false, {5, 3, 1}});
    ExportOptions no;
    no.destination = (tmp / "neg_good").string();
    CHECK(export_usb(nt, nl, no, err));
    negative_tests(tmp / "neg_good", tmp / "neg_work");
  }

  // ---- index page layout, ISRC / UTF-8 strings, PWV4 entry layout -----------------------------------------------
  {
    const Bytes db = slurp(stick / "PIONEER/rekordbox/export.pdb");
    Pdb p;
    p.b = db;
    const std::size_t ip = std::size_t(p.first(1)) * 4096;  // an index page
    CHECK(db[ip + 0x1B] == 0x64);
    CHECK(le16(db, ip + 0x24) == 0x03EC);  // magic follows two 0x1FFF words at 0x20 and 0x22
    CHECK(le16(db, ip + 0x20) == 0x1FFF && le16(db, ip + 0x22) == 0x1FFF);
    CHECK(le32(db, ip + 0x28) == p.first(1));  // page_index copy
    CHECK(le32(db, ip + 0x30) == 0x03FFFFFF && le32(db, ip + 0x34) == 0);
    CHECK(le16(db, ip + 0x38) == 0 && le16(db, ip + 0x3A) == 0x1FFF);  // num_entries, first_empty
    CHECK(le32(db, ip + 0x3C) == 0x1FFFFFF8);
    // 13/11 bit packing: a page with 3 rows has offsets=3 in the low 13 bits and rows=3 above them.
    const std::size_t dp = p.data_page(1, 0);
    const std::uint32_t packed = db[dp + 0x18] | (db[dp + 0x19] << 8) | (db[dp + 0x1A] << 16);
    CHECK((packed & 0x1FFF) == (packed >> 13) && (packed & 0x1FFF) > 0 && (packed & 0x1FFF) < 16);

    using namespace deckhand::usbexport::detail;
    Bytes s;
    s.push_back(0x90); s.push_back(9); s.push_back(0); s.push_back(0);   // long, length 9 = 4 + 5
    for (int ch : {0x03, int('U'), int('S'), int('X'), 0x00}) s.push_back(static_cast<std::uint8_t>(ch));  // ISRC body: 03, ASCII, NUL
    const View sv{s, 0};
    const DsqlStr isrc = dsql_read(sv, 0, true);
    CHECK(isrc.kind == StrKind::Isrc && isrc.text == "USX" && isrc.extent == 9);
    const DsqlStr asu = dsql_read(sv, 0, false);
    CHECK(asu.kind == StrKind::Utf16);  // outside the ISRC slot the same bytes are UTF-16
    Bytes u8{0x20, 8, 0, 0, 'a', 0xC3, 0xA9, 'b'};
    const DsqlStr n8 = dsql_read(View{u8, 0}, 0);
    CHECK(n8.kind == StrKind::Utf8 && n8.text == "a\xC3\xA9" "b");
    Bytes bad{0x40, 3, 0, 0};
    CHECK(!dsql_read(View{bad, 0}, 0).ok());

    // PWV4: bytes 3, 4, 5 are the low-third, mid and high energy; byte 2 the lower half.
    const fs::path src = tmp / "pwv4src";
    fs::create_directories(src);
    ExportTrack t = make_track(1, src);
    t.waveform.assign(3000, WaveSample{100, 50, 20});
    t.duration_ms = 20000;
    ExportOptions o;
    o.destination = (tmp / "pwv4").string();
    CHECK(export_usb({t}, {}, o, err));
    const Bytes ext = slurp(first_anlz(tmp / "pwv4", ".EXT"));
    const std::size_t t4 = anlz_tag(ext, "PWV4");
    CHECK(t4 != std::string::npos);
    CHECK(ext[t4 + 0x18 + 3] == 100 && ext[t4 + 0x18 + 4] == 50 && ext[t4 + 0x18 + 5] == 20);
    CHECK(ext[t4 + 0x18 + 2] == (2 * 100 + 50 + 1) / 3);
    expect_valid(tmp / "pwv4");
  }

  // ---- exportExt.pdb (experimental) -----------------------------------------------------------------------------
  {
    const fs::path src = tmp / "extsrc";
    fs::create_directories(src);
    std::vector<ExportTrack> et;
    for (int i = 1; i <= 5; ++i) et.push_back(make_track(i, src));
    et[0].tags = {"Peak time", "Vocal \xE6\x9D\xB1"};
    et[2].tags = {"Peak time"};
    et[4].tags = {std::string(300, 't')};  // forces the far-name row variant
    ExportOptions o;
    o.destination = (tmp / "ext").string();
    CHECK(export_usb(et, {}, o, err));
    CHECK(!fs::exists(tmp / "ext/PIONEER/rekordbox/exportExt.pdb"));  // default off
    o.write_ext_db = true;
    o.destination = (tmp / "ext2").string();
    CHECK(export_usb(et, {}, o, err));
    expect_valid(tmp / "ext2");
    ReadExport r;
    CHECK(read_export((tmp / "ext2").string(), r, err));
    CHECK(r.has_ext_db && r.ext_table_rows.size() == 2);
    CHECK(r.ext_table_rows[0] == 1 + 3 && r.ext_table_rows[1] == 4);  // category + 3 tags, 4 links
    std::map<std::string, ReadTag> by_name;
    for (const auto& tag : r.tags) by_name[tag.name] = tag;
    CHECK(by_name.size() == 4 && by_name.at("DeckHand").is_category && by_name.at("DeckHand").id == 1);
    CHECK(!by_name.at("Peak time").is_category && by_name.at("Peak time").category == 1);
    CHECK(by_name.count("Vocal \xE6\x9D\xB1") && by_name.count(std::string(254, 't')));  // capped at 254 bytes like every DB string
    std::set<std::pair<std::uint32_t, std::uint32_t>> links(r.tag_links.begin(), r.tag_links.end());
    CHECK(links.size() == 4 && links.count({1, by_name.at("Peak time").id}) && links.count({3, by_name.at("Peak time").id}));
    // Far-name tag row: subtype 0x0684.
    const Bytes edb = slurp(tmp / "ext2/PIONEER/rekordbox/exportExt.pdb");
    CHECK(le32(edb, 8) == 2 && le32(edb, 0x1C) == 3 && le32(edb, 0x2C) == 4);
    CHECK(find_bytes(edb, std::string("\x84\x06", 2)) != std::string::npos);
    // A corrupted link is caught.
    Bytes bad = edb;
    Pdb ep;
    ep.b = edb;
    put32(bad, ep.data_page(4, 0) + 0x28 + 4, 999);
    write_bytes(tmp / "ext2/PIONEER/rekordbox/exportExt.pdb", bad);
    ValidationReport rep;
    CHECK(!validate_export((tmp / "ext2").string(), rep) && rep.has("fk.tagtrack_track"));
  }

  // ---- reference comparison harness -------------------------------------------------------------------------------
  {
    const fs::path src = tmp / "cmpsrc";
    fs::create_directories(src);
    std::vector<ExportTrack> a;
    for (int i = 1; i <= 6; ++i) {
      ExportTrack t = make_track(i, src);
      t.waveform.resize(1500);
      t.duration_ms = 10000;
      t.cues.erase(std::remove_if(t.cues.begin(), t.cues.end(), [](const ExportCue& c) { return c.time_ms > 9000; }), t.cues.end());
      a.push_back(std::move(t));
    }
    std::vector<ExportPlaylist> pl = {{1, "Set", 0, false, {1, 2, 3}}};
    ExportOptions oa;
    oa.destination = (tmp / "cmp_a").string();
    CHECK(export_usb(a, pl, oa, err));
    CompareReport same;
    CHECK(compare_exports(oa.destination, oa.destination, same, err));
    CHECK(same.identical() && same.matched_tracks == 6);
    CHECK(!same.histograms.empty());

    std::vector<ExportTrack> b = a;
    b[1].comment = "changed";
    b[2].rating = (b[2].rating + 1) % 6;
    b[3].cues.pop_back();       // one hot cue fewer: PCO2/PCOB differ
    b[4].title = "Renamed";     // matched by file name, title differs
    ExportOptions ob = oa;
    ob.destination = (tmp / "cmp_b").string();
    ob.write_color_preview = false;  // PWV4 only in the reference
    pl.push_back({2, "Extra", 0, false, {4}});
    CHECK(export_usb(b, pl, ob, err));
    CompareReport diff;
    CHECK(compare_exports(oa.destination, ob.destination, diff, err));
    CHECK(!diff.identical() && diff.matched_tracks == 6);
    auto has_field = [&](const std::string& f, const std::string& ref, const std::string& ours) {
      for (const auto& d : diff.fields)
        if (d.field == f && d.reference == ref && d.ours == ours) return true;
      return false;
    };
    CHECK(has_field("s_comment", a[1].comment.empty() ? "a:" : "a:" + a[1].comment, "a:changed"));
    CHECK(has_field("rating", std::to_string(a[2].rating), std::to_string(b[2].rating)));
    CHECK(has_field("s_title", "a:" + a[4].title, "a:Renamed"));
    bool pwv4 = false, cue_diff = false, tbl_diff = false;
    for (const auto& l : diff.anlz) {
      if (l.find("PWV4 only in the reference") != std::string::npos) pwv4 = true;
      if (l.find("PCO") != std::string::npos) cue_diff = true;
    }
    for (const auto& l : diff.structure)
      if (l.find("table 0x7") != std::string::npos || l.find("table 0x8") != std::string::npos) tbl_diff = true;
    CHECK(pwv4 && cue_diff && tbl_diff);
    CHECK(diff.text().find("field differences") != std::string::npos);
    bool bitmask_hist = false;
    for (const auto& h : diff.histograms)
      if (h.field == "track.bitmask" && h.reference.count("0x000C0700") && h.ours == "0x000C0700") bitmask_hist = true;
    CHECK(bitmask_hist);
    CompareReport none;
    CHECK(!compare_exports((tmp / "nothing").string(), oa.destination, none, err) && !err.empty());
  }

  // ---- names, collisions, deterministic sanitising ------------------------------------------------------------------
  {
    CHECK(sanitize_fs_name("CON", 255, "x") == "_CON");
    CHECK(sanitize_fs_name("con.txt", 255, "x") == "_con.txt");
    CHECK(sanitize_fs_name("Com1.mp3", 255, "x") == "_Com1.mp3");
    CHECK(sanitize_fs_name("a:b*c?\"d<e>f|g/h\\i.mp3", 255, "x") == "a_b_c__d_e_f_g_h_i.mp3");
    CHECK(sanitize_fs_name("trail. . ", 255, "x") == "trail");
    CHECK(sanitize_fs_name("...", 255, "fallback") == "fallback");
    CHECK(sanitize_fs_name("\xF0\x9F\x8E\xA7 mix.flac", 255, "x") == "_ mix.flac");
    CHECK(sanitize_fs_name("caf\xC3\xA9", 255, "x") == "caf\xC3\xA9");
    CHECK(sanitize_fs_name("bad\xFF" "byte", 255, "x") == "bad_byte");
    const std::string longname = std::string(400, 'a') + ".flac";
    const std::string cut = sanitize_fs_name(longname, 255, "x");
    CHECK(cut.size() <= 255 && cut.size() > 200 && cut.substr(cut.size() - 5) == ".flac");
    const std::string longu = std::string(200, '\xC3') + std::string(200, '\xA9');
    (void)longu;
    std::string multi;
    for (int i = 0; i < 200; ++i) multi += "\xE6\x9D\xB1";
    const std::string mc = sanitize_fs_name(multi + ".mp3", 255, "x");
    CHECK(mc.size() <= 255 && mc.size() % 3 == 4 % 3 && mc.substr(mc.size() - 4) == ".mp3");
    CHECK(sanitize_fs_name(longname, 255, "x") == cut);  // deterministic
    CHECK(fat_path_problems("/Contents/Artist/Album/song.mp3").empty());
    CHECK(!fat_path_problems("/Contents/AC:DC/x.mp3").empty());
    CHECK(!fat_path_problems("/Contents/nul/x.mp3").empty());
    CHECK(!fat_path_problems("/Contents/x./y.mp3").empty());
    CHECK(!fat_path_problems("/Contents/" + std::string(256, 'a') + "/y.mp3").empty());
    CHECK(!fat_path_problems("/Contents//y.mp3").empty());

    // End to end: awkward artist and album names, case variants, nothing violates FAT rules, collisions resolve.
    const fs::path src = tmp / "namesrc";
    fs::create_directories(src);
    const std::vector<std::pair<std::string, std::string>> names = {
        {"AC/DC", "Back:In*Black"}, {"ac/dc", "back:in*black"}, {"CON", "AUX"}, {"nul.", "lpt1"},
        {std::string(300, 'x'), std::string(300, 'y')}, {"\xF0\x9F\x8E\xA7 DJ", "\xF0\x9F\x8E\xA7"}, {"Mr. ", " lead "},
        {"", ""}, {"ABBA", "Gold"}, {"abba", "GOLD"}, {"Abba", "gold"}};
    std::vector<ExportTrack> nt;
    for (std::size_t i = 0; i < names.size(); ++i) {
      ExportTrack t = make_track(static_cast<int>(i) + 1, src);
      t.artist = names[i].first;
      t.album = names[i].second;
      fs::create_directories(src / std::to_string(i));
      std::ofstream(src / std::to_string(i) / "same.mp3") << "audio" << i;  // identical file names, different files
      t.source_path = (src / std::to_string(i) / "same.mp3").string();
      t.duration_ms = 20000;
      t.waveform.resize(3000);
      nt.push_back(std::move(t));
    }
    ExportOptions o;
    o.destination = (tmp / "names").string();
    CHECK(export_usb(nt, {}, o, err));
    expect_valid(tmp / "names");
    ReadExport r;
    CHECK(read_export(o.destination, r, err));
    std::set<std::string> lowered;
    std::map<std::string, std::set<std::string>> dir_spell;
    for (const auto& rt : r.tracks) {
      std::string low = rt.file_path;
      for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      CHECK(rt.file_path.size() <= 400);
      // Same file exported by several tracks is shared; different artists give different folders.
      lowered.insert(low);
    }
    CHECK(lowered.size() >= 8);
    // Same artist/album with different case maps to one folder spelling.
    std::string abba;
    for (const auto& rt : r.tracks)
      if (rt.track.artist == "ABBA" || rt.track.artist == "abba" || rt.track.artist == "Abba") {
        const std::string dir = rt.file_path.substr(0, rt.file_path.find('/', 10));
        if (abba.empty()) abba = dir;
        CHECK(dir == abba);
      }
    // Deterministic: a second export of the same input gives byte-identical database paths.
    ExportOptions o2 = o;
    o2.destination = (tmp / "names2").string();
    CHECK(export_usb(nt, {}, o2, err));
    ReadExport r2;
    CHECK(read_export(o2.destination, r2, err));
    CHECK(r2.tracks.size() == r.tracks.size());
    for (std::size_t i = 0; i < r.tracks.size(); ++i) CHECK(r.tracks[i].file_path == r2.tracks[i].file_path);
  }

  // ---- failures leave nothing behind, pre-existing audio is never touched ---------------------------------------------
  {
    auto listing = [](const fs::path& root) {
      std::map<std::string, std::uintmax_t> m;
      std::error_code ec;
      if (!fs::exists(root, ec)) return m;
      for (const auto& e : fs::recursive_directory_iterator(root)) m[e.path().lexically_relative(root).generic_string()] = e.is_directory() ? 0 : fs::file_size(e.path());
      return m;
    };
    const fs::path src = tmp / "failsrc";
    fs::create_directories(src);
    std::vector<ExportTrack> ft;
    for (int i = 1; i <= 8; ++i) {
      ExportTrack t = make_track(i, src);
      t.waveform.resize(3000);
      t.duration_ms = 20000;
      ft.push_back(std::move(t));
    }
    // 1. nearly full
    {
      ExportOptions o;
      o.destination = (tmp / "full/stick").string();
      o.available_bytes_override = 5000;
      CHECK(!export_usb(ft, {}, o, err) && err.find("free space") != std::string::npos);
      CHECK(!fs::exists(tmp / "full"));
    }
    // 2. unwritable: the destination lies below a regular file
    {
      std::ofstream(tmp / "afile") << "x";
      ExportOptions o;
      o.destination = (tmp / "afile/stick").string();
      CHECK(!export_usb(ft, {}, o, err) && !err.empty());
      CHECK(fs::is_regular_file(tmp / "afile") && fs::file_size(tmp / "afile") == 1);
    }
    // 3. failure in the commit phase on a stick that already holds an export and audio
    {
      const fs::path st = tmp / "failstick";
      ExportOptions o;
      o.destination = st.string();
      CHECK(export_usb(ft, {{1, "Old", 0, false, {1, 2}}}, o, err));
      // A pre-existing file that collides with a new destination name but has other content.
      const std::string artist = ft[0].artist, album = ft[0].album;
      ReadExport before;
      CHECK(read_export(st.string(), before, err));
      const fs::path old_audio = st / before.tracks[0].file_path.substr(1);
      std::ofstream(old_audio, std::ios::binary | std::ios::trunc) << "precious original audio, different size";
      const std::string precious_text = "precious original audio, different size";
      const Bytes precious(precious_text.begin(), precious_text.end());
      const auto snapshot = listing(st);
      const Bytes old_pdb = slurp(st / "PIONEER/rekordbox/export.pdb");
      std::vector<ExportTrack> ft2 = ft;
      ft2.push_back(make_track(99, src));
      for (int fail : {0, 3, 20}) {
        ExportOptions o2;
        o2.destination = st.string();
        o2.fail_after_commits = fail;
        CHECK(!export_usb(ft2, {{7, "New", 0, false, {99}}}, o2, err));
        CHECK(listing(st) == snapshot);
        CHECK(slurp(st / "PIONEER/rekordbox/export.pdb") == old_pdb);
        CHECK(slurp(old_audio) == precious);
      }
      // The successful retry keeps the pre-existing audio file and writes the colliding track elsewhere.
      ExportOptions o3;
      o3.destination = st.string();
      CHECK(export_usb(ft2, {{7, "New", 0, false, {99}}}, o3, err));
      CHECK(slurp(old_audio) == precious);
      ReadExport after;
      CHECK(read_export(st.string(), after, err));
      CHECK(after.tracks.size() == 9);
      std::set<std::string> ps;
      for (const auto& t : after.tracks) CHECK(ps.insert(t.file_path).second);
      CHECK(after.tracks[0].file_path != before.tracks[0].file_path);  // moved to "(2)"
      ReadExport again;
      CHECK(export_usb(ft2, {{7, "New", 0, false, {99}}}, o3, err));  // idempotent: identical audio is reused
      CHECK(read_export(st.string(), again, err));
      for (std::size_t i = 0; i < again.tracks.size(); ++i) CHECK(again.tracks[i].file_path == after.tracks[i].file_path);
      std::size_t audio_files = 0;
      for (const auto& e : fs::recursive_directory_iterator(st / "Contents")) audio_files += e.is_regular_file();
      CHECK(audio_files == 10);  // 9 tracks + the preserved original
      expect_valid(st);
    }
#if !defined(_WIN32)
    // 4. read-only folder
    if (geteuid() != 0) {
      const fs::path st = tmp / "rostick";
      fs::create_directories(st / "PIONEER");
      fs::permissions(st / "PIONEER", fs::perms::owner_read | fs::perms::owner_exec);
      ExportOptions o;
      o.destination = st.string();
      const auto snapshot = listing(st);
      CHECK(!export_usb(ft, {}, o, err) && !err.empty());
      CHECK(listing(st) == snapshot);
      fs::permissions(st / "PIONEER", fs::perms::owner_all);
    }
#endif
  }

  // ---- 20 000 tracks, 500 playlists: time, memory, validity ---------------------------------------------------------
  {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ExportTrack> big;
    big.reserve(20000);
    for (int i = 1; i <= 20000; ++i) {
      ExportTrack t;
      t.id = i;
      t.title = "Synthetic track " + std::to_string(i) + (i % 9 == 0 ? " \xE6\x9D\xB1" : "");
      t.artist = "Artist " + std::to_string(i % 1500);
      t.album = "Album " + std::to_string(i % 3000);
      t.genre = "Genre " + std::to_string(i % 40);
      t.key = std::to_string(1 + i % 12) + (i % 2 ? "A" : "B");
      t.bpm = 90 + i % 80;
      t.duration_ms = 30000 + i % 1000;
      for (int b = 0; b < 8; ++b) t.beats.push_back({std::int64_t(b) * 500, t.bpm, b % 4 + 1});
      ExportCue c;
      c.time_ms = 1000;
      t.cues.push_back(c);
      big.push_back(std::move(t));
    }
    std::vector<ExportPlaylist> pls;
    for (int f = 1; f <= 20; ++f) pls.push_back({f, "Folder " + std::to_string(f), 0, true, {}});
    for (int p = 0; p < 480; ++p) {
      ExportPlaylist pl{21 + p, "Playlist " + std::to_string(p), 1 + p % 20, false, {}};
      for (int k = 0; k < 60; ++k) pl.track_ids.push_back(1 + (p * 37 + k * 11) % 20000);
      pls.push_back(std::move(pl));
    }
    ExportOptions o;
    o.destination = (tmp / "big").string();
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(export_usb(big, pls, o, err));
    const auto t2 = std::chrono::steady_clock::now();
    ValidationReport rep;
    ValidateOptions vo;
    vo.check_audio = false;
    CHECK(validate_export(o.destination, rep, vo));
    CHECK(rep.ok() && rep.tracks_checked == 20000);
    const auto t3 = std::chrono::steady_clock::now();
    ReadExport rx;
    CHECK(read_export(o.destination, rx, err));
    CHECK(rx.tracks.size() == 20000 && rx.playlists.size() == 500);
    long rss_mb = -1;
#if !defined(_WIN32)
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(__APPLE__)
      rss_mb = ru.ru_maxrss / (1024 * 1024);
#else
      rss_mb = ru.ru_maxrss / 1024;
#endif
    }
#endif
    auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    std::fprintf(stderr, "20000 tracks / 500 playlists: build %.1fs, export %.1fs, validate %.1fs, read-back %.1fs, peak RSS %ld MB (whole test process)\n",
                 secs(t0, t1), secs(t1, t2), secs(t2, t3), secs(t3, std::chrono::steady_clock::now()), rss_mb);
    fs::remove_all(tmp / "big");
  }

  fs::remove_all(tmp);
  return 0;
}
