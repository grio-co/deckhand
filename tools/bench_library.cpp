// Library load benchmark: creates a 100k-track library (if the file is missing) and times list/sort/search (best of 7).
// Build: c++ -std=c++20 -O2 -Isrc/library/include -Isrc/log/include -Isrc/core/include tools/bench_library.cpp build/libdh_library.a build/libdh_log.a $(cat build/sqlite_link.txt) -framework CoreFoundation -o bench_library
// Run on a quiet machine: timings swing 2x or more under load.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "deckhand/library/library.hpp"
using namespace deckhand; using C=std::chrono::steady_clock;
static double ms(C::time_point a){return std::chrono::duration<double,std::milli>(C::now()-a).count();}
template<class F> double best(F f){double b=1e9;for(int i=0;i<7;i++){auto t=C::now();f();b=std::min(b,ms(t));}return b;}
int main(int,char**argv){
  const bool fresh = !std::filesystem::exists(argv[1]);
  Library lib(argv[1]);
  if (fresh) { Transaction tx(lib.db());
    for (int i = 0; i < 100000; i++) { NewTrack t; t.path = "/music/d" + std::to_string(i % 500) + "/f" + std::to_string(i) + ".mp3"; t.title = "Title " + std::to_string((i * 7919) % 100000); t.artist = "Artist " + std::to_string((i * 104729) % 3000); t.album = "Album " + std::to_string(i % 4000); t.bpm = 80 + i % 100; t.key = "8A"; lib.upsert_track(t); }
    tx.commit(); }
  printf("ids only (default) %.0f | ids title %.0f | ids search %.0f ms\n",
   best([&]{lib.track_ids(0,"",SortKey::Default);}), best([&]{lib.track_ids(0,"",SortKey::Title);}),
   best([&]{lib.track_ids(0,"title 99",SortKey::Default);}));
  printf("default %.0f | title %.0f | bpm %.0f | artist-desc %.0f | search %.0f | playlist-free-search(x) %.0f ms\n",
   best([&]{lib.tracks(0,"",SortKey::Default);}), best([&]{lib.tracks(0,"",SortKey::Title);}),
   best([&]{lib.tracks(0,"",SortKey::Bpm);}), best([&]{lib.tracks(0,"",SortKey::Artist,true);}),
   best([&]{lib.tracks(0,"title 99",SortKey::Default);}), best([&]{lib.tracks(0,"zzz",SortKey::Default);}));
  // Suggestions: seed = a 120 BPM track; candidates come from the tempo ranges (direct, half and double time).
  {
    const auto seed = lib.track_id_for_path("/music/d20/f20.mp3");  // i = 20 -> 100 BPM; also try a mid-range seed below
    const auto seed2 = lib.track_id_for_path("/music/d40/f40.mp3");   // i = 40 -> 120 BPM
    NextTrackOptions o;
    o.weights = lib.suggest_weights();
    std::size_t n1 = 0, n2 = 0;
    const double a = best([&]{ n1 = lib.suggest(*seed, o).size(); });
    const double b = best([&]{ n2 = lib.suggest(*seed2, o).size(); });
    o.limit = 0;
    std::size_t all = 0;
    const double c = best([&]{ all = lib.suggest(*seed2, o).size(); });
    printf("suggest top20 (100 BPM seed) %.1f ms | (120 BPM seed) %.1f ms | scoring all %zu candidates %.1f ms (%zu / %zu results)\n", a, b, all, c, n1, n2);
  }
}
