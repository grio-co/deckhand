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

}  // namespace

int main(int argc, char** argv) {
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
  }
  fs::remove_all(dir);
  if (rc == 0) std::puts("test_windows_ipc ok");
  return rc;
}
