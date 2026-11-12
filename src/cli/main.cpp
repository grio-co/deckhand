// deckhandctl: library tools without the UI.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <chrono>
#include <thread>

#include "deckhand/audio/engine.hpp"
#include "deckhand/import/import.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/log/log.hpp"

using namespace deckhand;

namespace {

std::string home() {
  const char* h = std::getenv("HOME");
  return h ? h : ".";
}
std::string default_data_dir() { return home() + "/Library/Application Support/DeckHand"; }

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
               "  import     [--source DB] [--key-file FILE] [--analysis DIR] [--data DIR]\n"
               "  playlists  [--data DIR]\n"
               "  list       [--playlist ID] [--search TEXT] [--data DIR]\n"
               "  devices                      list audio outputs\n"
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
    Library lib(data + "/library.db");
    if (cmd == "import") {
      ImportOptions o;
      const std::string src_dir = home() + "/Library/Pioneer/rekordbox";
      o.source_db = arg_value(argc, argv, "--source", (src_dir + "/master.db").c_str());
      o.analysis_root = arg_value(argc, argv, "--analysis", (src_dir + "/share").c_str());
      o.key = read_file_trimmed(arg_value(argc, argv, "--key-file", (data + "/source-v6.key").c_str()));
      o.scratch_dir = data + "/scratch";
      const ImportStats s = import_library_v6(lib, o);
      std::printf("tracks added %d, updated %d; playlists %d; cues %d; beatgrids %d\n", s.tracks_added,
                  s.tracks_updated, s.playlists, s.cues, s.beatgrids);
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
