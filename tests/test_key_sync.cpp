// Key sync: key arithmetic (all 24 keys x tempo offsets), the engine's one-shot and follow modes measured on synthetic
// tones, hysteresis, 16 hot cue slots, the wire checks and parity with the engine process.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#include "check.hpp"
#include "deckhand/analysis/analysis.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/remote_engine.hpp"
#include "deckhand/core/key_math.hpp"

using namespace deckhand;
namespace km = deckhand::keymath;
namespace fs = std::filesystem;

namespace {

const double kPi = 3.14159265358979323846;
constexpr int kSr = 44100;

// ---- key math ---------------------------------------------------------------------------------------------
void test_math() {
  for (int k = 0; k < 24; ++k) {  // every spelling the library may store parses back to the same key
    CHECK(km::parse(km::camelot(k)) == k);
    CHECK(km::parse(km::open_key(k)) == k);
    CHECK(km::parse(key_name(k)) == k);
  }
  CHECK(km::parse("Db") == km::make(1, false) && km::parse("F# minor") == km::make(6, true) && km::parse("08A") == km::parse("8A"));
  CHECK(km::parse("") == -1 && km::parse("H") == -1 && km::parse("13A") == -1 && km::parse("Cx") == -1);
  // notation agrees with the analysis module for every key
  for (int k = 0; k < 24; ++k) {
    CHECK(km::camelot(k) == key_wheel(k));
    CHECK(km::relative(km::relative(k)) == k);
    CHECK(km::wheel_number(km::relative(k)) == km::wheel_number(k));
    CHECK(km::is_minor(km::relative(k)) != km::is_minor(k));
    CHECK(km::wheel_step(km::wheel_step(k, 1), -1) == k);
    CHECK(km::wheel_number(km::wheel_step(k, 1)) == km::wheel_number(k) % 12 + 1);  // +1 = next number
    CHECK(km::wheel_step(k, 12) == k);
  }
  CHECK(km::camelot(0) == "8B" && km::camelot(21) == "8A" && km::open_key(0) == "1d" && km::open_key(21) == "1m");
  CHECK(km::camelot(-1).empty() && km::camelot(24).empty() && km::relative(-1) == -1);
  CHECK(std::fabs(km::tempo_to_semitones(2.0) - 12.0) < 1e-12 && km::tempo_to_semitones(1.0) == 0.0);

  // every own key x every reference key x tempo-induced pitch of the reference x mode x resolution
  long solved = 0;
  for (int own = 0; own < 24; ++own)
    for (int ref = 0; ref < 24; ++ref)
      for (int t = -10; t <= 10; ++t) {  // tempo offsets of -10 % .. +10 % in 1 % steps
        const double ref_pitch = km::tempo_to_semitones(1.0 + t / 100.0);
        for (int mode = 0; mode < 2; ++mode)
          for (int fine = 0; fine < 2; ++fine) {
            km::SyncParams p;
            p.own_key = own;
            p.ref_key = ref;
            p.ref_pitch = ref_pitch;
            p.mode = mode ? km::SyncMode::Compatible : km::SyncMode::Match;
            p.fine = fine != 0;
            p.limit = 12;
            const km::SyncSolution s = km::solve_sync(p);
            CHECK(s.result == km::SyncResult::Ok);
            CHECK(s.shift >= -6.0 && s.shift <= 6.0);
            CHECK(km::is_minor(s.target_key) == km::is_minor(own));
            // where the own key lands, against the reference sounding key (on the own ring)
            const double landed = km::tonic(own) + s.shift;
            const int base = km::is_minor(own) == km::is_minor(ref) ? ref : km::relative(ref);
            const double want_tonic = km::tonic(base) + ref_pitch;
            const double miss = km::wrap_semitones(landed - want_tonic);  // 0 = same key, +-7 = a wheel step
            const double tol = fine ? 0.006 : 0.5 + 1e-9;
            if (!mode) {
              CHECK(std::fabs(miss) <= tol);
              CHECK(s.candidate == 0);
            } else {
              // one of same / +1 / -1 (only the ring of the reference's own mode has the neighbours), and the smallest shift
              double best = 1e9;
              const int ncand = km::is_minor(own) == km::is_minor(ref) ? 3 : 1;
              const double off[3] = {0, 7, -7};
              for (int c = 0; c < ncand; ++c) best = std::min(best, std::fabs(km::wrap_semitones(want_tonic + off[c] - km::tonic(own))));
              CHECK(std::fabs(s.shift) <= best + (fine ? 0.006 : 0.5 + 1e-9));
              CHECK(std::fabs(s.raw) <= best + 1e-9);
            }
            if (fine) CHECK(std::fabs(s.shift * 100 - std::round(s.shift * 100)) < 1e-6);
            else CHECK(s.shift == std::round(s.shift));
            ++solved;
          }
      }
  CHECK(solved == 24L * 24 * 21 * 4);

  // limits: beyond the limit nothing is applied and the result says so
  km::SyncParams p;
  p.own_key = 2;   // D major
  p.ref_key = 21;  // A minor: relative C major, two semitones down
  p.limit = 2;
  CHECK(km::solve_sync(p).result == km::SyncResult::Ok && km::solve_sync(p).shift == -2);
  p.limit = 1.99;
  CHECK(km::solve_sync(p).result == km::SyncResult::TooFar);
  p.limit = 0;
  CHECK(km::solve_sync(p).result == km::SyncResult::TooFar);
  p.own_key = 7;  // G major against C major (the reference's relative): five semitones, but one step on the wheel
  p.ref_key = 0;
  p.limit = 2;
  CHECK(km::solve_sync(p).result == km::SyncResult::TooFar);
  p.mode = km::SyncMode::Compatible;
  CHECK(km::solve_sync(p).result == km::SyncResult::Ok && km::solve_sync(p).shift == 0 && km::solve_sync(p).candidate == 1);
  p.own_key = -1;
  CHECK(km::solve_sync(p).result == km::SyncResult::NoOwnKey);
  p.own_key = 3;
  p.ref_key = 99;
  CHECK(km::solve_sync(p).result == km::SyncResult::NoRefKey);

  // hysteresis: a reference whose pitch wobbles around a rounding boundary never changes a held shift
  {
    km::SyncParams q;
    q.own_key = 2;
    q.ref_key = 21;
    q.limit = 6;
    double held = -1;
    int changes = 0;
    for (int i = 0; i < 2000; ++i) {
      q.ref_pitch = 0.5 + 0.1 * std::sin(i * 0.37);  // raw shift swings between -1.6 and -1.4
      q.prefer = 0;
      const km::SyncSolution s = km::solve_sync(q);
      if (!km::hold_current(held, s, false)) {
        held = s.shift;
        ++changes;
      }
    }
    CHECK(changes == 0);
    // a real move does change it
    q.ref_pitch = 0;
    CHECK(!km::hold_current(held, km::solve_sync(q), false));
    // candidate preference: two equally near neighbours do not trade places on noise
    km::SyncParams c;
    c.own_key = 7;
    c.ref_key = 0;
    c.mode = km::SyncMode::Compatible;
    c.limit = 12;
    c.ref_pitch = 0.0;
    // G major against C major, reference pitch near +1: one step up (raw +1) and one step down (raw -1) are equally near
    for (const double mid : {0.95, 1.05}) {
      c.prefer = -1;
      c.ref_pitch = mid;
      const int first = km::solve_sync(c).candidate;
      CHECK(first == 1 || first == 2);
      c.prefer = first;
      for (double e = -0.1; e <= 0.1001; e += 0.01) {
        c.ref_pitch = mid + e;
        CHECK(km::solve_sync(c).candidate == first);
      }
    }
  }
}

// ---- engine -----------------------------------------------------------------------------------------------
std::unique_ptr<LoadedTrack> tone(double hz, int key, double seconds = 90) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = kSr;
  audio->frames = static_cast<std::uint64_t>(seconds * kSr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) {
    const float v = 0.4f * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = v;
  }
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->bpm = 120;
  t->grid.push_back({0, 120, 1});
  t->key_index = key;
  return t;
}

