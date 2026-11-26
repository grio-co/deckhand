// The engine helper must not linger when the UI that started it gave up waiting (it would hold the controllers beside
// the in-process fallback), and must never leave once a UI attached (audio and controllers survive a UI crash).
// Null audio device; no real audio or MIDI device is opened (the helper is started with --no-midi).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "check.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/remote_engine.hpp"

namespace fs = std::filesystem;
using namespace deckhand;
using namespace deckhand::ipc;

namespace {

fs::path make_dir(const char* tag) {
  const fs::path p = fs::temp_directory_path() / (std::string("dh_helper_exit_") + tag + "_" + std::to_string(current_pid()) + "_" +
                                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000));
  fs::create_directories(p);
  return p;
}

// Waits (no deadline assertion: a generous limit only stops a hung test) until the child exited.
bool wait_exit(std::uint32_t pid, int seconds = 90) {
  for (int i = 0; i < seconds * 20; ++i) {
    if (reap_child(pid)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

std::uint32_t spawn(const std::string& helper, const fs::path& dir, std::vector<std::string> extra) {
  std::vector<std::string> args = {"--data-dir", dir.string(), "--null-audio", "--no-midi"};
  args.insert(args.end(), extra.begin(), extra.end());
  std::uint32_t pid = 0;
  std::string err;
  CHECK(spawn_detached(helper, args, (dir / "helper.out").string(), pid, err));
  return pid;
}

void note(const fs::path& dir, std::uint32_t pid) {
  std::ofstream f(dir / "deckhand-engine.abandon", std::ios::binary);
  f << pid;
}

void test_exits(const std::string& helper) {
  {  // told to leave: the note names this very process
    const fs::path dir = make_dir("told");
    const std::uint32_t pid = spawn(helper, dir, {});
    note(dir, pid);
    CHECK(wait_exit(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  {  // a note naming another process is ignored: only the timer decides
    const fs::path dir = make_dir("other");
    const std::uint32_t pid = spawn(helper, dir, {"--exit-unattached", "2"});
    note(dir, pid + 100000);
    CHECK(wait_exit(pid));  // by the timer
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  {  // the UI gave up (start wait 0): it writes the note itself, naming the helper it started
    const fs::path dir = make_dir("gaveup");
    RemoteEngine::Options o;
    o.data_dir = dir.string();
    o.helper_path = helper;
    o.null_audio = true;
    o.auto_restart = false;
    o.start_wait_s = 0;
    RemoteEngine re(o);
    std::string err;
    CHECK(!re.connect(err));
    std::ifstream f(dir / "deckhand-engine.abandon", std::ios::binary);
    unsigned pid = 0;
    CHECK(static_cast<bool>(f >> pid) && pid != 0);
    CHECK(wait_exit(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
}

void test_attached_stays(const std::string& helper) {
  const fs::path dir = make_dir("attached");
  const std::uint32_t pid = spawn(helper, dir, {});
  RemoteEngine::Options o;
  o.data_dir = dir.string();
  o.helper_path = (dir / "no-such-helper").string();  // never spawns: it can only attach to the helper started above
  o.null_audio = true;
  o.auto_restart = false;
  RemoteEngine re(o);
  std::string err;
  bool attached = false;
  for (int i = 0; i < 1200 && !attached; ++i) {  // until the helper listens (a loaded machine takes a while)
    attached = re.connect(err);
    if (!attached) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(attached);
  CHECK(re.attached_existing());
  note(dir, pid);          // even told to leave, an engine a UI attached to stays
  std::this_thread::sleep_for(std::chrono::seconds(3));  // the helper looks every 250 ms; only a false pass is possible here
  CHECK(process_alive(pid) && !reap_child(pid));
  re.shutdown_remote(0);
  CHECK(wait_exit(pid));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace

int main(int, char** argv) {
  std::string helper = (fs::absolute(argv[0]).parent_path() / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  if (!fs::exists(helper)) {
    std::printf("engine process not built, skipped\n");
    return 0;
  }
  test_exits(helper);
  test_attached_stays(helper);
  std::printf("test_engine_helper_exit: all passed\n");
  return 0;
}
