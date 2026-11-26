// The Windows implementation of the engine IPC layer (src/audio/ipc_win32.cpp): named shared memory, the
// single-instance lock, the loopback control channel with its shared secret, process spawn / liveness / reaping,
// session names, and (when deckhand-engine.exe sits beside this test or DECKHAND_ENGINE_BIN names it) the real engine
// process driven through RemoteEngine, including an engine ended with TerminateProcess (the SIGKILL of Windows).
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../../tests/check.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/remote_engine.hpp"

using namespace deckhand;
using namespace deckhand::ipc;
namespace fs = std::filesystem;

namespace {

void note(const std::string& s) {
  std::printf("%s\n", s.c_str());
  std::fflush(stdout);
}

bool wait_for(auto pred, double seconds) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

// ---- shared memory -------------------------------------------------------------------------------
void test_shm() {
  const std::string name = "Local\\dhtest.shm." + std::to_string(current_pid());
  std::string err;
  Shm a;
  CHECK(Shm::create(name, 10000, a, err));
  CHECK(a.valid() && a.size() == 10000);
  const auto* bytes = static_cast<const unsigned char*>(a.data());
  CHECK(std::all_of(bytes, bytes + 10000, [](unsigned char c) { return c == 0; }));  // zero-filled
  std::memcpy(a.data(), "hello", 6);

  Shm ro;
  CHECK(Shm::open(name, false, ro, err));
  CHECK(ro.size() >= 10000 && ro.size() < 10000 + 65536);  // whole pages
  CHECK(std::memcmp(ro.data(), "hello", 6) == 0);
  Shm rw;
  CHECK(Shm::open(name, true, rw, err));
  std::memcpy(rw.data(), "HELLO", 6);
  CHECK(std::memcmp(a.data(), "HELLO", 6) == 0);  // the same memory

  Shm dup;
  CHECK(!Shm::create(name, 4096, dup, err, /*replace=*/false) && !err.empty());  // the name exists
  Shm::remove(name);  // the name goes; mappings made before stay valid
  CHECK(std::memcmp(ro.data(), "HELLO", 6) == 0);
  Shm gone;
  CHECK(!Shm::open(name, false, gone, err));
  ro.close();
  rw.close();
  Shm again;
  CHECK(Shm::create(name, 4096, again, err, /*replace=*/false));  // free again
  Shm::remove(name);
  Shm missing;
  CHECK(!Shm::open("Local\\dhtest.nothing", false, missing, err));
  note("shared memory: create, open read-only / read-write, zero-fill, replace, remove");
}

// ---- instance lock -------------------------------------------------------------------------------
void test_lock(const fs::path& dir) {
  const std::string path = (dir / "x.lock").string();
  std::string err;
  InstanceLock a, b;
  CHECK(a.acquire(path, err) && a.held());
  CHECK(!b.acquire(path, err) && !b.held());
  a.release();
  CHECK(b.acquire(path, err));
  b.release_and_remove();
  CHECK(!fs::exists(path));
  CHECK(a.acquire(path, err));
  a.release_and_remove();
  note("instance lock: exclusive, released on release(), file removed by release_and_remove()");
}

// ---- control channel -----------------------------------------------------------------------------
void test_channel(const fs::path& dir) {
  const std::string path = (dir / "x.sock").string();
  std::string err;
  Listener l;
  CHECK(l.listen(path, err));
  CHECK(fs::exists(path));
  Conn none;
  CHECK(!l.accept(none, 50));  // nobody there: times out

  Conn client;
  CHECK(Conn::connect(path, client) && client.valid());
  Conn server;
  CHECK(l.accept(server, 2000) && server.valid());
  const char hi[] = "ping over loopback";
  CHECK(client.send_all(hi, sizeof hi));
  char buf[64] = {};
  CHECK(server.recv_all(buf, sizeof hi, 1000));
  CHECK(std::strcmp(buf, hi) == 0);
  bool timed_out = false;
  CHECK(!server.recv_all(buf, 1, 80, &timed_out) && timed_out);  // idle: a timeout, not an error
  client.close();
  timed_out = true;
  CHECK(!server.recv_all(buf, 1, 1000, &timed_out) && !timed_out);  // closed by the peer

  // A stranger that connects to the port without the secret is refused.
  std::string text = slurp(path);
  unsigned port = 0;
  CHECK(std::sscanf(text.c_str(), "%u", &port) == 1 && port > 0);
  SOCKET raw = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<u_short>(port));
  CHECK(::connect(raw, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
  const char wrong[16] = {1, 2, 3};
  ::send(raw, wrong, sizeof wrong, 0);
  Conn refused;
  CHECK(!l.accept(refused, 2000) && !refused.valid());
  char c;
  CHECK(::recv(raw, &c, 1, 0) <= 0);  // the listener closed it
  ::closesocket(raw);

  // The server drops the file and the socket when closed; a client then fails to connect.
  l.close();
  CHECK(!fs::exists(path));
  Conn late;
  CHECK(!Conn::connect(path, late));
  note("control channel: secret handshake, framing reads with timeout, stranger refused, cleanup");
}

// ---- spawn, liveness, reaping --------------------------------------------------------------------
void test_spawn(const fs::path& dir) {
  std::string err;
  wchar_t self_w[1024] = L"";
  GetModuleFileNameW(nullptr, self_w, 1024);
  const std::string self = fs::path(self_w).string();
  const std::string log = (dir / "child.log").string();
  std::uint32_t pid = 0;
  CHECK(spawn_detached(self, {"--child", "600", "5", "with space \"quoted\" arg"}, log, pid, err));
  CHECK(pid != 0 && process_alive(pid));
  CHECK(!reap_child(pid));  // still running
  CHECK(wait_for([&] { return reap_child(pid); }, 10));
  CHECK(!process_alive(pid));
  const std::string out = slurp(log);
  CHECK(out.find("child says hello") != std::string::npos && out.find("with space \"quoted\" arg") != std::string::npos);
  CHECK(!reap_child(pid));  // reaped once only

  std::uint32_t bad = 0;
  CHECK(!spawn_detached((dir / "no-such.exe").string(), {}, "", bad, err) && !err.empty());
  CHECK(!process_alive(0) && process_alive(current_pid()));
  note("process spawn: arguments quoted, output captured, liveness and reaping");
}

void test_names(const fs::path& dir) {
  const std::string d1 = (dir / "one").string(), d2 = (dir / "two").string();
  fs::create_directories(d1);
  fs::create_directories(d2);
  SessionNames a, a2, b;
  std::string err;
  CHECK(session_names(d1, a, err) && session_names(d1, a2, err) && session_names(d2, b, err));
  CHECK(a.shm_main == a2.shm_main && a.lock == a2.lock && a.socket == a2.socket);
  CHECK(a.shm_main != b.shm_main && a.lock != b.lock);
  CHECK(a.block(3) != a.block(4) && a.block(3).rfind(a.shm_block_prefix, 0) == 0);
  CHECK(a.shm_main.rfind("Local\\", 0) == 0);
  CHECK(fs::is_directory(a.dir));
  _putenv_s("DECKHAND_IPC_DIR", (dir / "ipcdir").string().c_str());
  SessionNames o;
  CHECK(session_names(d1, o, err) && o.dir == (dir / "ipcdir").string() && fs::is_directory(o.dir));
  _putenv_s("DECKHAND_IPC_DIR", "");
  note("session names: stable per data folder, distinct between folders, DECKHAND_IPC_DIR honoured");
}

// ---- the engine process ----------------------------------------------------------------------------
std::unique_ptr<LoadedTrack> sine_track(double seconds, std::uint32_t sr, std::int64_t id) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = sr;
  audio->frames = static_cast<std::uint64_t>(seconds * sr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i)
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] = 0.25f * static_cast<float>(std::sin(2 * 3.14159265358979 * 220.0 * static_cast<double>(i) / sr));
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->track_id = id;
  t->path = "/music/test tone.wav";
  t->bpm = 120;
  t->grid.push_back({0, 120, 1});
  return t;
}