Command cmd(Op op, int deck, float f = 0, int i = 0) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.i = i;
  return c;
}

// Renders and returns the left master channel.
std::vector<float> run(Engine& e, double seconds) {
  const std::uint32_t frames = static_cast<std::uint32_t>(seconds * kSr);
  std::vector<float> out(static_cast<std::size_t>(frames) * 4), left;
  left.reserve(frames);
  std::uint32_t done = 0;
  while (done < frames) {
    const std::uint32_t n = std::min<std::uint32_t>(256, frames - done);
    e.render(out.data() + static_cast<std::size_t>(done) * 4, n);
    done += n;
  }
  for (std::uint32_t i = 0; i < frames; ++i) left.push_back(out[static_cast<std::size_t>(i) * 4]);
  return left;
}

// Frequency from rising zero crossings (linear interpolation), over the second half of the buffer.
double freq_of(const std::vector<float>& x) {
  const std::size_t from = x.size() / 2;
  double first = -1, last = -1;
  int n = 0;
  for (std::size_t i = from + 1; i < x.size(); ++i)
    if (x[i - 1] < 0 && x[i] >= 0) {
      const double t = static_cast<double>(i - 1) + -x[i - 1] / (x[i] - x[i - 1]);
      if (first < 0) first = t;
      last = t;
      ++n;
    }
  return n > 2 ? (n - 1) * kSr / (last - first) : 0;
}

