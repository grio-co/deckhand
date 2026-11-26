// deckhandctl: library tools without the UI.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>

#include <chrono>
#include <thread>

#include "deckhand/api/auth.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/import/import.hpp"
#include "deckhand/import/interchange.hpp"
#include "deckhand/library/backup.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/midi/controller.hpp"
#include "deckhand/log/log.hpp"
#include "deckhand/services/usb_export.hpp"

using namespace deckhand;

namespace {

#if defined(_WIN32)
std::string env_or(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return v && *v ? v : fallback;
}
std::string home() { return env_or("USERPROFILE", "."); }
std::string default_data_dir() { return env_or("APPDATA", home()) + "\\DeckHand"; }
// The source library lives in the roaming profile on Windows.
std::string source_library_dir() { return env_or("APPDATA", home()) + "\\Pioneer\\rekordbox"; }
#else
std::string home() {
  const char* h = std::getenv("HOME");
  return h ? h : ".";
}
std::string default_data_dir() { return home() + "/Library/Application Support/DeckHand"; }
std::string source_library_dir() { return home() + "/Library/Pioneer/rekordbox"; }
#endif

std::string read_file_trimmed(const std::string& path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

const char* arg_value(int argc, char** argv, const char* name, const char* fallback) {
  for (int i = 2; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return fallback;
}

int usage() {
  std::fprintf(stderr,
               "usage: deckhandctl <command> [options]\n"
               "  import     [--source DB] [--key-file FILE] [--analysis DIR] [--data DIR] [--overwrite-edits]\n"
               "  playlists  [--data DIR]\n"
               "  list       [--playlist ID] [--search TEXT] [--data DIR]\n"
               "  export DIR [--playlist ID] [--no-copy 1] [--convert 1] [--profile ID] [--data DIR]   write a club-player USB export\n"
               "  export-xml FILE [--playlist ID]     write the library as DJ library XML (collection, grids, cues, playlists)\n"
               "  import-xml FILE [--dry-run 1] [--overwrite-metadata 1] [--overwrite-cues 1] [--overwrite-grid 1] [--no-merge 1]\n"
               "  export-m3u8 FILE|DIR [--playlist ID] [--relative 1]   one playlist to a file, or (no --playlist) all into a folder\n"
               "  import-m3u8 FILE [--name NAME] [--parent ID] [--dry-run 1]\n"
               "  duplicates [--mode exact|likely|all] [--same-bpm 1] [--same-key 1] [--no-hash 1]   list duplicate groups (hashes files that share a size first)\n"
               "  merge-duplicates KEEP_ID REMOVE_ID[,ID...] [--copy-cues missing|all|none] [--dry-run 1]   removed tracks go to the trash; files stay\n"
               "  strip-analysis --kinds grid,key,loudness,waveform,first_sound,phrases,vocals,stems  (--ids 1,2 | --playlist ID | --search TEXT | --all 1) [--force 1] [--dry-run 1]\n"
               "  backup                       copy the library into backups/   [--data DIR]\n"
               "  backups                      list the copies, newest first    [--data DIR]\n"
               "  restore FILE                 replace the library with a copy (quit DeckHand first)   [--data DIR]\n"
               "  token list | create NAME SCOPE[,SCOPE...] | revoke ID   [--data DIR]   API access tokens\n"
               "  devices                      list audio outputs\n"
               "  midi                         list connected MIDI input devices\n"
               "  hid                          list HID devices (vid:pid, usage, product); keyboards and mice are left out, nothing is opened\n"
               "  play FILE  [--seconds N] [--silent 1] [--device NAME]   decode and play through the engine\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  if (cmd == "devices") {
    for (const auto& d : Engine::output_devices()) std::printf("%s\n", d.c_str());
    return 0;
  }
  if (cmd == "midi") {
    auto io = create_midi_io([](const std::string&, const std::uint8_t*, std::size_t) {}, [] {});
    if (!io) return 1;
    for (const auto& d : io->devices()) std::printf("%s\n", d.c_str());
    return 0;
  }
  if (cmd == "hid") {
    auto io = create_hid_io([](const std::string&, int, const std::uint8_t*, std::size_t) {}, [] {});
    if (!io) {
      std::fprintf(stderr, "HID is not available on this platform\n");
      return 1;
    }
    io->set_matches({HidMatch{}});  // every device; enumerating opens nothing
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // the platform lists devices in the background
    for (const auto& d : io->devices()) {
      if (hid_usage_refused(d.usage_page, d.usage)) continue;  // keyboards, keypads, mice and pointers are never listed
      std::printf("%04x:%04x  usage %04x/%04x  %s%s%s\n", d.vid, d.pid, d.usage_page, d.usage, d.product.c_str(),
                  d.manufacturer.empty() ? "" : "  by ", d.manufacturer.c_str());
    }
    return 0;
  }
  if (cmd == "play" && argc >= 3) {
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    auto audio = decode_file(argv[2], error);
    if (!audio) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    const double decode_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("decoded %.1f s at %u Hz in %.2f s (%zu waveform points)\n",
                static_cast<double>(audio->frames) / audio->sample_rate, audio->sample_rate, decode_s, audio->wave.size());
    Engine e;
    OutputConfig oc;
    oc.device_hint = arg_value(argc, argv, "--device", "");
    if (!e.start(oc, error)) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    Command c;
    if (std::atoi(arg_value(argc, argv, "--silent", "0"))) {
      c.op = Op::MasterLevel;
      c.f = 0;
      e.send(c);
    }
    auto track = std::make_unique<LoadedTrack>();
    track->audio = audio;
    e.load(0, std::move(track));
    c = Command{};
    c.op = Op::PlayToggle;
    e.send(c);
    const double seconds = std::atof(arg_value(argc, argv, "--seconds", "3"));
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    const DeckView v = e.deck(0);
    std::printf("device %s, %d ch, %u Hz; played %.2f s of %.2f s wall time; level %.3f\n", e.device_name().c_str(),
                e.output_channels(), e.sample_rate(), v.position / v.sample_rate, seconds, v.level);
    e.stop();
    return 0;
  }
  const std::string data = arg_value(argc, argv, "--data", default_data_dir().c_str());
  try {
    std::filesystem::create_directories(data);
    // These work on the file, before (or instead of) opening the library, so that nothing upgrades it first.
    if (cmd == "backup") {
      std::printf("%s\n", backup_library(data + "/library.db").c_str());
      return 0;
    }
    if (cmd == "backups") {
      for (const auto& b : library_backups(data + "/library.db"))
        std::printf("%s\t%s\tschema %d\t%lld tracks\n", b.path.c_str(), b.reason.c_str(), b.schema_version,
                    static_cast<long long>(b.tracks));
      return 0;
    }
    if (cmd == "restore" && argc >= 3) {
      const std::string saved = restore_library(data + "/library.db", argv[2]);
      if (!saved.empty()) std::printf("the replaced library is at %s\n", saved.c_str());
      std::printf("restored\n");
      return 0;
    }
    Library lib(data + "/library.db");
    if (cmd == "import") {
      ImportOptions o;
      const std::string src_dir = source_library_dir();
      o.source_db = arg_value(argc, argv, "--source", (src_dir + "/master.db").c_str());
      o.analysis_root = arg_value(argc, argv, "--analysis", (src_dir + "/share").c_str());
      o.key = read_file_trimmed(arg_value(argc, argv, "--key-file", (data + "/source-v6.key").c_str()));
      o.scratch_dir = data + "/scratch";
      for (int i = 2; i < argc; ++i) o.overwrite_edited = o.overwrite_edited || std::string(argv[i]) == "--overwrite-edits";
      const ImportStats s = import_library_v6(lib, o);
      std::printf("tracks added %d, updated %d; playlists %d; cues %d; beatgrids %d\n", s.tracks_added,
                  s.tracks_updated, s.playlists, s.cues, s.beatgrids);
    } else if ((cmd == "export-xml" || cmd == "export-m3u8") && argc >= 3) {
      const std::int64_t playlist = std::atoll(arg_value(argc, argv, "--playlist", "0"));
      InterchangeExportStats st;
      if (cmd == "export-xml") {
        InterchangeExportOptions o;
        o.playlist_id = playlist;
        st = export_library_xml(lib, argv[2], o);
      } else if (playlist) {
        st = export_playlist_m3u8(lib, playlist, argv[2], std::atoi(arg_value(argc, argv, "--relative", "0")) != 0);
      } else {
        st = export_all_playlists_m3u8(lib, argv[2], std::atoi(arg_value(argc, argv, "--relative", "1")) != 0);
      }
      if (!st.ok) {
        std::fprintf(stderr, "error: %s\n", st.error.c_str());
        return 1;
      }
      std::printf("wrote %d tracks, %d playlists, %d cues, %d files\n", st.tracks, st.playlists, st.cues, st.files);
    } else if ((cmd == "import-xml" || cmd == "import-m3u8") && argc >= 3) {
      const bool dry = std::atoi(arg_value(argc, argv, "--dry-run", "0")) != 0;
      InterchangeStats st;
      if (cmd == "import-xml") {
        InterchangeImportOptions o;
        o.dry_run = dry;
        o.merge = !std::atoi(arg_value(argc, argv, "--no-merge", "0"));
        o.overwrite_metadata = std::atoi(arg_value(argc, argv, "--overwrite-metadata", "0")) != 0;
        o.overwrite_cues = std::atoi(arg_value(argc, argv, "--overwrite-cues", "0")) != 0;
        o.overwrite_grid = std::atoi(arg_value(argc, argv, "--overwrite-grid", "0")) != 0;
        st = import_library_xml(lib, argv[2], o);
      } else {
        M3uImportOptions o;
        o.dry_run = dry;
        o.playlist_name = arg_value(argc, argv, "--name", "");
        o.parent = std::atoll(arg_value(argc, argv, "--parent", "0"));
        st = import_playlist_m3u8(lib, argv[2], o);
      }
      if (!st.ok) {
        std::fprintf(stderr, "error: %s\n", st.error.c_str());
        return 1;
      }
      std::printf("%s: %d tracks in file, %d added, %d matched, %d playlists created, %d entries added, %d cues%s\n",
                  dry ? "dry run" : "imported", st.tracks_in_file, st.tracks_added,
                  st.tracks_matched_by_path + st.tracks_matched_by_name, st.playlists_created, st.playlist_entries_added,
                  st.cues, dry ? " (nothing written)" : "");
    } else if (cmd == "token" && argc >= 3) {
      api::TokenStore store(data + "/api-tokens.json");
      std::string error;
      if (!store.load(&error)) std::fprintf(stderr, "warning: %s\n", error.c_str());
      const std::string sub = argv[2];
      if (sub == "list") {
        for (const auto& t : store.list()) {
          std::string scopes;
          for (const auto& sc : t.scopes) scopes += (scopes.empty() ? "" : ",") + sc;
          std::printf("%s\t%s\t%s\n", t.id.c_str(), t.name.c_str(), scopes.c_str());
        }
      } else if (sub == "create" && argc >= 5) {
        std::vector<std::string> scopes;
        std::string list = argv[4];
        for (std::size_t a = 0; a <= list.size();) {
          const std::size_t b = std::min(list.find(',', a), list.size());
          if (b > a) scopes.push_back(list.substr(a, b - a));
          a = b + 1;
        }
        api::TokenStore::Created c;
        if (!store.create(argv[3], scopes, 0, {}, &c, &error)) {
          std::fprintf(stderr, "error: %s\n", error.c_str());
          return 1;
        }
        std::printf("id %s\ntoken %s\n(shown once; restart DeckHand to pick it up)\n", c.info.id.c_str(), c.secret.c_str());
      } else if (sub == "revoke" && argc >= 4) {
        if (!store.revoke(argv[3])) {
          std::fprintf(stderr, "error: no such token\n");
          return 1;
        }
      } else {
        return usage();
      }
    } else if (cmd == "export" && argc >= 3) {
      UsbExportRequest req;
      req.destination = argv[2];
      for (int i = 3; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--playlist") == 0) req.playlist_ids.push_back(std::atoll(argv[i + 1]));
      req.copy_audio = !std::atoi(arg_value(argc, argv, "--no-copy", "0"));
      req.convert_incompatible = std::atoi(arg_value(argc, argv, "--convert", "0")) != 0;
      req.player_profile = arg_value(argc, argv, "--profile", "");
      UsbExportResult res;
      std::string error;
      if (!export_library_to_usb(lib, req, res, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
      }
      std::printf("exported %d tracks, %d playlists; %d files missing\n", res.tracks, res.playlists, res.missing_files);
      for (const auto& m : res.missing) std::printf("  missing: %s\n", m.c_str());
      if (res.converted) std::printf("%d tracks written as 24-bit WAV\n", res.converted);
      if (res.remaining_will_not_play) std::printf("warning: %d tracks a club player will not play (see docs/EXPORT.md)\n", res.remaining_will_not_play);
    } else if (cmd == "duplicates") {
      const std::string mode = arg_value(argc, argv, "--mode", "all");
      if (!std::strcmp(arg_value(argc, argv, "--no-hash", "0"), "0")) {
        for (HashProgress p = lib.hash_pending(200); p.remaining > 0 && (p.sized + p.hashed + p.missing) > 0; p = lib.hash_pending(200))
          std::fprintf(stderr, "hashing, %d left\n", p.remaining);
      }
      DuplicateOptions o;
      o.exact = mode != "likely";
      o.likely = mode != "exact";
      o.same_bpm = !std::strcmp(arg_value(argc, argv, "--same-bpm", "0"), "1");
      o.same_key = !std::strcmp(arg_value(argc, argv, "--same-key", "0"), "1");
      const auto groups = lib.find_duplicates(o);
      for (const auto& g : groups) {
        std::printf("%s group, confidence %.2f, suggest keeping %lld (%s)", g.kind.c_str(), g.confidence,
                    static_cast<long long>(g.suggested_keep), g.suggestion_reason.c_str());
        for (const auto& d : g.differences) std::printf(" [%s differs]", d.c_str());
        std::printf("\n");
        for (const auto& m : g.members)
          std::printf("  %lld\t%s\t%d kbps\t%d plays\t%d cues\t%s\n", static_cast<long long>(m.row.id), m.row.format.c_str(), m.row.bitrate,
                      m.row.play_count, m.cue_count, m.row.path.c_str());
      }
      std::printf("%zu groups\n", groups.size());
    } else if (cmd == "merge-duplicates" && argc >= 4) {
      std::vector<std::int64_t> remove;
      std::string list = argv[3];
      for (std::size_t at = 0; at < list.size();) {
        const auto comma = list.find(',', at);
        remove.push_back(std::atoll(list.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str()));
        at = comma == std::string::npos ? list.size() : comma + 1;
      }
      MergeOptions o;
      const std::string c = arg_value(argc, argv, "--copy-cues", "missing");
      o.cues = c == "all" ? MergeOptions::Cues::All : c == "none" ? MergeOptions::Cues::None : MergeOptions::Cues::Missing;
      o.dry_run = !std::strcmp(arg_value(argc, argv, "--dry-run", "0"), "1");
      const MergeResult m = lib.merge_duplicates(std::atoll(argv[2]), remove, o);
      if (!m.ok) {
        std::fprintf(stderr, "error: %s\n", m.error.c_str());
        return 1;
      }
      std::printf("%skept %lld, %zu to the trash; %d playlists re-pointed (%d already had it); cues copied: %d hot, %d memory, %d loops; grid %s; %d plays moved; edit batch %lld\n",
                  m.dry_run ? "dry run: " : "", static_cast<long long>(m.kept), m.removed.size(), m.playlists_repointed, m.playlists_already_had,
                  m.hot_cues_copied, m.memory_cues_copied, m.loops_copied, m.grid_copied ? "copied" : "kept", m.history_moved,
                  static_cast<long long>(m.batch_id));
    } else if (cmd == "strip-analysis") {
      StripKinds k;
      std::string kinds = arg_value(argc, argv, "--kinds", "");
      for (std::size_t at = 0; at < kinds.size();) {
        const auto comma = kinds.find(',', at);
        const std::string n = kinds.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? kinds.size() : comma + 1;
        if (n == "grid") k.grid = true;
        else if (n == "key") k.key = true;
        else if (n == "loudness") k.loudness = true;
        else if (n == "waveform") k.waveform = true;
        else if (n == "first_sound") k.first_sound = true;
        else if (n == "phrases") k.phrases = true;
        else if (n == "vocals") k.vocals = true;
        else if (n == "stems") k.stems = true;
        else {
          std::fprintf(stderr, "unknown kind: %s\n", n.c_str());
          return 2;
        }
      }
      std::vector<std::int64_t> ids;
      const std::string idlist = arg_value(argc, argv, "--ids", "");
      if (!idlist.empty()) {
        for (std::size_t at = 0; at < idlist.size();) {
          const auto comma = idlist.find(',', at);
          ids.push_back(std::atoll(idlist.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str()));
          at = comma == std::string::npos ? idlist.size() : comma + 1;
        }
      } else if (std::strcmp(arg_value(argc, argv, "--playlist", ""), "") || std::strcmp(arg_value(argc, argv, "--search", ""), "")) {
        ids = lib.track_ids(std::atoll(arg_value(argc, argv, "--playlist", "0")), arg_value(argc, argv, "--search", ""));
      } else if (!std::strcmp(arg_value(argc, argv, "--all", "0"), "1")) {
        ids = lib.track_ids(0, "");
      } else {
        return usage();
      }
      if (!(k.grid || k.key || k.loudness || k.waveform || k.first_sound || k.phrases || k.vocals || k.stems)) return usage();
      const StripResult r = lib.strip_analysis(ids, k, !std::strcmp(arg_value(argc, argv, "--force", "0"), "1"),
                                               !std::strcmp(arg_value(argc, argv, "--dry-run", "0"), "1"));
      std::printf("%s%d tracks looked at, %d changed; grid locked and skipped: %d; keys set by hand and skipped: %d\n", r.dry_run ? "dry run: " : "",
                  r.tracks, r.tracks_changed, r.skipped_grid_locked, r.skipped_user_key);
      for (const auto& [kind, n] : r.cleared) std::printf("  %s: %d\n", kind.c_str(), n);
      for (const auto& n : r.notes) std::printf("  note: %s\n", n.c_str());
    } else if (cmd == "playlists") {
      for (const auto& p : lib.playlists())
        std::printf("%lld\t%s%s\t(%d)\n", static_cast<long long>(p.id), p.is_folder ? "[folder] " : "",
                    p.name.c_str(), p.track_count);
    } else if (cmd == "list") {
      const auto rows = lib.tracks(std::atoll(arg_value(argc, argv, "--playlist", "0")),
                                   arg_value(argc, argv, "--search", ""));
      for (const auto& t : rows)
        std::printf("%lld\t%.1f\t%s\t%s - %s\n", static_cast<long long>(t.id), t.bpm, t.key.c_str(),
                    t.artist.c_str(), t.title.c_str());
      std::printf("%zu tracks\n", rows.size());
    } else {
      return usage();
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