Command cmd(Op op, int deck, float f = 0, double d = 0) {
  Command c;
  c.op = op;
  c.deck = static_cast<std::uint8_t>(deck);
  c.f = f;
  c.d = d;
  return c;
}

int test_engine(const fs::path& dir, const std::string& helper) {
  const std::string data = (dir / "engine-data").string();
  fs::create_directories(data);
  RemoteEngine::Options o;
  o.data_dir = data;
  o.helper_path = helper;
  o.null_audio = true;
  o.log_level = 2;
  o.backoff_ms = 100;
  SessionNames names;
  std::string err;
  CHECK(session_names(data, names, err));

  RemoteEngine eng(o);
  std::mutex mu;
  std::vector<RemoteEngine::Status> statuses;
  eng.set_status_callback([&](RemoteEngine::Status s, const std::string&) {
    std::lock_guard<std::mutex> lock(mu);
    statuses.push_back(s);
  });
  if (!eng.connect(err)) {
    std::printf("FAIL: could not start or attach to the engine process: %s\n", err.c_str());
    return 1;
  }
  CHECK(!eng.attached_existing() && eng.engine_pid() != 0 && process_alive(eng.engine_pid()));
  const std::uint32_t first_pid = eng.engine_pid();
  OutputConfig oc;
  CHECK(eng.start(oc, err));
  eng.load(0, sine_track(10.0, 44100, 7));
  CHECK(wait_for([&] { return eng.deck(0).loaded; }, 5));
  eng.send(cmd(Op::MasterLevel, 0, 0.0f));
  eng.send(cmd(Op::PlayToggle, 0));
  CHECK(wait_for([&] { return eng.deck(0).playing && eng.deck(0).position > 4410; }, 5));
  note("engine process: started (pid " + std::to_string(first_pid) + "), track loaded through shared memory, deck plays");

  // A second client (a UI that restarted) attaches to the same engine.
  {
    RemoteEngine other(o);
    CHECK(other.connect(err));
    CHECK(other.attached_existing() && other.engine_pid() == first_pid);
    CHECK(other.deck(0).loaded);
    other.detach();
  }
  note("engine process: a second client attaches to the running engine");

  // The engine is ended without warning: noticed, restarted, decks empty, a new load plays.
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, first_pid);
  CHECK(h != nullptr);
  TerminateProcess(h, 137);
  CloseHandle(h);
  CHECK(wait_for([&] {
    std::lock_guard<std::mutex> lock(mu);
    return std::find(statuses.begin(), statuses.end(), RemoteEngine::Status::Restarted) != statuses.end();
  }, 10));
  CHECK(eng.engine_pid() != first_pid && process_alive(eng.engine_pid()));
  CHECK(!eng.deck(0).loaded);
  CHECK(eng.start(oc, err));
  eng.load(1, sine_track(10.0, 48000, 5));
  CHECK(wait_for([&] { return eng.deck(1).loaded; }, 5));
  eng.send(cmd(Op::PlayToggle, 1));
  CHECK(wait_for([&] { return eng.deck(1).position > 4800; }, 5));
  note("engine process: ended with TerminateProcess, restarted as pid " + std::to_string(eng.engine_pid()) + ", new load plays");

  eng.shutdown_remote(50);
  CHECK(wait_for([&] {
    Shm probe;
    std::string e;
    return !Shm::open(names.shm_main, false, probe, e);
  }, 5));
  CHECK(!fs::exists(names.socket));
  for (int i = 0; i < kBlockSlots; ++i) {
    Shm probe;
    std::string e;
    CHECK(!Shm::open(names.block(i), false, probe, e));
  }
  InstanceLock again;
  CHECK(again.acquire(names.lock, err));
  again.release_and_remove();
  note("engine process: clean shutdown released the segments, the socket file and the lock");
  return 0;
}

