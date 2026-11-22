// Engine-process IPC tests: lock-free structures (threads and fork), validation of everything the engine maps or
// receives (fuzzed), and the real deckhand-engine binary driven through RemoteEngine, including a UI killed with
// SIGKILL, an engine killed with SIGKILL, recording across both and stale-segment cleanup.
// On Windows the fork / kill / SIGTERM parts do not exist; platform/windows/tests/ipc_test.cpp covers the Windows IPC layer and
// the engine process (TerminateProcess instead of SIGKILL).
// Build with -fsanitize=address,undefined to check the engine-side parsers for out-of-bounds reads.
#if !defined(_WIN32)
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/remote_engine.hpp"

using namespace deckhand;
using namespace deckhand::ipc;
namespace fs = std::filesystem;

namespace {

std::string g_helper;  // path of deckhand-engine

#pragma GCC diagnostic ignored "-Wformat-security"
void note(const char* fmt, auto... a) {
  char buf[512];
  std::snprintf(buf, sizeof buf, fmt, a...);
  std::printf("%s\n", buf);
  std::fflush(stdout);
}

[[maybe_unused]] double now_s() { return static_cast<double>(now_ns()) / 1e9; }

std::unique_ptr<LoadedTrack> sine_track(double seconds, std::uint32_t sr = 44100, std::int64_t id = 77, const char* path = "/music/test tone.wav") {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = sr;
  audio->frames = static_cast<std::uint64_t>(seconds * sr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i)
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = 0.25f * static_cast<float>(std::sin(2 * M_PI * 220.0 * static_cast<double>(i) / sr));
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->track_id = id;
  t->path = path;
  t->bpm = 120;
  t->grid.push_back({0, 120, 1});
  t->memory_cues = {1000, 5000};
  t->hot_cue[2] = 44100;
  t->cue_point = 0;
  return t;
}

Command cmd(Op op, int deck, float f = 0, int i = 0, double d = 0) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  c.d = d;
  return c;
}

#if defined(_WIN32)
void* shared_anon(std::size_t n) {  // one process only here (the fork test is POSIX-only)
  void* p = std::calloc(1, n);
  CHECK(p != nullptr);
  return p;
}
void shared_free(void* p, std::size_t) { std::free(p); }
#else
void* shared_anon(std::size_t n) {
  void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  CHECK(p != MAP_FAILED);
  return p;
}
void shared_free(void* p, std::size_t n) { munmap(p, n); }
#endif

// ================================================================== lock-free structures
struct Blob {
  std::uint64_t w[24];
};

void test_seqlock_threads() {
  auto* slot = new SeqSlot<Blob>();
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (std::uint64_t n = 1; !stop; ++n) {
      Blob b;
      for (auto& x : b.w) x = n;
      slot->write(b);
    }
  });
  std::atomic<std::uint64_t> torn{0}, reads{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r)
    readers.emplace_back([&] {
      for (int i = 0; i < 300000; ++i) {
        Blob b = slot->read();
        for (const auto x : b.w)
          if (x != b.w[0]) ++torn;
        ++reads;
      }
    });
  for (auto& t : readers) t.join();
  stop = true;
  writer.join();
  note("seqlock: %llu reads, %llu torn", static_cast<unsigned long long>(reads.load()), static_cast<unsigned long long>(torn.load()));
  CHECK(torn == 0);
  delete slot;
}

void test_ring_threads() {
  auto* ring = new CmdRing<1024>();
  ring->reset();
  constexpr int kProducers = 4, kEach = 60000;
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p)
    producers.emplace_back([&, p] {
      for (int n = 0; n < kEach; ++n) {
        WireCommand w;
        w.op = static_cast<std::uint8_t>(p);
        w.d = n;
        while (!ring->push(w)) std::this_thread::yield();
      }
    });
  std::vector<int> next(kProducers, 0);
  int got = 0;
  bool ordered = true;
  while (got < kProducers * kEach) {
    WireCommand w;
    if (!ring->pop(w)) {
      std::this_thread::yield();
      continue;
    }
    if (static_cast<int>(w.d) != next[w.op]++) ordered = false;
    ++got;
  }
  for (auto& t : producers) t.join();
  CHECK(ordered);
  WireCommand w;
  CHECK(!ring->pop(w));
  note("mpsc ring: %d commands from %d producers, per-producer order kept", got, kProducers);
  delete ring;
}

