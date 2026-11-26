#pragma once
// LightingEngine: the object the application owns. Holds the configuration, the show, the outputs and (optionally) a
// render thread. The application feeds it MusicState from its tick and sends commands; nothing else is needed.
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deckhand/lighting/show.hpp"

namespace deckhand::broadcast { class Json; }

namespace deckhand::lighting {

struct LightingConfig {
  bool enabled = false;        // master switch: while false no output is ever opened (preview still renders)
  double refresh_hz = 40;      // 1..60
  double keepalive_s = 1.0;    // an unchanged frame is repeated this often
  ShowConfig show;             // refresh_hz is taken from the field above
  ArtNetConfig artnet;
  SacnConfig sacn;
  SerialConfig serial;
  std::vector<PatchEntry> patch;
  std::vector<Profile> profiles;             // user profiles stored inline
  std::vector<std::string> profile_files;    // .dhfix files, read at configure()
};

deckhand::broadcast::Json config_to_json(const LightingConfig& c);
bool config_from_json(const deckhand::broadcast::Json& j, LightingConfig* out, std::string* err);
bool load_config_file(const std::string& path, LightingConfig* out, std::string* err);  // missing file = defaults
bool save_config_file(const std::string& path, const LightingConfig& c, std::string* err);  // atomic replace

struct OutputInfo {
  std::string name;
  bool open = false;
  OutputStats stats;
  std::string error;
};
struct LightingStatus {
  bool running = false;
  bool outputs_enabled = false;
  ShowStatus show;
  std::vector<OutputInfo> outputs;
  std::vector<std::string> issues;   // patch problems (errors and warnings), human readable
  std::uint64_t frames_rendered = 0, frames_sent = 0;
  int fixtures = 0;
  std::vector<int> universes;
};

class LightingEngine {
 public:
  LightingEngine();
  ~LightingEngine();
  LightingEngine(const LightingEngine&) = delete;
  LightingEngine& operator=(const LightingEngine&) = delete;

  // Applies a configuration (patch validated, profiles loaded). Returns false and keeps the old one when the patch has
  // errors; `err` then lists them. Never opens an output by itself.
  bool configure(const LightingConfig& cfg, std::string* err = nullptr);
  LightingConfig config() const;

  // start(): opens the enabled outputs and runs the render thread. An output that fails to open is reported in the
  // status and does not stop the others. stop(): one dark frame, outputs closed, thread joined.
  // spawn_thread = false opens the outputs only; the caller then drives tick() itself (tests, embedding).
  bool start(std::string* err = nullptr, bool spawn_thread = true);
  void stop();
  bool running() const { return running_.load(); }

  // For embedding and tests: an extra output, opened at start() (or immediately when already running). Counts as
  // enabled by the user. Remove all with clear_attached_outputs().
  void attach_output(std::shared_ptr<Output> out);
  void clear_attached_outputs();

  // Called from the application's tick (10-60 Hz). Thread safe.
  void set_state(const MusicState& m);
  // One engine iteration at monotonic time `now_s` (the thread calls it with the steady clock; tests with their own).
  // Returns true when a frame was rendered. Do not call it while the thread runs.
  bool tick(double now_s);
  // Seconds (same clock as tick) at which the next frame is due; for the thread and for tests.
  double next_due() const;

  // ---- commands (thread safe) ----
  void set_blackout(bool on);
  void set_hold(bool on);
  void set_master(float v);                     // 0..1
  void set_strobe(bool enabled, float max_hz);  // photosensitivity switch and cap
  void set_latency_ms(double ms);
  void set_palette(const PaletteConfig& p);
  void set_override(const Override& o);         // manual layer; auto resumes with release_override()
  void release_override();

  LightingStatus status() const;
  std::vector<UniverseFrame> preview() const;   // copy of the last rendered universes
  std::vector<Look> preview_looks() const;      // per fixture colour/intensity for a UI visualiser

 private:
  void thread_main();
  void open_outputs_locked();
  void close_outputs_locked(bool dark);
  void send_locked(const std::vector<UniverseFrame>& f);

  mutable std::mutex mu_;
  LightingConfig cfg_;
  ProfileSet profiles_;
  ShowEngine show_;
  Pacer pacer_;
  std::vector<std::shared_ptr<Output>> outputs_, attached_;
  std::vector<OutputInfo> infos_;
  std::vector<std::string> issues_;
  std::vector<UniverseFrame> last_sent_;
  bool have_sent_ = false;
  std::uint64_t rendered_ = 0, sent_ = 0;
  double next_due_ = 0;

  std::atomic<bool> running_{false}, stop_{false};
  std::thread thread_;
  std::mutex wait_mu_;
  std::condition_variable cv_;
};

}  // namespace deckhand::lighting