// ---- the UI process dies, the engine plays on ----------------------------------------------------------------------
// A stand-in for the app: attaches to (starting if needed) the engine for `data`, plays a track, optionally records,
// prints "READY <engine pid>" and then waits to be killed.
int ui_process(const std::string& data, const std::string& helper, const std::string& rec) {
  RemoteEngine::Options o;
  o.data_dir = data;
  o.helper_path = helper;
  o.null_audio = true;
  o.log_level = 2;
  RemoteEngine eng(o);
  std::string err;
  OutputConfig oc;
  if (!eng.connect(err) || !eng.start(oc, err)) {
    std::printf("UIFAIL %s\n", err.c_str());
    return 1;
  }
  eng.load(0, sine_track(200.0, 44100, 7));
  if (!wait_for([&] { return eng.deck(0).loaded; }, 10)) return 1;
  eng.send(cmd(Op::MasterLevel, 0, 0.0f));
  eng.send(cmd(Op::PlayToggle, 0));
  if (!wait_for([&] { return eng.deck(0).playing && eng.deck(0).position > 4410; }, 5)) return 1;
  if (!rec.empty() && !eng.record_open(rec, false, err)) {
    std::printf("UIFAIL record %s\n", err.c_str());
    return 1;
  }
  std::printf("READY %u\n", eng.engine_pid());
  std::fflush(stdout);
  // Only one UI may be attached to an engine, so the test process cannot look at the engine itself while this one lives:
  // this process reports the playhead with a system-wide monotonic timestamp instead (now_ns is comparable between processes).
  for (int i = 0; i < 900; ++i) {
    const DeckView d = eng.deck(0);
    std::printf("POS %.3f %.1f %llu\n", d.position, d.sample_rate, static_cast<unsigned long long>(now_ns()));
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return 0;
}

struct WavInfo {
  bool valid = false;  // a finalised 24-bit stereo file whose header sizes match the file
  double seconds = 0;
};

WavInfo read_wav(const fs::path& p) {
  WavInfo w;
  const std::string d = slurp(p);
  if (d.size() < 80 || d.compare(0, 4, "RIFF") != 0 || d.compare(8, 4, "WAVE") != 0 || d.compare(48, 4, "fmt ") != 0 ||
      d.compare(72, 4, "data") != 0)
    return w;
  auto u32 = [&](std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, d.data() + at, 4);
    return v;
  };
  auto u16 = [&](std::size_t at) {
    std::uint16_t v = 0;
    std::memcpy(&v, d.data() + at, 2);
    return v;
  };
  const std::uint32_t rate = u32(60);
  if (u16(56) != 1 || u16(58) != 2 || u16(70) != 24 || rate == 0) return w;
  if (u32(4) != d.size() - 8 || u32(76) != d.size() - 80) return w;  // sizes were patched at finalisation
  w.valid = true;
  w.seconds = static_cast<double>(u32(76)) / 6.0 / rate;
  return w;
}