double semis(double hz) { return 12.0 * std::log2(hz / 440.0) + 9.0; }  // pitch class scale: A = 9

// Plays deck `d` alone (the other fader down) and measures what it sounds like.
double sounding(Engine& e, int d) {
  e.send(cmd(Op::Fader, d, 1.0f));
  e.send(cmd(Op::Fader, d ^ 1, 0.0f));
  run(e, 0.6);
  const double f = freq_of(run(e, 0.8));
  e.send(cmd(Op::Fader, d ^ 1, 1.0f));
  return f;
}

struct Rig {
  Engine e;
  Rig(double ref_hz, int ref_key, double own_hz, int own_key) {
    e.configure_offline(kSr, 4);
    e.load(0, tone(ref_hz, ref_key));
    e.load(1, tone(own_hz, own_key));
    run(e, 0.05);
    e.send(cmd(Op::Crossfader, 0, 0.5f));
    e.send(cmd(Op::PlayToggle, 0));
    e.send(cmd(Op::PlayToggle, 1));
    run(e, 0.1);
  }
};

void test_engine() {
  // 1. whole-semitone match at equal tempo: D major (293.66 Hz) next to A minor (440 Hz) goes down two semitones to C major
  {
    Rig r(440.0, 21, 293.66, 2);
    CHECK(r.e.deck(1).key_index == 2 && r.e.deck(0).key_index == 21);
    CHECK(std::fabs(sounding(r.e, 1) - 293.66) < 3);
    r.e.send(cmd(Op::Tempo, 1, 0.5f));  // the own tempo is irrelevant: key lock keeps the pitch
    r.e.key_match(1);
    run(r.e, 0.05);
    const DeckView v = r.e.deck(1);
    CHECK(v.key_sync_result == 1 && v.key_shift == -2.0f && v.key_lock && v.key_sync_target == 0);
    CHECK(v.key_sync_follow == 0);
    const double f = sounding(r.e, 1);
    CHECK(std::fabs(f - 261.63) < 2.7);  // C4 within 1 %
    CHECK(std::fabs(semis(f) - (semis(440.0) + 3.0 - 12.0)) < 0.2);  // the relative major of the reference
    // reset gives the key lock back too
    r.e.key_reset(1);
    run(r.e, 0.05);
    const DeckView w = r.e.deck(1);
    CHECK(w.key_shift == 0 && !w.key_lock && w.key_sync_result == 0 && w.key_sync_target == -1);
    CHECK(std::fabs(sounding(r.e, 1) - 293.66 * 1.03) < 6);  // tempo +3 % (half of 6 %), no key lock: the pitch follows
  }

  // 2. the reference sounds sharp because its tempo is up with key lock off: +6 % is +1.009 semitones
  {
    Rig r(440.0, 21, 293.66, 2);
    r.e.send(cmd(Op::Tempo, 0, 1.0f));  // deck 1 (index 0) +6 %
    run(r.e, 0.3);
    const double f0 = sounding(r.e, 0);
    CHECK(std::fabs(f0 - 440 * 1.06) < 5);
    CHECK(std::fabs(r.e.deck(0).key_pitch - 1.0088f) < 0.005f);
    r.e.key_match(1);  // whole semitones: -1 (raw -0.99)
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_result == 1 && r.e.deck(1).key_shift == -1.0f);
    const double m1 = semis(sounding(r.e, 1));
    CHECK(std::fabs(km::wrap_semitones(m1 - (semis(f0) + 3.0))) < 0.05);  // within the 0.01 the rounding leaves
    // cents: lands exactly
    r.e.key_sync_config(1, 0, true, 2.0);
    r.e.key_match(1);
    run(r.e, 0.05);
    const DeckView v = r.e.deck(1);
    CHECK(v.key_sync_fine == 1 && std::fabs(v.key_shift - (-0.99f)) < 0.011f);
    const double m2 = semis(sounding(r.e, 1));
    CHECK(std::fabs(km::wrap_semitones(m2 - (semis(f0) + 3.0))) < 0.06);
    // key lock on the reference: only its shift counts, not its tempo
    r.e.send(cmd(Op::KeyLock, 0, 0, 1));
    r.e.send(cmd(Op::KeyShift, 0, 1.0f));
    run(r.e, 0.05);
    r.e.key_match(1);
    run(r.e, 0.05);
    CHECK(std::fabs(r.e.deck(0).key_pitch - 1.0f) < 1e-6f);
    CHECK(std::fabs(r.e.deck(1).key_shift - (-1.0f)) < 0.011f);
  }

  // 3. too far, compatible mode, unknown keys
  {
    Rig r(261.63, 0, 392.0, 7);  // G major against C major: five semitones away, one step on the wheel
    r.e.key_sync_config(1, 0, false, 2.0);
    r.e.key_match(1);
    run(r.e, 0.05);
    DeckView v = r.e.deck(1);
    CHECK(v.key_sync_result == 2 && v.key_shift == 0 && !v.key_lock && v.key_sync_target == -1);  // nothing was done
    r.e.key_sync_config(1, 1, false, 2.0);  // compatible: G is one step from C on the wheel
    r.e.key_match(1);
    run(r.e, 0.05);
    v = r.e.deck(1);
    CHECK(v.key_sync_result == 1 && v.key_shift == 0 && v.key_sync_target == 7);
    r.e.set_track_key(1, -1);
    r.e.key_match(1);
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_result == 3 && r.e.deck(1).key_index == -1);
    r.e.set_track_key(1, 7);
    r.e.set_track_key(0, -1);
    r.e.key_match(1);
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_result == 4);
  }

  // 4. follow: the shift tracks the reference's tempo with hysteresis; a hand on the shift ends it
  {
    Rig r(440.0, 21, 293.66, 2);
    r.e.key_follow(1, 1);
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_follow == 1 && r.e.deck(1).key_shift == -2.0f);
    std::vector<float> seen;
    auto sweep = [&](double from, double to) {
      const int steps = 60;
      for (int i = 0; i <= steps; ++i) {
        const double x = from + (to - from) * i / steps;
        r.e.send(cmd(Op::Tempo, 0, static_cast<float>(x)));
        run(r.e, 0.02);
        const float s = r.e.deck(1).key_shift;
        if (seen.empty() || seen.back() != s) seen.push_back(s);
      }
    };
    sweep(0.0, 1.0);  // 0 .. +6 %: the reference's pitch rises 0 .. 1.01 semitones, the raw shift goes -2 .. -0.99
    CHECK(seen.size() >= 1 && seen.size() <= 3);  // -2, then -1: no flutter in between
    CHECK(seen.back() == -1.0f);
    const std::size_t n1 = seen.size();
    sweep(1.0, 0.7);  // wobble inside the band: no change
    CHECK(seen.size() == n1);
    sweep(0.7, 0.0);
    CHECK(seen.back() == -2.0f && seen.size() <= n1 + 2);
    // two decks following each other: the lower one leads
    r.e.key_follow(0, 1);
    run(r.e, 0.05);
    CHECK(r.e.deck(0).key_sync_result == 5);
    CHECK(r.e.deck(1).key_sync_result == 1);
    r.e.key_follow(0, 0);
    // a manual shift ends following
    r.e.send(cmd(Op::KeyShift, 1, 3.0f));
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_follow == 0 && r.e.deck(1).key_shift == 3.0f);
    // toggling and the reset
    r.e.key_follow(1, -1);
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_follow == 1);
    r.e.key_follow(1, -1);
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_follow == 0);
    r.e.key_follow(1, 1);
    r.e.key_reset(1);
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_follow == 0 && r.e.deck(1).key_shift == 0 && !r.e.deck(1).key_lock);
    // switching key lock off by hand while following ends it
    r.e.key_follow(1, 1);
    r.e.send(cmd(Op::KeyLock, 1, 0, 0));
    run(r.e, 0.05);
    CHECK(r.e.deck(1).key_sync_follow == 0);
  }

  // 5. sixteen hot cue slots, set, jump, delete
  {
    Engine e;
    e.configure_offline(kSr, 4);
    e.load(0, tone(220, 0, 60));
    run(e, 0.05);
    for (int i = 0; i < kHotCues; ++i) {
      Command s = cmd(Op::HotCueSetAt, 0, 0, i);
      s.d = (i + 1) * 2.0 * kSr;
      e.send(s);
    }
    run(e, 0.02);
    static_assert(kHotCues >= 16);
    const DeckView v = e.deck(0);
    for (int i = 0; i < 16; ++i) CHECK(std::fabs(v.hot_cue[i] - (i + 1) * 2.0 * kSr) < 64);
    e.send(cmd(Op::HotCueDown, 0, 0, 15));  // an existing slot jumps
    run(e, 0.02);
    CHECK(std::fabs(e.deck(0).position - 32.0 * kSr) < 2000);
    e.send(cmd(Op::HotCueUp, 0, 0, 15));
    run(e, 0.02);
    e.send(cmd(Op::HotCueClear, 0, 0, 15));
    run(e, 0.02);
    CHECK(e.deck(0).hot_cue[15] < 0 && e.deck(0).hot_cue[14] > 0);
    // loaded with 16 cues
    auto t = tone(220, 0, 60);
    for (int i = 0; i < 16; ++i) t->hot_cue[i] = (i + 1) * kSr;
    t->hot_loop_end[12] = 20.0 * kSr;
    e.load(1, std::move(t));
    run(e, 0.05);
    for (int i = 0; i < 16; ++i) CHECK(e.deck(1).hot_cue[i] == (i + 1) * kSr);
    CHECK(e.deck(1).hot_cue_end[12] == 20.0 * kSr && e.deck(1).hot_cue_end[11] < 0);
  }
}

