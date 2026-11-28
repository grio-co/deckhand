// deckhandctl metadata ...: opt-in tag fill from MusicBrainz. Off until `enable`; `fill` only prints proposals unless --apply 1.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"
#include "deckhand/metadata/metadata.hpp"

using namespace deckhand;

namespace {

const char* opt(int argc, char** argv, const char* name, const char* fallback) {
  for (int i = 2; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], name)) return argv[i + 1];
  return fallback;
}

std::string data_dir(int argc, char** argv) {
  const std::string given = opt(argc, argv, "--data", "");
  if (!given.empty()) return given;
#if defined(_WIN32)
  const char* a = std::getenv("APPDATA");
  return std::string(a ? a : ".") + "\\DeckHand";
#else
  const char* h = std::getenv("HOME");
  return std::string(h ? h : ".") + "/Library/Application Support/DeckHand";
#endif
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  for (std::size_t a = 0; a <= s.size();) {
    const std::size_t b = std::min(s.find(',', a), s.size());
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

int usage() {
  std::fprintf(stderr,
               "usage: deckhandctl metadata <command> [--data DIR]\n"
               "  status                      whether lookup is on, and what would be sent\n"
               "  enable | disable            turn the opt-in lookup on or off (off by default)\n"
               "  fill (--ids 1,2 | --playlist ID | --search TEXT) [--min-confidence 0.8] [--fields album,year,genre] [--overwrite 1]\n"
               "       [--overwrite-edited 1] [--apply 1]   look the selected tracks up; prints proposals, writes only with --apply 1\n"
               "  undo BATCH_ID               undo an earlier --apply\n"
               "Only the artist and title of the selected tracks are sent (to musicbrainz.org). See docs/METADATA.md.\n");
  return 2;
}

}  // namespace

int metadata_command(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string sub = argv[2];
  try {
    Library lib(data_dir(argc, argv) + (
#if defined(_WIN32)
        "\\library.db"
#else
        "/library.db"
#endif
        ));
    if (sub == "status") {
      std::printf("lookup: %s\nbuilt with network support: %s\nuser agent: %s\n", metadata::enabled(lib) ? "on" : "off",
                  metadata::network_built() ? "yes" : "no", metadata::user_agent().c_str());
      return 0;
    }
    if (sub == "enable" || sub == "disable") {
      metadata::set_enabled(lib, sub == "enable");
      std::printf("metadata lookup is now %s\n", sub == "enable" ? "on" : "off");
      return 0;
    }
    if (sub == "undo" && argc >= 4) {
      const auto r = lib.undo_batch(std::atoll(argv[3]));
      std::printf("%s: %d fields restored, %d conflicts\n", r.status == UndoStatus::Ok ? "undone" : "not undone", r.applied, r.conflicts);
      return r.status == UndoStatus::Ok ? 0 : 1;
    }
    if (sub != "fill") return usage();
    if (!metadata::enabled(lib)) {
      std::fprintf(stderr, "metadata lookup is off; run `deckhandctl metadata enable` first (see docs/METADATA.md)\n");
      return 1;
    }
    std::vector<std::int64_t> ids;
    if (*opt(argc, argv, "--ids", "")) {
      for (const auto& s : split(opt(argc, argv, "--ids", ""))) ids.push_back(std::atoll(s.c_str()));
    } else if (*opt(argc, argv, "--playlist", "") || *opt(argc, argv, "--search", "")) {
      ids = lib.track_ids(std::atoll(opt(argc, argv, "--playlist", "0")), opt(argc, argv, "--search", ""));
    } else {
      return usage();
    }
    metadata::FillOptions fo;
    fo.match.min_confidence = std::atof(opt(argc, argv, "--min-confidence", "0.8"));
    fo.overwrite_nonempty = std::atoi(opt(argc, argv, "--overwrite", "0")) != 0;
    fo.overwrite_edited = std::atoi(opt(argc, argv, "--overwrite-edited", "0")) != 0;
    if (*opt(argc, argv, "--fields", "")) {
      fo.fields.clear();
      for (const auto& f : split(opt(argc, argv, "--fields", ""))) fo.fields.insert(f);
    }
    metadata::Service svc(lib);
    std::printf("looking up %zu track(s), about one request per second...\n", ids.size());
    const auto result = svc.propose(ids, fo);
    std::vector<metadata::Proposal> all;
    for (const auto& t : result) {
      std::printf("track %lld: %s%s%s\n", static_cast<long long>(t.track_id), metadata::status_name(t.status),
                  t.message.empty() ? "" : " - ", t.message.c_str());
      for (const auto& p : t.proposals) {
        std::printf("  %-12s %s -> %s  (%.0f%%)%s  %s\n", p.field.c_str(), p.current.empty() ? "(empty)" : p.current.c_str(),
                    p.proposed.c_str(), p.confidence * 100, p.info_only ? " [info only]" : "", p.source_url.c_str());
        all.push_back(p);
      }
    }
    if (std::atoi(opt(argc, argv, "--apply", "0")) != 0) {
      metadata::ApplyOptions ao;
      ao.allow_overwrite = fo.overwrite_nonempty;
      ao.overwrite_edited = fo.overwrite_edited;
      const auto r = svc.apply(all, ao);
      std::printf("applied %d, skipped %d, journal batch %lld (undo with: deckhandctl metadata undo %lld)\n", r.applied, r.skipped,
                  static_cast<long long>(r.batch_id), static_cast<long long>(r.batch_id));
    } else if (!all.empty()) {
      std::printf("nothing written; add --apply 1 to accept every proposal above\n");
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