int test_ui_kill(const fs::path& dir, const std::string& helper) {
  const std::string data = (dir / "ui-kill-data").string();
  fs::create_directories(data);
  const std::string rec1 = (dir / "through-kill.wav").string(), rec2 = (dir / "orderly-quit.wav").string();
  wchar_t self_w[1024] = L"";
  GetModuleFileNameW(nullptr, self_w, 1024);
  const std::string self = fs::path(self_w).string();
  const std::string log = (dir / "ui1.log").string();
  SessionNames names;
  std::string err;
  CHECK(session_names(data, names, err));

  std::uint32_t ui_pid = 0;
  CHECK(spawn_detached(self, {"--ui", data, helper, rec1}, log, ui_pid, err));
  std::uint32_t engine_pid = 0;
  CHECK(wait_for([&] {
    const std::string t = slurp(log);
    const auto at = t.find("READY ");
    if (at == std::string::npos) return false;
    engine_pid = static_cast<std::uint32_t>(std::strtoul(t.c_str() + at + 6, nullptr, 10));
    return engine_pid != 0;
  }, 60));
  CHECK(process_alive(engine_pid) && engine_pid != ui_pid);
  note("ui kill: the UI process (pid " + std::to_string(ui_pid) + ") started the engine (pid " + std::to_string(engine_pid) +
       "), plays a track and records");

  RemoteEngine::Options o;
  o.data_dir = data;
  o.helper_path = helper;
  o.null_audio = true;
  o.log_level = 2;
  // The last playhead the UI reported (complete lines only) and when.
  struct Pos {
    double frames = 0, rate = 44100;
    std::uint64_t ns = 0;
    int lines = 0;
  };
  auto last_pos = [&] {
    Pos p;
    const std::string t = slurp(log);
    std::size_t at = 0;
    while ((at = t.find("POS ", at)) != std::string::npos) {
      const std::size_t end = t.find('\n', at);
      if (end == std::string::npos) break;  // a line that was being written
      unsigned long long ns = 0;
      double f = 0, r = 0;
      if (std::sscanf(t.c_str() + at, "POS %lf %lf %llu", &f, &r, &ns) == 3) {
        p.frames = f;
        p.rate = r;
        p.ns = ns;
        ++p.lines;
      }
      at = end;
    }
    return p;
  };
  CHECK(wait_for([&] { return last_pos().lines >= 8; }, 10));  // at least 1.5 s of audio, so the recording has content
  const Pos before = last_pos();
  CHECK(before.frames > 4410);

  HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, ui_pid);
  CHECK(h != nullptr);
  TerminateProcess(h, 137);  // no WM_CLOSE, no handlers: the "UI crash"
  CHECK(WaitForSingleObject(h, 10000) == WAIT_OBJECT_0);
  CloseHandle(h);
  reap_child(ui_pid);
  CHECK(!process_alive(ui_pid));
  CHECK(process_alive(engine_pid));  // the engine is not tied to the UI's lifetime (no job object, no parent link)
  const Pos last = last_pos();  // the final report, possibly newer than `before`
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // nobody is attached for a while

  RemoteEngine back(o);
  CHECK(back.connect(err) && back.attached_existing() && back.engine_pid() == engine_pid);
  CHECK(wait_for([&] { return back.deck(0).loaded && back.deck(0).playing; }, 5));
  const DeckView d = back.deck(0);
  const double elapsed = static_cast<double>(now_ns() - last.ns) / 1e9;
  const double advanced = (d.position - last.frames) / last.rate;
  note("ui kill: a new UI attached " + std::to_string(elapsed) + " s after the last reading; the deck advanced " +
       std::to_string(advanced) + " s");
  CHECK(std::fabs(advanced - elapsed) < 0.75);
  CHECK(d.position < d.length);
  const std::string path_back = back.recorder_path();
  CHECK(back.recorder_status().open && path_back.find("through-kill.wav") != std::string::npos);

  // The recording that was running when the UI died is still being written; the new UI ends it.
  back.record_close();
  CHECK(wait_for([&] { return !back.recorder_status().open; }, 10));
  const double closed_after = static_cast<double>(now_ns() - last.ns) / 1e9;
  const WavInfo w1 = read_wav(rec1);
  note("ui kill: the recording that spanned the kill is a valid WAV of " + std::to_string(w1.seconds) + " s (its last report was " +
       std::to_string(closed_after) + " s before the close; the recording began before that report)");
  CHECK(w1.valid && w1.seconds >= closed_after - 0.5);

  // An orderly quit finalises a recording that is still open, then leaves nothing behind.
  CHECK(back.record_open(rec2, false, err));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  back.shutdown_remote(50);
  CHECK(wait_for([&] { return !process_alive(engine_pid); }, 10));
  const WavInfo w2 = read_wav(rec2);
  CHECK(w2.valid && w2.seconds > 0.8);
  CHECK(wait_for([&] {
    Shm probe;
    std::string e;
    return !Shm::open(names.shm_main, false, probe, e);
  }, 5));
  CHECK(!fs::exists(names.socket));
  for (int i = 0; i < kBlockSlots; ++i) {
    Shm probe;
    std::string e;
    CHECK(!Shm::open(names.block(i), false, probe, e));
  }
  InstanceLock again;
  CHECK(again.acquire(names.lock, err));
  again.release_and_remove();
  note("ui kill: orderly quit finalised the open recording (" + std::to_string(w2.seconds) +
       " s), stopped the engine process and left no segment, socket file or lock");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::strcmp(argv[1], "--ui") == 0) return ui_process(argv[2], argv[3], argv[4]);
  if (argc >= 5 && std::strcmp(argv[1], "--child") == 0) {  // helper process for test_spawn
    std::printf("child says hello %s\n", argv[4]);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
    return std::atoi(argv[3]);
  }
  const fs::path dir = fs::temp_directory_path() / ("dh-win-ipc-test-" + std::to_string(current_pid()));
  std::error_code ignore;
  fs::remove_all(dir, ignore);  // leftovers of an earlier run (a process id can repeat)
  fs::create_directories(dir);

  test_shm();
  test_lock(dir);
  test_channel(dir);
  test_spawn(dir);
  test_names(dir);

  std::string helper = (fs::path(executable_dir()) / "deckhand-engine.exe").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  int rc = 0;
  if (!fs::exists(helper)) {
    std::printf("skip: deckhand-engine not found (%s)\n", helper.c_str());
  } else {
    rc = test_engine(dir, helper);
    if (rc == 0) rc = test_ui_kill(dir, helper);
  }
  fs::remove_all(dir);
  if (rc == 0) std::puts("test_windows_ipc ok");
  return rc;
}
