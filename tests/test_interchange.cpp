// Library interchange: DJ library XML round trip, escaping, malformed input, dry run, merge, M3U8.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "check.hpp"
#include "deckhand/import/interchange.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {

fs::path g_dir;

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  CHECK(f.good());
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void spit(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << s;
}
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string db_path(const char* name) { return (g_dir / (std::string(name) + ".db")).string(); }

std::int64_t add(Library& lib, const std::string& path, const char* title, const char* artist = "", double bpm = 0,
                 std::int64_t dur = 0) {
  NewTrack t;
  t.path = path;
  t.title = title;
  t.artist = artist;
  t.bpm = bpm;
  t.duration_ms = dur;
  return lib.upsert_track(t);
}

std::vector<std::string> list_paths(Library& lib, std::int64_t playlist) {
  std::vector<std::string> out;
  for (const auto id : lib.playlist_track_ids(playlist)) out.push_back(lib.track(id)->path);
  return out;
}
std::int64_t find_playlist(Library& lib, const std::string& name) {
  for (const auto& p : lib.playlists())
    if (p.name == name) return p.id;
  return 0;
}

struct Fixture {
  std::string a_path, b_path, c_path;  // real files for a, b; c is missing
};

Fixture build_source(Library& lib) {
  Fixture f;
  const fs::path music = g_dir / "Music & More";
  f.a_path = (music / "a b#1%2.mp3").string();
#ifdef _WIN32
  f.b_path = (music / "x q.flac").string();  // < > " are not legal in Windows file names
#else
  f.b_path = (music / "<x> \"q\" \xC3\xA9\xE6\x97\xA5\xE6\x9C\xAC.flac").string();
#endif
  f.c_path = (g_dir / "gone" / "zz.wav").string();
  spit(f.a_path, "0123456789");
  spit(f.b_path, "abcdef");

  const auto a = add(lib, f.a_path, "Fish & Chips <Remix> \"live\"", "Tom's \xC3\x85rtist", 128.4, 215000);
  const auto b = add(lib, f.b_path, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E title", "B", 0, 61000);
  const auto c = add(lib, f.c_path, "Missing one", "C", 174, 180000);

  lib.edit_track(a, [] {
    TrackEdit e;
    e.comment = "line1\nline2 & <tab>\t\"q\"";
    e.rating = 4;
    e.key = "8A";
    e.genre = "House";
    e.album = "Alb&um";
    return e;
  }());
  lib.set_track_color(a, 0x3366CC);
  lib.replace_beatgrid(a, {{120.5, 128.4, 1}, {30120.25, 129.0, 3}});
  lib.set_hot_cue(a, 0, 1500, -1);
  lib.set_hot_cue_style(a, 0, "Intro & \"drop\"", 0xFF8800);
  lib.set_hot_cue(a, 3, 5000, 9000);  // a hot loop
  lib.set_hot_cue_style(a, 3, "Loop \xC3\xA9", -1);
  {
    Cue m;
    m.kind = CueKind::Memory;
    m.start_ms = 1000;
    m.label = "Drop";
    m.color = 0xFF0000;
    Cue l;
    l.kind = CueKind::Loop;
    l.start_ms = 20000;
    l.end_ms = 24000;
    lib.replace_memory_cues(a, {m, l});
  }
  lib.set_hot_cue(b, 1, 250, -1);
  lib.replace_beatgrid(c, {{0, 174, 1}});
  lib.set_key(c, "5B");

  const auto sets = lib.create_playlist(0, "Sets & <Live>", true);
  const auto inner = lib.create_playlist(sets, "Nested \xC3\xA9", true);
  const auto p1 = lib.create_playlist(inner, "Warm up", false);
  lib.set_playlist_tracks(p1, {c, a, b, a});  // order and a repeat survive
  const auto p2 = lib.create_playlist(sets, "Peak", false);
  lib.set_playlist_tracks(p2, {b, c});
  lib.create_playlist(0, "Empty", false);
  const auto top = lib.create_playlist(0, "Top", false);
  lib.set_playlist_tracks(top, {a});

  const auto s = lib.begin_history_session();
  lib.add_history(s, a, "A", 1000);
  lib.add_history(s, a, "B", 2000);
  lib.add_history(s, c, "A", 3000);
  return f;
}

static std::string native(std::string p) {
#ifdef _WIN32
  for (char& c : p) if (c == '/') c = '\\';
#endif
  return p;
}

void test_round_trip() {
  Library src(db_path("src"));
  const Fixture f = build_source(src);
  const fs::path x1 = g_dir / "out" / "one.xml", x2 = g_dir / "out" / "two.xml";

  const auto ex = export_library_xml(src, x1.string());
  CHECK(ex.ok && ex.error.empty());
  CHECK(ex.tracks == 3 && ex.playlists == 6 && ex.cues == 5);
  const std::string xml = slurp(x1);
  CHECK(xml.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<DJ_PLAYLISTS", 0) == 0);
  // escaping, URLs, UTF-8 kept as UTF-8
  CHECK(has(xml, "Fish &amp; Chips &lt;Remix&gt; &quot;live&quot;"));
  CHECK(has(xml, "Sets &amp; &lt;Live&gt;"));
  CHECK(has(xml, "line1&#10;line2 &amp; &lt;tab&gt;&#9;&quot;q&quot;"));
  CHECK(has(xml, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E title"));
  CHECK(has(xml, "Location=\"file://localhost/"));
  CHECK(has(xml, "a%20b%231%252.mp3"));
  CHECK(has(xml, "Music%20%26%20More"));
#ifdef _WIN32
  CHECK(has(xml, "x%20q.flac"));
#else
  CHECK(has(xml, "%3Cx%3E%20%22q%22%20%C3%A9%E6%97%A5%E6%9C%AC.flac"));
#endif
  CHECK(!has(xml, "Fish & Chips"));
  CHECK(has(xml, "Rating=\"204\"") && has(xml, "Colour=\"0x3366CC\"") && has(xml, "Tonality=\"8A\""));
  CHECK(has(xml, "<TEMPO Inizio=\"0.12050\" Bpm=\"128.4000\" Metro=\"4/4\" Battito=\"1\"/>"));
  CHECK(has(xml, "Type=\"4\" Start=\"5.000\" End=\"9.000\" Num=\"3\""));
  CHECK(has(xml, "PlayCount=\"2\""));

  Library dst(db_path("dst"));
  InterchangeImportOptions o;
  o.import_play_counts = true;
  const auto im = import_library_xml(dst, x1.string(), o);
  CHECK(im.ok);
  CHECK(im.tracks_in_file == 3 && im.tracks_added == 3 && im.tracks_missing_file == 1);
  CHECK(im.missing_paths.size() == 1 && im.missing_paths[0] == f.c_path);
  CHECK(im.playlists_created == 6 && im.cues == 5 && im.grids == 2 && im.history_entries == 3);
  CHECK(dst.track_count() == 3);

  CHECK(export_library_xml(dst, x2.string()).ok);
  CHECK(slurp(x2) == xml);  // byte identical

  // what survived, read back from the library itself
  const auto a = *dst.track_id_for_path(f.a_path);
  CHECK(dst.track(a)->title == "Fish & Chips <Remix> \"live\"");
  CHECK(dst.track(a)->comment == "line1\nline2 & <tab>\t\"q\"");
  CHECK(dst.track(a)->rating == 4 && dst.track(a)->color == 0x3366CC && dst.track(a)->key == "8A");
  const auto grid = dst.beatgrid(a);
  CHECK(grid.size() == 2 && grid[0].first_beat_ms == 120.5 && grid[1].first_beat_ms == 30120.25 && grid[1].bar_beat == 3);
  CHECK(grid[1].bpm == 129.0);
  int hot = 0, mem = 0;
  for (const Cue& c : dst.cues(a)) {
    if (c.slot == 0) {
      CHECK(c.kind == CueKind::Hot && c.start_ms == 1500 && c.label == "Intro & \"drop\"" && c.color == 0xFF8800);
      ++hot;
    } else if (c.slot == 3) {
      CHECK(c.kind == CueKind::Loop && c.start_ms == 5000 && c.end_ms == 9000 && c.label == "Loop \xC3\xA9");
      ++hot;
    } else {
      ++mem;
    }
  }
  CHECK(hot == 2 && mem == 2);
  const auto mc = dst.memory_cues(a);
  CHECK(mc.size() == 2 && mc[0].label == "Drop" && mc[0].color == 0xFF0000 && mc[1].kind == CueKind::Loop &&
        mc[1].end_ms == 24000);
  const auto warm = find_playlist(dst, "Warm up");
  CHECK(list_paths(dst, warm) == list_paths(src, find_playlist(src, "Warm up")));
  CHECK(list_paths(dst, warm).size() == 4);
  CHECK(list_paths(dst, find_playlist(dst, "Peak")) == list_paths(src, find_playlist(src, "Peak")));
  CHECK(dst.track(a)->play_count == 2);

  // a subtree export carries only its own tracks and playlists
  InterchangeExportOptions so;
  so.playlist_id = find_playlist(src, "Peak");
  const fs::path x3 = g_dir / "out" / "peak.xml";
  const auto sx = export_library_xml(src, x3.string(), so);
  CHECK(sx.ok && sx.tracks == 2 && sx.playlists == 1);
  CHECK(!has(slurp(x3), "Fish &amp;"));
  so.playlist_id = 99999;
  CHECK(!export_library_xml(src, (g_dir / "out" / "nope.xml").string(), so).ok);
  InterchangeExportOptions bare;
  bare.include_cues = bare.include_grid = bare.include_playlists = false;
  CHECK(export_library_xml(src, (g_dir / "out" / "bare.xml").string(), bare).ok);
  const std::string bx = slurp(g_dir / "out" / "bare.xml");
  CHECK(!has(bx, "POSITION_MARK") && !has(bx, "TEMPO") && !has(bx, "<NODE Type=\"1\""));

  // dry run: exact statistics, no write
  Library dry(db_path("dry"));
  InterchangeImportOptions d;
  d.dry_run = true;
  const auto ds = import_library_xml(dry, x1.string(), d);
  CHECK(ds.ok && ds.dry_run && ds.tracks_added == 3 && ds.playlists_created == 6 && ds.cues == 5);
  CHECK(dry.track_count() == 0 && dry.playlists().empty());

  // importing the same file twice adds nothing
  Library twice(db_path("twice"));
  CHECK(import_library_xml(twice, x1.string()).ok);
  const auto second = import_library_xml(twice, x1.string());
  CHECK(second.ok && second.tracks_added == 0 && second.tracks_matched_by_path == 3 && second.playlists_created == 0 &&
        second.playlists_merged == 6 && second.playlist_entries_added == 0 && second.cues == 0 && second.grids == 0 &&
        second.tracks_updated == 0);
  CHECK(twice.track_count() == 3 && twice.playlists().size() == 6);
  CHECK(list_paths(twice, find_playlist(twice, "Warm up")).size() == 4);
  CHECK(twice.track(*twice.track_id_for_path(f.a_path))->play_count == 0);  // counts were not asked for

  // skip mode leaves existing tracks alone
  InterchangeImportOptions skip;
  skip.merge = false;
  const auto sk = import_library_xml(twice, x1.string(), skip);
  CHECK(sk.ok && sk.tracks_skipped == 3 && sk.tracks_added == 0);
}

void test_fill_vs_overwrite() {
  Library src(db_path("fo_src"));
  build_source(src);
  const fs::path x = g_dir / "out" / "fo.xml";
  CHECK(export_library_xml(src, x.string()).ok);
  const std::string a_path = src.track(1)->path;

  Library lib(db_path("fo_dst"));
  const auto a = add(lib, a_path, "Mine", "", 100);
  lib.set_hot_cue(a, 0, 7777, -1);                    // slot 0 taken, others empty
  lib.replace_beatgrid(a, {{5, 100, 1}});             // has a grid
  const auto r = import_library_xml(lib, x.string());
  CHECK(r.ok && r.tracks_matched_by_path == 1 && r.tracks_added == 2);
  CHECK(lib.track(a)->title == "Mine" && lib.track(a)->bpm == 100);  // only empty fields are filled
  CHECK(lib.track(a)->key == "8A" && lib.track(a)->rating == 4 && lib.track(a)->comment.find("line1") == 0);
  CHECK(lib.beatgrid(a).size() == 1 && lib.beatgrid(a)[0].first_beat_ms == 5);
  bool slot0 = false, slot3 = false;
  for (const Cue& c : lib.cues(a)) {
    if (c.slot == 0) slot0 = c.start_ms == 7777;
    if (c.slot == 3) slot3 = true;
  }
  CHECK(slot0 && slot3 && lib.memory_cues(a).size() == 2);

  InterchangeImportOptions ow;
  ow.overwrite_cues = ow.overwrite_grid = ow.overwrite_metadata = true;
  const auto r2 = import_library_xml(lib, x.string(), ow);
  CHECK(r2.ok && r2.tracks_updated >= 1);
  CHECK(lib.track(a)->title == "Fish & Chips <Remix> \"live\"" && lib.beatgrid(a).size() == 2);
  for (const Cue& c : lib.cues(a))
    if (c.slot == 0) CHECK(c.start_ms == 1500);
}

void test_edit_protection() {
  Library src(db_path("ep_src"));
  build_source(src);
  const fs::path x = g_dir / "out" / "ep.xml";
  CHECK(export_library_xml(src, x.string()).ok);
  Library lib(db_path("ep_dst"));
  const auto a = add(lib, src.track(1)->path, "Mine", "", 100);
  lib.set_key(a, "5A");
  lib.set_key(a, "");  // the user emptied it on purpose
  const auto r = import_library_xml(lib, x.string());
  CHECK(r.ok && lib.track(a)->key.empty() && lib.track(a)->rating == 4);  // key protected, an untouched field filled
  bool rating_imported = false, key_imported = false;
  for (const auto& e : lib.track_history(a, 200)) {
    rating_imported = rating_imported || (e.field == "rating" && e.source == "import");
    key_imported = key_imported || (e.field == "key" && e.source == "import");
  }
  CHECK(rating_imported && !key_imported);
  InterchangeImportOptions ow;
  ow.overwrite_metadata = true;
  CHECK(import_library_xml(lib, x.string(), ow).ok && lib.track(a)->key == "8A");
  CHECK(lib.track_history(a)[0].source == "import");
  // A dry run journals nothing.
  const auto before = lib.track_history(a, 200).size();
  ow.dry_run = true;
  lib.set_key(a, "");
  ow.overwrite_metadata = true;
  CHECK(import_library_xml(lib, x.string(), ow).ok && lib.track(a)->key.empty() && lib.track_history(a, 200).size() == before + 1);
}

void test_match_by_name() {
  const fs::path moved = g_dir / "moved" / "Song.mp3";
  spit(moved, "x");
  Library lib(db_path("byname"));
  const auto id = add(lib, (g_dir / "oldplace" / "song.mp3").string(), "Song", "", 0, 100500);  // file is gone
  const fs::path x = g_dir / "out" / "byname.xml";
  spit(x, "<DJ_PLAYLISTS Version=\"1.0.0\"><COLLECTION Entries=\"1\"><TRACK TrackID=\"7\" Name=\"Song\" TotalTime=\"100\" Location=\"" +
              interchange_path_to_url(moved.string()) + "\"><TEMPO Inizio=\"0.5\" Bpm=\"120\" Metro=\"4/4\" Battito=\"1\"/></TRACK>"
              "</COLLECTION><PLAYLISTS><NODE Type=\"0\" Name=\"ROOT\" Count=\"1\"><NODE Type=\"1\" Name=\"L\" KeyType=\"0\" Entries=\"1\">"
              "<TRACK Key=\"7\"/></NODE></NODE></PLAYLISTS></DJ_PLAYLISTS>");
  const auto r = import_library_xml(lib, x.string());
  CHECK(r.ok && r.tracks_matched_by_name == 1 && r.tracks_added == 0 && lib.track_count() == 1);
  CHECK(lib.track(id)->path == moved.string() && lib.beatgrid(id).size() == 1);
  CHECK(list_paths(lib, find_playlist(lib, "L")).size() == 1);
}

void test_tolerant_and_malformed() {
  Library lib(db_path("tol"));
  const fs::path real = g_dir / "tol" / "r.mp3";
  spit(real, "x");
  const std::string url = interchange_path_to_url(real.string());
  // unknown elements and attributes, comments, CDATA, a DOCTYPE, single quotes, numeric references, a fade-in mark,
  // KeyType 1 (by location), an unknown key, a NODE without Type
  const std::string xml =
      "\xEF\xBB\xBF<?xml version='1.0' encoding='UTF-8'?>\n<!DOCTYPE DJ_PLAYLISTS [ <!ENTITY x 'y'> ]>\n<!-- hi -->\n"
      "<DJ_PLAYLISTS Version='1.0.0' Extra='1'><PRODUCT Name='Other'/><Unknown><Deep/></Unknown>"
      "<COLLECTION><TRACK TrackID='1' Name='A&#38;B &#x41;' Whatever='1' Location='" + url + "' AverageBpm='127,5' Rating='255'>"
      "<![CDATA[ x < y ]]><POSITION_MARK Type='1' Start='1.0' Num='-1'/><POSITION_MARK Type='0' Start='2.0' Num='-1' Name='m'/>"
      "<POSITION_MARK Type='0' Start='bad'/><FUTURE a='b'/></TRACK>"
      "<TRACK TrackID='2' Name='no location'/></COLLECTION>"
      "<PLAYLISTS><NODE Name='ROOT'><NODE Name='F'><NODE Type='1' Name='P' KeyType='1'><TRACK Key='" + url + "'/>"
      "<TRACK Key='file://localhost/nope/none.mp3'/></NODE></NODE><NODE Type='1' Name='Q'><TRACK Key='1'/><TRACK Key='99'/></NODE>"
      "</NODE></PLAYLISTS></DJ_PLAYLISTS>";
  const fs::path x = g_dir / "out" / "tol.xml";
  spit(x, xml);
  const auto r = import_library_xml(lib, x.string());
  CHECK(r.ok);
  CHECK(r.tracks_added == 1 && r.marks_ignored == 2 && r.playlist_entries_unresolved == 2 && !r.warnings.empty());
  const auto id = *lib.track_id_for_path(real.string());
  CHECK(lib.track(id)->title == "A&B A" && lib.track(id)->bpm == 127.5 && lib.track(id)->rating == 5);
  CHECK(lib.memory_cues(id).size() == 1 && lib.memory_cues(id)[0].label == "m");
  CHECK(list_paths(lib, find_playlist(lib, "P")).size() == 1 && list_paths(lib, find_playlist(lib, "Q")).size() == 1);
  CHECK(find_playlist(lib, "ROOT") == 0);

  // malformed input: a clear error, nothing written
  const std::string head = "<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"1\" Name=\"x\" Location=\"file://localhost/q/x.mp3\"/>";
  const std::vector<std::string> bad = {
      head + "</COLLECTION>",                                         // never closed
      head + "</WRONG></DJ_PLAYLISTS>",                               // mismatched
      "<DJ_PLAYLISTS><COLLECTION><TRACK Name=\"a&bogus;\"/></COLLECTION></DJ_PLAYLISTS>",
      "<DJ_PLAYLISTS><COLLECTION><TRACK Name=a/></COLLECTION></DJ_PLAYLISTS>",
      "<DJ_PLAYLISTS><COLLECTION><TRACK Name=\"a<b\"/></COLLECTION></DJ_PLAYLISTS>",
      "<DJ_PLAYLISTS><COLLECTION><TRACK Name=\"&#0;\"/></COLLECTION></DJ_PLAYLISTS>",
      "<other/>",                                                     // wrong document
      "plain text",
      "",
      "<DJ_PLAYLISTS/><DJ_PLAYLISTS/>",
      "\xFF\xFE<\0D\0",                                               // UTF-16
      "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><DJ_PLAYLISTS/>",
      "<DJ_PLAYLISTS Name=\"\xC3\x28\"/>",                            // invalid UTF-8
      std::string(300, '<') + "a",
  };
  std::string deep;
  for (int i = 0; i < 400; ++i) deep += "<a>";
  const auto before = lib.track_count();
  int n = 0;
  for (const std::string& b : bad) {
    const fs::path bp = g_dir / "out" / ("bad" + std::to_string(n++) + ".xml");
    spit(bp, b);
    const auto e = import_library_xml(lib, bp.string());
    CHECK(!e.ok && !e.error.empty() && e.tracks_added == 0);
  }
  spit(g_dir / "out" / "deep.xml", deep);
  CHECK(!import_library_xml(lib, (g_dir / "out" / "deep.xml").string()).ok);
  CHECK(!import_library_xml(lib, (g_dir / "out" / "does_not_exist.xml").string()).ok);
  CHECK(lib.track_count() == before);
}

void test_urls() {
  CHECK(interchange_path_to_url("/a b/c#d%e.mp3") == "file://localhost/a%20b/c%23d%25e.mp3");
  CHECK(interchange_url_to_path("file://localhost/a%20b/c%23d%25e.mp3") == native("/a b/c#d%e.mp3"));
  CHECK(interchange_url_to_path("file:///x/y.mp3") == native("/x/y.mp3"));
  CHECK(interchange_url_to_path("file://somehost/x/y.mp3") == native("/x/y.mp3"));
  CHECK(interchange_path_to_url("C:\\Music\\\xC3\xA9.mp3") == "file://localhost/C:/Music/%C3%A9.mp3");
  CHECK(interchange_url_to_path("file://localhost/C:/Music/%C3%A9.mp3") == native("C:/Music/\xC3\xA9.mp3"));
  CHECK(interchange_url_to_path("/plain/100%.mp3") == "/plain/100%.mp3");
}

void test_m3u() {
  const fs::path music = g_dir / "m3" / "music";
  const fs::path t1 = music / "one.mp3", t2 = music / "sub dir" / "two \xC3\xA9.mp3", t3 = music / "three.mp3";
  spit(t1, "1");
  spit(t2, "2");
  Library lib(db_path("m3u"));
  const auto a = add(lib, t1.string(), "One", "Art", 0, 61000);
  const auto b = add(lib, t2.string(), "Two", "", 0, 1000);
  const auto c = add(lib, t3.string(), "Three", "", 0, 0);  // file missing
  const auto f = lib.create_playlist(0, "Folder: A/B", true);
  const auto p = lib.create_playlist(f, "Mix * \"1\"?", false);
  lib.set_playlist_tracks(p, {c, a, b});
  const auto p2 = lib.create_playlist(f, "mix _ _1__", false);  // collides after sanitising? names differ in case only below
  lib.set_playlist_tracks(p2, {a});
  const auto p3 = lib.create_playlist(0, "CON", false);
  lib.set_playlist_tracks(p3, {b});

  for (const bool rel : {false, true}) {
    const fs::path out = g_dir / "m3" / (rel ? "rel.m3u8" : "abs.m3u8");
    const auto e = export_playlist_m3u8(lib, p, out.string(), rel);
    CHECK(e.ok && e.tracks == 3);
    const std::string text = slurp(out);
    CHECK(text.rfind("#EXTM3U\n", 0) == 0 && has(text, "#EXTINF:61,Art - One\n") && has(text, "#EXTINF:-1,Three\n"));
    if (rel) CHECK(has(text, "\nmusic/one.mp3\n") && has(text, "\nmusic/sub dir/two \xC3\xA9.mp3\n"));
    else CHECK(has(text, "\n" + t1.string() + "\n"));
    Library fresh(db_path(rel ? "m3u_rel" : "m3u_abs"));
    const auto a2 = add(fresh, t1.string(), "One");
    add(fresh, t2.string(), "Two");
    const auto im = import_playlist_m3u8(fresh, out.string());
    CHECK(im.ok && im.playlists_created == 1 && im.tracks_matched_by_path == 2 && im.tracks_added == 1 && im.tracks_missing_file == 1);
    const auto pl = find_playlist(fresh, "Mix * \"1\"?");
    CHECK(pl && list_paths(fresh, pl) == std::vector<std::string>({t3.string(), t1.string(), t2.string()}));
    CHECK(a2 > 0);
    // again: nothing duplicated
    const auto again = import_playlist_m3u8(fresh, out.string());
    CHECK(again.ok && again.playlists_created == 0 && again.playlists_merged == 1 && again.playlist_entries_added == 0 &&
          fresh.track_count() == 3);
  }
  CHECK(!export_playlist_m3u8(lib, 424242, (g_dir / "m3" / "no.m3u8").string()).ok);
  CHECK(!import_playlist_m3u8(lib, (g_dir / "m3" / "absent.m3u8").string()).ok);

  // a folder exports flattened
  const auto flat = export_playlist_m3u8(lib, f, (g_dir / "m3" / "flat.m3u8").string(), false);
  CHECK(flat.ok && flat.tracks == 4);

  // everything: folders as directories, safe names
  const fs::path all = g_dir / "m3" / "all";
  const auto ea = export_all_playlists_m3u8(lib, all.string());
  CHECK(ea.ok && ea.files == 3);
  CHECK(fs::is_directory(all / "Folder_ A_B"));
  CHECK(fs::exists(all / "Folder_ A_B" / "Mix _ _1__.m3u8"));
  CHECK(fs::exists(all / "Folder_ A_B" / "mix _ _1__ (2).m3u8"));
  CHECK(fs::exists(all / "_CON.m3u8"));
  CHECK(has(slurp(all / "Folder_ A_B" / "Mix _ _1__.m3u8"), "\n../../music/one.mp3\n"));

  // a hand-made file: BOM, CRLF, comments, a file:// URL, relative path with backslashes, a stream, blank lines
  const fs::path hand = g_dir / "m3" / "hand" / "Evening.m3u8";
  spit(hand, "\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:90,Someone - New Song\r\n../music/one.mp3\r\n\r\n# a comment\r\n" +
                 std::string("#EXTINF:120,Other\r\n") + interchange_path_to_url(t2.string()) + "\r\n" +
                 "http://example.com/stream\r\n..\\music\\three.mp3\r\n#EXTINF:30,Fresh - Brand New\r\n" +
                 (g_dir / "m3" / "nowhere" / "new.mp3").string() + "\r\n");
  Library lib2(db_path("m3u_hand"));
  M3uImportOptions mo;
  mo.dry_run = true;
  const auto dry = import_playlist_m3u8(lib2, hand.string(), mo);
  CHECK(dry.ok && dry.playlists_created == 1 && lib2.track_count() == 0 && lib2.playlists().empty());
  const auto hr = import_playlist_m3u8(lib2, hand.string());
  CHECK(hr.ok && hr.tracks_in_file == 4 && hr.tracks_added == 4 && hr.playlist_entries_unresolved == 1);
  const auto ep = find_playlist(lib2, "Evening");
  CHECK(ep && list_paths(lib2, ep) ==
                  std::vector<std::string>({t1.lexically_normal().string(), t2.string(), t3.lexically_normal().string(),
                                            (g_dir / "m3" / "nowhere" / "new.mp3").string()}));
  const auto first = lib2.track(lib2.playlist_track_ids(ep)[0]);
  CHECK(first->title == "New Song" && first->artist == "Someone" && first->duration_ms == 90000);
  M3uImportOptions named;
  named.playlist_name = "Renamed";
  CHECK(import_playlist_m3u8(lib2, hand.string(), named).playlists_created == 1);
  CHECK(find_playlist(lib2, "Renamed") != 0);
}

}  // namespace

int main() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  int local = 0;
  g_dir = fs::temp_directory_path() / ("deckhand_interchange_" + std::to_string(stamp) + "_" +
                                       std::to_string(reinterpret_cast<std::uintptr_t>(&local) & 0xFFFFF));
  fs::create_directories(g_dir);
  test_urls();
  test_round_trip();
  test_fill_vs_overwrite();
  test_edit_protection();
  test_match_by_name();
  test_tolerant_and_malformed();
  test_m3u();
  std::error_code ec;
  fs::remove_all(g_dir, ec);
  std::puts("test_interchange: ok");
  return 0;
}