// ---- wire -------------------------------------------------------------------------------------------------
void test_wire() {
  using namespace ipc;
  CHECK(kProtocolVersion >= 7);
  auto ok = [](Op op, int deck, int i, float f) {
    WireCommand w{};
    w.op = static_cast<std::uint8_t>(op);
    w.deck = static_cast<std::uint8_t>(deck);
    w.i = i;
    w.f = f;
    Command c;
    return validate_command(w, c);
  };
  CHECK(ok(Op::TrackKey, 0, 23, 0) && ok(Op::TrackKey, 3, -1, 0) && !ok(Op::TrackKey, 0, 24, 0) && !ok(Op::TrackKey, 0, -2, 0));
  CHECK(ok(Op::KeyMatch, 1, 0, 0) && ok(Op::KeyReset, 1, 0, 0) && !ok(Op::KeyMatch, kMaxDecks, 0, 0));
  CHECK(ok(Op::KeyFollow, 1, 1, 0) && ok(Op::KeyFollow, 1, -1, 0) && !ok(Op::KeyFollow, 1, 2, 0));
  CHECK(ok(Op::KeySyncConfig, 1, 0x101, 12.0f) && ok(Op::KeySyncConfig, 1, 0, 0.0f));
  CHECK(!ok(Op::KeySyncConfig, 1, 2, 2.0f) && !ok(Op::KeySyncConfig, 1, 0, 13.0f) && !ok(Op::KeySyncConfig, 1, 0, -1.0f));
  CHECK(ok(Op::HotCueDown, 0, 15, 0) && !ok(Op::HotCueDown, 0, 16, 0) && ok(Op::HotCueSetAt, 0, 15, 0));
}

