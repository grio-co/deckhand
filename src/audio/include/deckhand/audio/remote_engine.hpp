#pragma once
// RemoteEngine: the IEngine the UI process uses when the engine runs in its own process (deckhand-engine).
// Commands go through a lock-free ring in shared memory, state comes back through seqlock snapshots, decoded
// audio is handed over in shared-memory blocks, and everything else uses a Unix-domain control socket
// (see ipc.hpp). If the engine process dies it is restarted with back-off; if this process dies the engine keeps
// playing and the next RemoteEngine attaches to it.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deckhand/audio/engine.hpp"

namespace deckhand {

class RemoteEngine final : public IEngine {
 public:
  struct Options {
    std::string data_dir;
    std::string resource_dir;  // controller mappings are read from <resource_dir>/controllers
    std::string helper_path;   // empty: deckhand-engine beside this executable
    bool null_audio = false;   // tests: the engine opens miniaudio's null device
    int log_level = 2;
    bool auto_restart = true;
    int backoff_ms = 500;      // first restart delay; doubles after each quick failure
  };
  enum class Status {
    Attached,      // connected to an engine that was already running
    Spawned,       // started a new engine
    Died,          // the engine process is gone
    Restarted,     // a replacement engine is up (decks are empty; open the output again)
    GaveUp,        // too many quick failures; no more restarts
    LoadRejected,  // the engine refused a track block
  };
  using StatusFn = std::function<void(Status, const std::string& message)>;
  struct UiRequest {
    enum Kind { Browse, BrowsePress, LoadSelected } kind = Browse;
    int value = 0;  // Browse: steps, LoadSelected: deck
  };
  using UiRequestFn = std::function<void(const UiRequest&)>;

  explicit RemoteEngine(const Options& options);
  ~RemoteEngine() override;  // detaches: the engine keeps running

  // Attaches to the engine for this data folder, starting one if none runs. Starts supervision on success.
  bool connect(std::string& error);
  bool connected() const;
  bool attached_existing() const { return attached_existing_; }
  std::uint32_t engine_pid() const;
  std::uint64_t session_id() const;
  void set_status_callback(StatusFn fn);
  void set_ui_request_handler(UiRequestFn fn);
  void heartbeat();                          // call from the UI's control loop (proves the UI is alive)
  void shutdown_remote(int fade_ms = 120);   // normal quit: the engine fades out, stops audio and exits
  void detach();                             // stop supervising; the engine keeps playing
  std::uint64_t dropped_commands() const { return dropped_.load(); }
  // What the engine process recorded about the track on a deck (set when it accepted the load): lets a restarted
  // UI rebuild its display state. False when the deck is empty.
  bool deck_info(int deck, std::int64_t& track_id, std::string& path) const;

  // ---- controller hub, which lives in the engine process ----
  bool hub_learn_begin(const std::string& action, std::string& error);
  void hub_learn_cancel();
  bool hub_learn_commit();
  std::string hub_learn_result(bool finalize);
  int hub_learn_state() const;  // LearnState
  std::string hub_learn_device() const;
  std::string hub_learn_error() const;
  std::vector<std::string> hub_connected() const;
  bool hub_audio_hint(std::string& device_match, int& master_channel, int& phones_channel) const;

  // ---- IEngine ----
  bool start(const OutputConfig& config, std::string& error) override;
  void stop() override;
  bool running() const override;
  std::string device_name() const override;
  int output_channels() const override;
  std::uint32_t sample_rate() const override;
  std::vector<std::string> devices() override;
  void send(const Command& c) override;
  void load(int deck, std::unique_ptr<LoadedTrack> track) override;
  DeckView deck(int index) const override;
  MixerView mixer() const override;
  SamplerView sampler(int slot) const override;
  void sampler_load(int slot, std::unique_ptr<LoadedSample> sample) override;
  bool record_open(const std::string& path, bool pre_clip, std::string& error) override;
  void record_stop() override;
  void record_close() override;
  RecorderStatus recorder_status() const override;
  std::string recorder_path() const override;
  void stream_enable(bool on, bool pre_clip = false) override;
  std::size_t stream_read(float* interleaved_stereo, std::size_t max_frames) override;
  std::uint64_t stream_dropped_frames() const override;
  std::uint64_t clock_frames() const override;
  double clock_seconds() const override;
  std::uint64_t clock_at_wall(std::chrono::steady_clock::time_point t) const override;
  std::uint64_t schedule(const Command& c, const Schedule& when) override;
  bool cancel(std::uint64_t id) override;
  std::size_t scheduled_count() const override;
  void cancel_all() override;
  bool poll(EngineEvent& out) override;
  void collect_garbage() override {}  // the engine process frees its own garbage

 private:
  struct Link;
  std::shared_ptr<Link> link() const;
  bool establish(std::string& error, bool allow_spawn, bool& spawned);
  bool rpc(Link& l, std::uint8_t type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& reply, int timeout_ms);
  bool rpc(std::uint8_t type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& reply, int timeout_ms);
  void monitor_main();
  void notify(Status s, const std::string& message);
  int next_slot();

  Options opt_;
  std::string names_error_;
  struct Names;
  std::unique_ptr<Names> names_;
  mutable std::mutex link_mutex_;
  std::shared_ptr<Link> link_;
  std::mutex block_mutex_;
  int slot_cursor_ = 0;
  std::atomic<std::uint64_t> generation_;
  std::atomic<std::uint64_t> dropped_{0};
  bool attached_existing_ = false;
  std::atomic<bool> stopping_{false}, supervise_{false};
  std::thread monitor_;
  std::mutex cb_mutex_;
  StatusFn status_fn_;
  UiRequestFn ui_fn_;
  std::mutex wake_mutex_;
  std::condition_variable wake_;
};

}  // namespace deckhand