void test_spsc_threads() {
  auto* ev = new SpscShared<WireEvent, 256>();
  auto* st = new StreamShared<1024>();
  std::thread prod([&] {
    for (int n = 0; n < 200000; ++n) {
      WireEvent e;
      e.sched_id = static_cast<std::uint64_t>(n);
      while (!ev->push(e)) std::this_thread::yield();
    }
  });
  std::uint64_t expect = 0;
  while (expect < 200000) {
    WireEvent e;
    if (!ev->pop(e)) continue;
    CHECK(e.sched_id == expect);
    ++expect;
  }
  prod.join();
  // stream ring: frame k holds (k, -k)
  std::thread sp([&] {
    std::uint64_t k = 0;
    float buf[2 * 64];
    while (k < 300000) {
      const std::size_t room = std::min<std::size_t>(st->free_frames(), 64);
      for (std::size_t i = 0; i < room; ++i) {
        buf[2 * i] = static_cast<float>(k + i);
        buf[2 * i + 1] = -static_cast<float>(k + i);
      }
      k += st->push(buf, room);
    }
  });
  std::uint64_t seen = 0;
  float out[2 * 100];
  while (seen < 300000) {
    const std::size_t n = st->pop(out, 100);
    for (std::size_t i = 0; i < n; ++i) {
      CHECK(out[2 * i] == static_cast<float>(seen + i) && out[2 * i + 1] == -static_cast<float>(seen + i));
    }
    seen += n;
  }
  sp.join();
  note("event and stream rings: in order, nothing lost");
  delete ev;
  delete st;
}