// ---- parity with the engine process --------------------------------------------------------------------
template <class P>
bool wait_for(P pred) {
  for (int i = 0; i < 12000; ++i) {  // up to a minute: only a hung helper stops here
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

void test_remote(const std::string& helper) {
  if (!fs::exists(helper)) {
    std::printf("engine process not built, remote parity skipped\n");
    return;
  }
  const fs::path dir = fs::temp_directory_path() / ("dh_keysync_" + std::to_string(std::rand()));
  fs::create_directories(dir);
  RemoteEngine::Options o;
  o.data_dir = dir.string();
  o.helper_path = helper;
  o.null_audio = true;
  o.auto_restart = false;
  RemoteEngine re(o);
  std::string err;
  CHECK(re.connect(err));
  OutputConfig oc;
  oc.null_backend = true;
  CHECK(re.start(oc, err));

  Engine local;
  local.configure_offline(kSr, 4);
  auto make = [](int key) {
    auto t = tone(220, key, 10);
    for (int i = 0; i < 16; ++i) t->hot_cue[i] = (i + 1) * 0.5 * kSr;
    t->hot_loop_end[15] = 9.0 * kSr;
    return t;
  };
  for (IEngine* e : {static_cast<IEngine*>(&re), static_cast<IEngine*>(&local)}) {
    e->load(0, make(21));
    e->load(1, make(2));
  }
  run(local, 0.05);
  CHECK(wait_for([&] { return re.deck(0).loaded && re.deck(1).loaded; }));
  for (int d = 0; d < 2; ++d) {
    CHECK(re.deck(d).key_index == local.deck(d).key_index);
    for (int i = 0; i < 16; ++i) CHECK(re.deck(d).hot_cue[i] == local.deck(d).hot_cue[i] && re.deck(d).hot_cue[i] > 0);
    CHECK(re.deck(d).hot_cue_end[15] == 9.0 * kSr && re.deck(d).hot_cue_end[14] < 0);
  }
  CHECK(re.deck(1).key_index == 2 && re.deck(0).key_index == 21);
  for (IEngine* e : {static_cast<IEngine*>(&re), static_cast<IEngine*>(&local)}) {
    e->key_sync_config(1, 1, true, 3.0);
    e->key_match(1);
  }
  run(local, 0.05);
  CHECK(wait_for([&] { return re.deck(1).key_sync_result == 1; }));
  const DeckView a = re.deck(1), b = local.deck(1);
  CHECK(a.key_shift == b.key_shift && a.key_sync_target == b.key_sync_target && a.key_sync_mode == 1 && a.key_sync_fine == 1);
  CHECK(a.key_sync_limit == 3.0f && a.key_lock == b.key_lock && a.key_lock);
  for (IEngine* e : {static_cast<IEngine*>(&re), static_cast<IEngine*>(&local)}) e->key_follow(1, 1);
  run(local, 0.05);
  CHECK(wait_for([&] { return re.deck(1).key_sync_follow == 1; }));
  for (IEngine* e : {static_cast<IEngine*>(&re), static_cast<IEngine*>(&local)}) e->key_reset(1);
  run(local, 0.05);
  CHECK(wait_for([&] { return re.deck(1).key_sync_follow == 0 && re.deck(1).key_shift == 0; }));
  CHECK(!re.deck(1).key_lock && re.deck(1).key_sync_result == 0);
  re.set_track_key(1, 5);
  CHECK(wait_for([&] { return re.deck(1).key_index == 5; }));
  re.shutdown_remote(0);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace

int main(int argc, char** argv) {
  test_math();
  std::printf("key math ok\n");
  test_engine();
  std::printf("engine key sync ok\n");
  test_wire();
  std::string helper = (fs::absolute(argv[0]).parent_path() / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  test_remote(helper);
  std::printf("test_key_sync: all passed\n");
  return 0;
}
