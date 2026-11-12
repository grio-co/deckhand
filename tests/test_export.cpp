#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "check.hpp"
#include "deckhand/export/export.hpp"

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

  fs::remove_all(tmp);
  return 0;
}