#if !defined(_WIN32)
// Same structures, two processes: a child hammers commands and snapshots, the parent checks them.
void test_fork_shared() {
  auto* sh = static_cast<Shared*>(shared_anon(sizeof(Shared)));
  init_shared(*sh);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    for (std::uint64_t n = 1; n <= 200000; ++n) {
      DeckView v;
      v.track_id = static_cast<std::int64_t>(n);
      v.position = static_cast<double>(n);
      v.length = static_cast<double>(n) * 2;
      sh->deck[0].write(v);
      WireCommand w;
      w.d = static_cast<double>(n);
      while (!sh->cmd.push(w)) {}
    }
    _exit(0);
  }
  std::uint64_t popped = 0, last_seen = 0, inconsistent = 0;
  while (popped < 200000) {
    WireCommand w;
    if (sh->cmd.pop(w)) {
      CHECK(static_cast<std::uint64_t>(w.d) == popped + 1);
      ++popped;
    }
    const DeckView v = sh->deck[0].read();
    if (v.position != static_cast<double>(v.track_id) || v.length != 2.0 * v.position) ++inconsistent;
    if (v.track_id == 0) continue;  // the writer was busy for longer than a reader waits: an empty snapshot, never a torn one
    CHECK(static_cast<std::uint64_t>(v.track_id) >= last_seen);
    last_seen = static_cast<std::uint64_t>(v.track_id);
  }
  int st = 0;
  waitpid(child, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(inconsistent == 0);
  note("fork: 200000 commands + snapshots across processes, %llu inconsistent reads", static_cast<unsigned long long>(inconsistent));

  // A producer killed mid-stream may leave a hole; a reset (what Attach does) makes the ring usable again.
  const pid_t killed = fork();
  CHECK(killed >= 0);
  if (killed == 0) {
    for (;;) {
      WireCommand w;
      sh->cmd.push(w);
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  kill(killed, SIGKILL);
  waitpid(killed, &st, 0);
  sh->cmd.reset();
  WireCommand w;
  CHECK(!sh->cmd.pop(w));
  w.d = 42;
  CHECK(sh->cmd.push(w));
  WireCommand r;
  CHECK(sh->cmd.pop(r) && r.d == 42);
  note("fork: ring usable again after a producer was killed mid-stream");
  shared_free(sh, sizeof(Shared));
}

#endif  // !_WIN32

// ================================================================== validation
std::vector<std::uint8_t> block_bytes(const LoadedTrack& t, BlockKind kind = BlockKind::Track) {
  const std::string name = "/dhtest." + std::to_string(current_pid());
  std::string error;
  Shm::remove(name);
  if (kind == BlockKind::Track) CHECK(create_track_block(name, 3, 99, t, error));
  else {
    LoadedSample s;
    s.audio = t.audio;
    s.bpm = 128;
    s.loop = true;
    CHECK(create_sample_block(name, 3, 99, s, error));
  }
  Shm shm;
  CHECK(Shm::open(name, false, shm, error));
  Shm::remove(name);
  const auto* p = static_cast<const std::uint8_t*>(shm.data());
  return std::vector<std::uint8_t>(p, p + static_cast<const BlockHeader*>(shm.data())->total_size);
}

void test_blocks() {
  const auto src = sine_track(2.0);
  // round trip through real shared memory
  const std::string name = "/dhtest.rt." + std::to_string(current_pid());
  std::string error;
  CHECK(create_track_block(name, 5, 1234, *src, error));
  CHECK(map_track_block(name, 6, 1234, error) == nullptr && error == "stale block");  // wrong slot
  CHECK(map_track_block(name, 5, 1235, error) == nullptr && error == "stale block");  // wrong generation
  auto t = map_track_block(name, 5, 1234, error);
  Shm::remove(name);
  CHECK(t);
  CHECK(t->audio->ext_pcm && t->audio->pcm.empty() && t->audio->frames == src->audio->frames);
  CHECK(std::memcmp(t->audio->data(), src->audio->pcm.data(), src->audio->frames * 8) == 0);
  CHECK(t->track_id == 77 && t->path == "/music/test tone.wav" && t->bpm == 120);
  CHECK(t->grid.size() == 1 && t->memory_cues.size() == 2 && t->hot_cue[2] == 44100 && t->hot_cue[0] == -1);
  // the engine plays straight from the mapping
  Engine e;
  e.configure_offline(44100, 2);
  e.load(0, std::move(t));
  e.send(cmd(Op::PlayToggle, 0));
  std::vector<float> out(2 * 4410);
  e.render(out.data(), 4410);
  e.render(out.data(), 4410);
  CHECK(e.deck(0).loaded && e.deck(0).playing && e.deck(0).position > 4000);
  note("block: shared-memory round trip plays (position %.0f)", e.deck(0).position);

  // malformed blocks are rejected, never crash
  const std::vector<std::uint8_t> good = block_bytes(*src);
  ParsedBlock pb;
  CHECK(parse_block(good.data(), good.size(), BlockKind::Track, pb, error));
  CHECK(!parse_block(good.data(), good.size(), BlockKind::Sample, pb, error));
  CHECK(!parse_block(good.data(), good.size() - 1, BlockKind::Track, pb, error));  // shorter than total_size
  CHECK(!parse_block(good.data(), 10, BlockKind::Track, pb, error));
  CHECK(!parse_block(nullptr, 0, BlockKind::Track, pb, error));
  const auto mutate = [&](auto fn) {
    std::vector<std::uint8_t> b = good;
    fn(*reinterpret_cast<BlockHeader*>(b.data()));
    // exact-size heap copy so address-sanitizer sees any read past the end
    std::vector<std::uint8_t> exact(b.begin(), b.end());
    return parse_block(exact.data(), exact.size(), BlockKind::Track, pb, error);
  };
  CHECK(mutate([](BlockHeader&) {}));
  CHECK(!mutate([](BlockHeader& h) { h.magic ^= 1; }));
  CHECK(!mutate([](BlockHeader& h) { h.sealed = 0; }));
  CHECK(!mutate([](BlockHeader& h) { h.frames = ~0ull; }));
  CHECK(!mutate([](BlockHeader& h) { h.frames = h.frames * 2; }));
  CHECK(!mutate([](BlockHeader& h) { h.frames = 0; }));
  CHECK(!mutate([](BlockHeader& h) { h.pcm_offset = h.total_size; }));
  CHECK(!mutate([](BlockHeader& h) { h.pcm_offset = ~0ull - 3; }));
  CHECK(!mutate([](BlockHeader& h) { h.pcm_offset += 4; }));  // misaligned
  CHECK(!mutate([](BlockHeader& h) { h.total_size = ~0ull; }));
  CHECK(!mutate([](BlockHeader& h) { h.sample_rate = 0; }));
  CHECK(!mutate([](BlockHeader& h) { h.sample_rate = 4000000; }));
  CHECK(!mutate([](BlockHeader& h) { h.grid_count = 0xFFFFFFFFu; }));
  CHECK(!mutate([](BlockHeader& h) { h.grid_count = 1000; }));
  CHECK(!mutate([](BlockHeader& h) { h.grid_offset = ~0ull - 7; }));
  CHECK(!mutate([](BlockHeader& h) { h.memcue_count = 5000; }));
  CHECK(!mutate([](BlockHeader& h) { h.path_len = 100000; }));
  CHECK(!mutate([](BlockHeader& h) { h.bpm = std::nan(""); }));
  CHECK(!mutate([](BlockHeader& h) { h.cue_point = INFINITY; }));
  CHECK(!mutate([](BlockHeader& h) { h.hot_cue[3] = std::nan(""); }));
  CHECK(!mutate([](BlockHeader& h) { h.kind = 9; }));
  {  // bad grid content and audio content
    std::vector<std::uint8_t> b = good;
    const auto* h = reinterpret_cast<const BlockHeader*>(b.data());
    auto* g = reinterpret_cast<WireGrid*>(b.data() + h->grid_offset);
    g->bpm = 0;
    CHECK(!parse_block(b.data(), b.size(), BlockKind::Track, pb, error));
    g->bpm = 120;
    g->bar_beat = 0;
    CHECK(!parse_block(b.data(), b.size(), BlockKind::Track, pb, error));
    g->bar_beat = 1;
    CHECK(parse_block(b.data(), b.size(), BlockKind::Track, pb, error));
    const float nan = std::nanf("");
    std::memcpy(b.data() + h->pcm_offset + 4000, &nan, 4);
    CHECK(!parse_block(b.data(), b.size(), BlockKind::Track, pb, error));
    CHECK(parse_block(b.data(), b.size(), BlockKind::Track, pb, error, /*scan_pcm=*/false));
  }

  // fuzz: random flips anywhere (biased to the header), random sizes, pure noise
  std::mt19937_64 rng(12345);
  int accepted = 0;
  const std::vector<std::uint8_t> small = block_bytes(*sine_track(0.05));
  for (int iter = 0; iter < 60000; ++iter) {
    std::vector<std::uint8_t> b = small;
    const int flips = 1 + static_cast<int>(rng() % 8);
    for (int k = 0; k < flips; ++k) {
      const std::size_t at = rng() % 3 == 0 ? rng() % b.size() : rng() % std::min<std::size_t>(b.size(), sizeof(BlockHeader) + 128);
      b[at] = static_cast<std::uint8_t>(rng());
    }
    if (rng() % 4 == 0) b.resize(rng() % (b.size() + 1));
    std::vector<std::uint8_t> exact(b.begin(), b.end());
    ParsedBlock out;
    if (parse_block(exact.data(), exact.size(), BlockKind::Track, out, error, rng() % 2)) {
      ++accepted;
      // whatever got through must lie inside the buffer
      const auto* base = exact.data();
      CHECK(reinterpret_cast<const std::uint8_t*>(out.pcm) >= base);
      CHECK(reinterpret_cast<const std::uint8_t*>(out.pcm) + out.h.frames * 8 <= base + exact.size());
      CHECK(out.grid.size() == out.h.grid_count);
      for (const GridSection& g : out.grid) CHECK(g.bpm > 0 && std::isfinite(g.first_beat_frame));
    }
  }
  for (int iter = 0; iter < 20000; ++iter) {
    std::vector<std::uint8_t> b(rng() % 4096);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    ParsedBlock out;
    CHECK(!parse_block(b.data(), b.size(), BlockKind::Track, out, error));
  }
  note("block fuzz: 80000 malformed blocks, %d survived mutation (all in bounds)", accepted);
}

void test_commands_and_wire() {
  Command c;
  WireCommand w;
  w.op = static_cast<std::uint8_t>(Op::PlayToggle);
  CHECK(validate_command(w, c) && c.op == Op::PlayToggle);
  w.op = static_cast<std::uint8_t>(Op::Load);
  CHECK(!validate_command(w, c));
  w.op = static_cast<std::uint8_t>(Op::SchedInsert);
  CHECK(!validate_command(w, c));
  w.op = static_cast<std::uint8_t>(Op::Count);
  CHECK(!validate_command(w, c));
  w.op = 255;
  CHECK(!validate_command(w, c));
  w = WireCommand{};
  w.op = static_cast<std::uint8_t>(Op::Seek);
  w.d = std::nan("");
  CHECK(!validate_command(w, c));
  w.d = 1e300;
  CHECK(!validate_command(w, c));
  w.d = 100;
  w.deck = 9;
  CHECK(!validate_command(w, c));
  w.deck = 3;
  CHECK(validate_command(w, c));
  w = WireCommand{};
  w.op = static_cast<std::uint8_t>(Op::HotCueDown);
  w.i = 8;
  CHECK(!validate_command(w, c));
  w.i = 7;
  CHECK(validate_command(w, c));
  w = WireCommand{};
  w.op = static_cast<std::uint8_t>(Op::FxEnable);
  w.deck = kFxMaster;
  w.i = 2;
  CHECK(validate_command(w, c));
  w.i = 3;
  CHECK(!validate_command(w, c));

  // Fuzz: whatever validate_command lets through must be safe to run in a real engine.
  Engine e;
  e.configure_offline(44100, 4);
  e.load(0, sine_track(20.0));
  e.load(1, sine_track(20.0, 48000, 78));
  LoadedSample* unused = nullptr;
  (void)unused;
  std::mt19937_64 rng(777);
  std::vector<float> out(4 * 512);
  int accepted = 0, total = 40000;
  const double specials[] = {0, 1, -1, 0.5, 1e9, -1e9, 1e-30, 3.0e38, 44100, 12345.678};
  for (int n = 0; n < total; ++n) {
    WireCommand f;
    f.op = static_cast<std::uint8_t>(rng() % 3 == 0 ? rng() : rng() % (static_cast<int>(Op::Count) + 4));
    f.deck = static_cast<std::uint8_t>(rng() % 3 == 0 ? rng() : rng() % 6);
    f.i = rng() % 4 == 0 ? static_cast<std::int32_t>(rng()) : static_cast<std::int32_t>(rng() % 20) - 2;
    const auto pick = [&]() -> double {
      if (rng() % 5 == 0) {
        std::uint64_t bits = rng();
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
      }
      return specials[rng() % 10] * (rng() % 2 ? 1 : -1);
    };
    f.f = static_cast<float>(pick());
    f.d = pick();
    if (validate_command(f, c)) {
      ++accepted;
      e.send(c);
    }
    if (n % 200 == 199) {
      e.send(cmd(Op::PlayToggle, 0));
      e.send(cmd(Op::PlayToggle, 1));
      e.render(out.data(), 512);
      for (const float x : out) CHECK(std::isfinite(x));
      e.collect_garbage();
      EngineEvent ev;
      while (e.poll(ev)) {}
    }
  }
  note("command fuzz: %d of %d random commands accepted and rendered, output always finite", accepted, total);

  // Reader and writer round trips, plus garbage
  Wr wr;
  wr.u8(7), wr.u32(0xDEADBEEF), wr.str("hello"), wr.f64(2.5), wr.i64(-5);
  Rd rd(wr.bytes().data(), wr.bytes().size());
  CHECK(rd.u8() == 7 && rd.u32() == 0xDEADBEEF && rd.str() == "hello" && rd.f64() == 2.5 && rd.i64() == -5 && rd.ok());
  CHECK(rd.u8() == 0 && !rd.ok());
  for (int iter = 0; iter < 20000; ++iter) {
    std::vector<std::uint8_t> b(rng() % 64);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    Rd r(b.data(), b.size());
    for (int k = 0; k < 8; ++k) {
      r.str();
      r.u64();
      r.u8();
    }
  }

  // The shared segment itself filled with garbage: pops and reads stay inside it.
  auto* sh = static_cast<Shared*>(shared_anon(sizeof(Shared)));
  for (int round = 0; round < 50; ++round) {
    auto* bytes = reinterpret_cast<std::uint8_t*>(sh);
    for (std::size_t i = 0; i < sizeof(Shared); ++i) bytes[i] = static_cast<std::uint8_t>(rng());
    WireCommand wc;
    WireEvent we;
    float buf[2 * 64];
    for (int k = 0; k < 5000; ++k) {
      sh->cmd.pop(wc);
      sh->events.pop(we);
      sh->stream.pop(buf, 64);
    }
    sh->deck[0].read();
    sh->hub.read();
    std::string err;
    CHECK(!check_shared(sh, sizeof(Shared), err));
  }
  shared_free(sh, sizeof(Shared));
  note("garbage segment: rings and snapshots stay in bounds");
}

#if !defined(_WIN32)
// ================================================================== two processes, real engine binary
struct Wav {
  bool valid = false;
  std::uint64_t frames = 0;
};
Wav read_wav(const std::string& path) {
  Wav w;
  std::ifstream f(path, std::ios::binary);
  std::vector<char> head(80);
  if (!f.read(head.data(), 80)) return w;
  const std::uint64_t size = fs::file_size(path);
  if (std::memcmp(head.data(), "RIFF", 4) || std::memcmp(head.data() + 8, "WAVE", 4) || std::memcmp(head.data() + 72, "data", 4)) return w;
  std::uint32_t riff = 0, data = 0;
  std::memcpy(&riff, head.data() + 4, 4);
  std::memcpy(&data, head.data() + 76, 4);
  w.valid = riff == size - 8 && data == size - 80;
  w.frames = data / 6;
  return w;
}

RemoteEngine::Options options_for(const std::string& dir) {
  RemoteEngine::Options o;
  o.data_dir = dir;
  o.helper_path = g_helper;
  o.null_audio = true;
  o.log_level = 2;
  o.backoff_ms = 100;
  return o;
}

bool wait_for(auto pred, double seconds) {
  const double end = now_s() + seconds;
  while (now_s() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

void stop_engine_if_any(const std::string& dir) {
  std::string error;
  RemoteEngine::Options o = options_for(dir);
  o.auto_restart = false;
  RemoteEngine probe(o);
  // attach only if one runs: connect() would start one, which we then immediately stop
  if (probe.connect(error)) probe.shutdown_remote(0);
}

void test_two_process(const std::string& base) {
  const std::string dir = base + "/data1";
  fs::create_directories(dir);
  SessionNames names;
  std::string error;
  CHECK(session_names(dir, names, error));
  CHECK(names.shm_main.size() <= 31 && names.block(63).size() <= 31 && names.socket.size() < 100);

  // stale leftovers from a dead engine and a dead UI must not matter
  {
    Shm junk;
    CHECK(Shm::create(names.shm_main, 4096, junk, error));
    Shm blk;
    CHECK(Shm::create(names.block(5), 4096, blk, error));
    std::ofstream(names.socket) << "stale";
  }

  int to_parent[2];
  CHECK(pipe(to_parent) == 0);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {  // ---- client A: starts the engine, loads, plays, records, then waits to be killed
    close(to_parent[0]);
    RemoteEngine eng(options_for(dir));
    std::string err;
    if (!eng.connect(err)) {
      dprintf(to_parent[1], "fail connect %s\n", err.c_str());
      _exit(1);
    }
    OutputConfig oc;
    if (!eng.start(oc, err)) {
      dprintf(to_parent[1], "fail start %s\n", err.c_str());
      _exit(1);
    }
    eng.load(0, sine_track(300.0));
    if (!wait_for([&] { return eng.deck(0).loaded; }, 10)) {
      dprintf(to_parent[1], "fail load\n");
      _exit(1);
    }
    eng.send(cmd(Op::MasterLevel, 0, 0.0f));
    eng.send(cmd(Op::PlayToggle, 0));
    if (!eng.record_open(base + "/rec1.wav", false, err)) {
      dprintf(to_parent[1], "fail rec %s\n", err.c_str());
      _exit(1);
    }
    const unsigned long long rec_clock = eng.clock_frames();
    wait_for([&] { return eng.deck(0).position > 22050; }, 5);
    const DeckView v = eng.deck(0);
    dprintf(to_parent[1], "ready %.3f %llu %llu %u %llu\n", v.position, static_cast<unsigned long long>(eng.clock_frames()),
            static_cast<unsigned long long>(now_ns()), eng.engine_pid(), rec_clock);
    for (;;) pause();
  }
  close(to_parent[1]);
  char line[256] = {};
  CHECK(read(to_parent[0], line, sizeof line - 1) > 0);
  CHECK(std::strncmp(line, "ready", 5) == 0);
  double pos0 = 0;
  unsigned long long clock0 = 0, ns0 = 0, rec_clock = 0;
  unsigned epid = 0;
  CHECK(std::sscanf(line, "ready %lf %llu %llu %u %llu", &pos0, &clock0, &ns0, &epid, &rec_clock) == 5);
  note("client A: engine pid %u, deck A position %.0f frames, clock %llu", epid, pos0, clock0);
  CHECK(process_alive(epid));
  // stale objects were collected by the engine at start-up
  {
    Shm probe;
    std::string e2;
    CHECK(!Shm::open(names.block(5), false, probe, e2));
  }

  kill(child, SIGKILL);  // the UI dies without any goodbye
  int st = 0;
  waitpid(child, &st, 0);
  CHECK(WIFSIGNALED(st));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(process_alive(epid));  // the engine does not care

  // ---- client B: a brand new process attaches to the running engine
  RemoteEngine eng(options_for(dir));
  std::vector<RemoteEngine::Status> statuses;
  std::mutex mu;
  eng.set_status_callback([&](RemoteEngine::Status s, const std::string&) {
    std::lock_guard<std::mutex> lock(mu);
    statuses.push_back(s);
  });
  CHECK(eng.connect(error));
  CHECK(eng.attached_existing() && eng.engine_pid() == epid);
  const DeckView v = eng.deck(0);
  const unsigned long long ns1 = now_ns(), clock1 = eng.clock_frames();
  const double gap = static_cast<double>(ns1 - ns0) / 1e9;
  const double advanced = (v.position - pos0) / v.sample_rate, clocked = static_cast<double>(clock1 - clock0) / eng.sample_rate();
  note("client B after kill -9 of A: gap %.3f s, deck advanced %.3f s (%.0f frames), engine clock advanced %.3f s", gap, advanced,
       v.position - pos0, clocked);
  CHECK(v.loaded && v.playing && v.track_id == 77);
  CHECK(std::fabs(advanced - gap) < 0.12);   // sample-count continuity: it played on through the gap
  CHECK(std::fabs(clocked - gap) < 0.12);
  std::int64_t id = 0;
  std::string path;
  CHECK(eng.deck_info(0, id, path) && id == 77 && path == "/music/test tone.wav");
  CHECK(eng.running() && eng.sample_rate() > 0 && eng.output_channels() == 2);
  note("state recovered: deck A track %lld, %s", static_cast<long long>(id), path.c_str());

  // recording survived the UI's death
  RecorderStatus rs = eng.recorder_status();
  CHECK(rs.recording && eng.recorder_path() == base + "/rec1.wav");
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const unsigned long long clock_stop = eng.clock_frames();
  eng.record_stop();
  eng.record_close();
  const Wav w = read_wav(base + "/rec1.wav");
  const double expect_frames = static_cast<double>(clock_stop - rec_clock);
  note("recording across the UI kill: valid WAV=%d, %llu frames (engine clock span since the recording started %.0f)", w.valid ? 1 : 0,
       static_cast<unsigned long long>(w.frames), expect_frames);
  CHECK(w.valid);
  CHECK(std::fabs(static_cast<double>(w.frames) - expect_frames) < 0.15 * eng.sample_rate());

  // the new UI can drive the old engine's decks
  eng.send(cmd(Op::Seek, 0, 0, 0, 1000));
  CHECK(wait_for([&] { return std::fabs(eng.deck(0).position - 1000) < 20000 && eng.deck(0).position < 30000; }, 2));
  CHECK(eng.devices().size() == 1);
  // commands the engine must refuse
  Command bad;
  bad.op = Op::HotCueDown;
  bad.i = 99;
  eng.send(bad);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // ---- the engine dies: the client notices, restarts it, decks are empty, audio can be opened again
  const std::uint32_t old_pid = eng.engine_pid();
  kill(static_cast<pid_t>(old_pid), SIGKILL);
  CHECK(wait_for([&] {
    std::lock_guard<std::mutex> lock(mu);
    return std::find(statuses.begin(), statuses.end(), RemoteEngine::Status::Restarted) != statuses.end();
  }, 8));
  CHECK(eng.engine_pid() != old_pid && process_alive(eng.engine_pid()));
  {
    std::lock_guard<std::mutex> lock(mu);
    CHECK(std::find(statuses.begin(), statuses.end(), RemoteEngine::Status::Died) != statuses.end());
  }
  CHECK(!eng.deck(0).loaded && !eng.running());
  OutputConfig oc;
  CHECK(eng.start(oc, error));
  eng.load(1, sine_track(10.0, 48000, 5, "/x.wav"));
  CHECK(wait_for([&] { return eng.deck(1).loaded; }, 5));
  eng.send(cmd(Op::MasterLevel, 0, 0.0f));
  eng.send(cmd(Op::PlayToggle, 1));
  CHECK(wait_for([&] { return eng.deck(1).position > 4800; }, 5));
  note("engine killed: detected, restarted as pid %u, decks empty, new load plays", eng.engine_pid());

  // a second recording, finalised by a clean shutdown
  CHECK(eng.record_open(base + "/rec2.wav", false, error));
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  eng.shutdown_remote(50);
  CHECK(wait_for([&] {
    Shm probe;
    std::string e2;
    return !Shm::open(names.shm_main, false, probe, e2);
  }, 3));
  const Wav w2 = read_wav(base + "/rec2.wav");
  CHECK(w2.valid && w2.frames > 8000);
  CHECK(!fs::exists(names.socket));
  for (int i = 0; i < kBlockSlots; ++i) {
    Shm probe;
    std::string e2;
    CHECK(!Shm::open(names.block(i), false, probe, e2));
  }
  InstanceLock again;
  CHECK(again.acquire(names.lock, error));  // the lock was released
  again.release_and_remove();
  note("clean shutdown: recording finalised (%llu frames), shm, socket and lock released", static_cast<unsigned long long>(w2.frames));
}

void test_sigterm_and_backoff(const std::string& base) {
  const std::string dir = base + "/data2";
  fs::create_directories(dir);
  SessionNames names;
  std::string error;
  CHECK(session_names(dir, names, error));
  {
    RemoteEngine eng(options_for(dir));
    CHECK(eng.connect(error));
    OutputConfig oc;
    CHECK(eng.start(oc, error));
    eng.send(cmd(Op::MasterLevel, 0, 0.0f));
    eng.load(0, sine_track(5.0));
    CHECK(wait_for([&] { return eng.deck(0).loaded; }, 5));
    eng.send(cmd(Op::PlayToggle, 0));
    CHECK(eng.record_open(base + "/rec3.wav", false, error));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const std::uint32_t pid = eng.engine_pid();
    eng.detach();
    kill(static_cast<pid_t>(pid), SIGTERM);
    CHECK(wait_for([&] { return reap_child(pid) || !process_alive(pid); }, 5));
  }
  const Wav w = read_wav(base + "/rec3.wav");
  note("SIGTERM during recording: WAV valid=%d, %llu frames", w.valid ? 1 : 0, static_cast<unsigned long long>(w.frames));
  CHECK(w.valid && w.frames > 10000);
  Shm probe;
  CHECK(!Shm::open(names.shm_main, false, probe, error));

  // no crash loop: repeatedly killed, the supervisor backs off and finally gives up
  RemoteEngine::Options o = options_for(dir);
  RemoteEngine eng(o);
  std::atomic<int> died{0}, restarted{0}, gave_up{0};
  eng.set_status_callback([&](RemoteEngine::Status s, const std::string&) {
    if (s == RemoteEngine::Status::Died) ++died;
    if (s == RemoteEngine::Status::Restarted) ++restarted;
    if (s == RemoteEngine::Status::GaveUp) ++gave_up;
  });
  CHECK(eng.connect(error));
  const double t0 = now_s();
  for (int round = 0; round < 10 && gave_up == 0; ++round) {
    const std::uint32_t pid = eng.engine_pid();
    if (pid && process_alive(pid)) kill(static_cast<pid_t>(pid), SIGKILL);
    wait_for([&] { return gave_up > 0 || (eng.connected() && eng.engine_pid() != pid); }, 8);
  }
  note("crash loop: %d deaths, %d restarts, then gave up (%d) after %.1f s", died.load(), restarted.load(), gave_up.load(), now_s() - t0);
  CHECK(gave_up == 1 && died >= 5 && restarted >= 3 && restarted < died);
  CHECK(!eng.connected());
  stop_engine_if_any(dir);
}

void test_second_instance(const std::string& base) {
  const std::string dir = base + "/data3";
  fs::create_directories(dir);
  std::string error;
  int ready[2];
  CHECK(pipe(ready) == 0);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {  // the first UI, in a process of its own
    RemoteEngine a(options_for(dir));
    std::string err;
    if (!a.connect(err)) _exit(1);
    a.heartbeat();
    dprintf(ready[1], "ready\n");
    for (;;) {
      a.heartbeat();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  char line[16] = {};
  CHECK(read(ready[0], line, sizeof line - 1) > 0);
  // a second helper for the same folder exits at once on the instance lock
  std::uint32_t pid = 0;
  CHECK(spawn_detached(g_helper, {"--data-dir", dir, "--null-audio", "--no-midi"}, "", pid, error));
  CHECK(wait_for([&] { return reap_child(pid); }, 5));
  // another UI cannot steal an engine whose UI is alive...
  RemoteEngine b(options_for(dir));
  CHECK(!b.connect(error));
  note("second UI refused while the first is alive: %s", error.c_str());
  // ...but attaches once the first one is gone
  kill(child, SIGKILL);
  int st = 0;
  waitpid(child, &st, 0);
  CHECK(b.connect(error) && b.attached_existing());
  // and a third attach supersedes the second: its commands are ignored from then on
  b.send(cmd(Op::MasterLevel, 0, 0.0f));
  OutputConfig oc;
  CHECK(b.start(oc, error));
  b.load(0, sine_track(5.0));
  CHECK(wait_for([&] { return b.deck(0).loaded; }, 5));
  RemoteEngine c(options_for(dir));
  CHECK(c.connect(error));
  b.send(cmd(Op::PlayToggle, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK(!c.deck(0).playing);
  c.send(cmd(Op::PlayToggle, 0));
  CHECK(wait_for([&] { return c.deck(0).playing; }, 2));
  note("a superseded UI's commands are ignored; the new one drives the same engine");
  b.detach();
  c.shutdown_remote(0);
}

#endif  // !_WIN32

}  // namespace

int main(int, char** argv) {
  fs::path exe = fs::absolute(argv[0]).parent_path();
  g_helper = (exe / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) g_helper = h;

  test_seqlock_threads();
  test_ring_threads();
  test_spsc_threads();
#if !defined(_WIN32)
  test_fork_shared();
#endif
  test_blocks();
  test_commands_and_wire();
#if defined(_WIN32)
  std::printf("engine ipc ok (portable parts; the fork and kill tests are POSIX-only, see platform/windows/tests/ipc_test.cpp)\n");
  return 0;
#else

  if (!fs::exists(g_helper)) {
    std::printf("deckhand-engine not built (%s): skipping the two-process tests\n", g_helper.c_str());
    std::printf("engine ipc ok (without the engine process)\n");
    return 0;
  }
  const std::string base = (fs::temp_directory_path() / ("dh-ipc-test-" + std::to_string(current_pid()))).string();
  fs::remove_all(base);
  fs::create_directories(base);
  test_two_process(base);
  test_sigterm_and_backoff(base);
  test_second_instance(base);
  fs::remove_all(base);
  std::printf("engine ipc ok\n");
  return 0;
#endif
}
