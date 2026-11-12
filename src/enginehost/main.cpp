// deckhand-engine: the audio engine in its own process.
//
// It owns the output device, the engine, the recorder and the stream tap, and it hosts the controller hub (MIDI
// I/O and mapping files), so a crashed or hung UI cannot stop the music or take the hardware away. The UI process
// (RemoteEngine) talks to it through the shared segments and the control socket described in
// deckhand/audio/ipc.hpp. The process never stops audio because the UI is gone: it only changes behaviour (browse
// requests from the controller are forwarded to the UI while it is alive and dropped otherwise).
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <mutex>
#include <thread>

#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace deckhand;
using namespace deckhand::ipc;

namespace {

std::atomic<int> g_signal{0};
void on_signal(int s) { g_signal.store(s); }

struct Options {
  std::string data_dir, resource_dir;
  bool null_audio = false, no_midi = false, probe = false;
  int log_level = 2;
};

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

void copy_text(char* dst, std::size_t cap, const std::string& s) {
  const std::size_t n = std::min(cap - 1, s.size());
  std::memcpy(dst, s.data(), n);
  dst[n] = 0;
}

class Host final : public ControlSink {
 public:
  explicit Host(Options o) : opt_(std::move(o)) {}
  ~Host() override { cleanup(); }

  bool init(std::string& error);
  void run();

  // ControlSink: the hub drives the engine directly; browsing is the UI's business.
  void command(const Command& c) override { engine_.send(c); }
  void browse(int steps) override { ui_event(kEvBrowseSteps, steps, 0); }
  void browse_press() override { ui_event(kEvBrowsePress, 0, 0); }
  void load_selected(int deck) override { ui_event(kEvLoadSelected, 0, deck); }

 private:
  void ui_event(std::uint32_t kind, int slot, int deck) {
    if (!ui_alive_.load()) return;  // nobody to ask: ignore, never block
    WireEvent e;
    e.kind = kind;
    e.slot = slot;
    e.deck = static_cast<std::uint32_t>(std::max(0, deck));
    push_event(e);
  }
  bool push_event(const WireEvent& e) {
    std::lock_guard<std::mutex> lock(event_mutex_);
    return sh_->events.push(e);
  }

  void service_main();
  void server_main();
  void handle_conn(Conn& c);
  void dispatch(std::uint8_t type, Rd& rd, Wr& out);
  int drain_commands();
  void apply_load(const WireCommand& w);
  void pump_events();
  void pump_stream();
  void publish_views();
  void publish_device();
  void publish_hub();
  void hub_tick();
  bool open_device(OutputConfig cfg, std::string& error);
  void audio_recheck();
  void shutdown(int fade_ms);
  void cleanup();

  Options opt_;
  Engine engine_;
  SessionNames names_;
  InstanceLock lock_;
  Shm shm_;
  Shared* sh_ = nullptr;
  Listener listener_;
  std::unique_ptr<ControllerHub> hub_;
  std::unique_ptr<MidiIO> midi_;

  std::mutex dev_mutex_, rec_mutex_, event_mutex_, hub_mutex_;
  OutputConfig last_cfg_;
  bool want_open_ = false;
  std::string choice_, rec_path_;
  DeviceInfo dev_;

  DeckMeta meta_[kMaxDecks];  // service thread only
  std::atomic<bool> ui_alive_{false}, stream_on_{false};
  std::atomic<bool> quit_{false}, service_stop_{false}, server_stop_{false}, recheck_{false}, reset_request_{false};
  std::atomic<int> quit_fade_ms_{120};
  std::atomic<std::uint32_t> reset_acks_{0};
  std::thread service_, server_;
  struct Handler {
    std::thread th;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::list<Handler> handlers_;
  bool cleaned_ = false;
};

// ---- start-up ---------------------------------------------------------------------------------------
bool Host::init(std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(opt_.data_dir, ec);
  log::set_level(static_cast<log::Level>(std::clamp(opt_.log_level, 0, 4)));
  log::set_file(opt_.data_dir + "/deckhand-engine.log");
  if (!session_names(opt_.data_dir, names_, error)) return false;
  if (!lock_.acquire(names_.lock, error)) return false;

  // We hold the lock, so nothing else serves this data folder: whatever is left over is stale.
  Shm::remove(names_.shm_main);
  for (int i = 0; i < kBlockSlots; ++i) Shm::remove(names_.block(i));

  if (!Shm::create(names_.shm_main, sizeof(Shared), shm_, error)) return false;
  sh_ = static_cast<Shared*>(shm_.data());
  init_shared(*sh_);
  sh_->hdr.session_id.store(now_ns() ^ (static_cast<std::uint64_t>(current_pid()) << 40) ^ 0x9E3779B97F4A7C15ull);
  sh_->hdr.engine_pid.store(current_pid());
  sh_->hdr.engine_start_ns.store(now_ns());
  sh_->hdr.engine_state.store(kStarting);
  publish_device();
  if (!listener_.listen(names_.socket, error)) return false;

  if (!opt_.no_midi) {
    std::vector<std::string> folders;
    if (!opt_.resource_dir.empty()) folders.push_back(opt_.resource_dir + "/controllers");
    folders.push_back(opt_.data_dir + "/controllers");
    hub_ = std::make_unique<ControllerHub>(*this, load_mappings(folders));
    midi_ = create_midi_io(
        [this](const std::string& device, const std::uint8_t* data, std::size_t n) {
          if (hub_) hub_->midi(device, data, n);
        },
        [this] {
          if (hub_) hub_->devices_changed();
          recheck_ = true;
        });
    hub_->set_io(midi_.get());
  }
  publish_hub();
  sh_->hdr.engine_state.store(kRunning);
  service_ = std::thread([this] { service_main(); });
  server_ = std::thread([this] { server_main(); });
  DH_LOG(Info, "engine process %u up (data %s)%s", current_pid(), opt_.data_dir.c_str(), opt_.null_audio ? ", null audio" : "");
  return true;
}

void Host::run() {
  while (!quit_ && g_signal.load() == 0) {
#if defined(__APPLE__)
    // CoreMIDI delivers device changes through the main thread's run loop.
    const auto t0 = std::chrono::steady_clock::now();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    if (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(10)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
  }
  if (g_signal.load() != 0) DH_LOG(Info, "signal %d: shutting down", g_signal.load());
  shutdown(g_signal.load() != 0 ? 80 : quit_fade_ms_.load());
}

void Host::shutdown(int fade_ms) {
  sh_->hdr.engine_state.store(kStopping);
  {
    std::lock_guard<std::mutex> lock(dev_mutex_);
    want_open_ = false;  // the recheck must not reopen the device while we leave
  }
  // Fade the master out so quitting does not click.
  const float level = engine_.mixer().master_level;
  if (engine_.running() && level > 0 && fade_ms > 0) {
    const int steps = 10;
    for (int k = 1; k <= steps; ++k) {
      Command c;
      c.op = Op::MasterLevel;
      c.f = level * (1.0f - static_cast<float>(k) / steps);
      engine_.send(c);
      std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, fade_ms / steps)));
    }
  }
  if (hub_) hub_->set_io(nullptr);
  midi_.reset();
  service_stop_ = true;
  if (service_.joinable()) service_.join();
  {  // finalise a recording that is still open (this is what makes the WAV valid after SIGTERM)
    std::lock_guard<std::mutex> lock(rec_mutex_);
    if (engine_.recorder_status().open) engine_.record_close();
  }
  engine_.stop();
  server_stop_ = true;
  if (server_.joinable()) server_.join();
  for (Handler& h : handlers_)
    if (h.th.joinable()) h.th.join();
  handlers_.clear();
  hub_.reset();
  cleanup();
  DH_LOG(Info, "engine process stopped");
}

void Host::cleanup() {
  if (cleaned_) return;
  cleaned_ = true;
  listener_.close();
  if (lock_.held()) {  // never touch the names of an engine that is running (we lost the lock race)
    Shm::remove(names_.shm_main);
    for (int i = 0; i < kBlockSlots; ++i) Shm::remove(names_.block(i));
  }
  shm_.close();
  lock_.release_and_remove();
}

// ---- service thread: commands in, snapshots and events out -------------------------------------------------------------
int Host::drain_commands() {
  if (reset_request_.exchange(false)) {
    sh_->cmd.reset();
    reset_acks_.fetch_add(1);
  }
  int n = 0;
  WireCommand w;
  while (n < 512 && sh_->cmd.pop(w)) {
    ++n;
    if (w.op == static_cast<std::uint8_t>(Op::Load)) {
      apply_load(w);
      continue;
    }
    Command c;
    if (validate_command(w, c)) engine_.send(c);
    else sh_->hdr.rejected_commands.fetch_add(1, std::memory_order_relaxed);
  }
  return n;
}

void Host::apply_load(const WireCommand& w) {
  WireEvent ev;
  ev.kind = kEvLoadResult;
  ev.deck = w.deck;
  ev.slot = w.i;
  ev.sched_id = w.aux;
  std::string error;
  std::unique_ptr<LoadedTrack> t;
  if (w.deck < kMaxDecks && w.i >= 0 && w.i < kBlockSlots) {
    const std::string name = names_.block(w.i);
    t = map_track_block(name, w.i, w.aux, error);
    if (t || error != "stale block") Shm::remove(name);  // the mapping (if any) stays alive without the name
  } else {
    error = "bad load request";
  }
  if (!t) {
    DH_LOG(Warn, "engine: refused a track for deck %d: %s", static_cast<int>(w.deck) + 1, error.c_str());
    ev.clock_frame = 1;
    push_event(ev);
    return;
  }
  DeckMeta& m = meta_[w.deck];
  m = DeckMeta{};
  m.track_id = t->track_id;
  m.generation = w.aux;
  m.path_len = static_cast<std::uint32_t>(std::min<std::size_t>(t->path.size(), sizeof m.path - 1));
  std::memcpy(m.path, t->path.data(), m.path_len);
  ev.track_id = t->track_id;
  engine_.load(w.deck, std::move(t));
  push_event(ev);
}

void Host::pump_events() {
  EngineEvent e;
  while (engine_.poll(e)) {
    WireEvent w;
    w.kind = static_cast<std::uint32_t>(e.type);
    w.slot = e.slot;
    w.deck = e.deck;
    w.frame = e.frame;
    w.track_id = e.track_id;
    w.sched_id = e.sched_id;
    w.clock_frame = e.clock_frame;
    push_event(w);  // when the UI is gone the ring fills up and the newest are dropped; the backlog is kept for the next UI
  }
}

void Host::pump_stream() {
  if (!stream_on_.load()) return;
  float tmp[2048];
  while (sh_->stream.free_frames() >= 1024) {
    const std::size_t n = engine_.stream_read(tmp, 1024);
    if (n == 0) break;
    sh_->stream.push(tmp, n);
  }
  sh_->hdr.stream_dropped.store(engine_.stream_dropped_frames(), std::memory_order_relaxed);
}

void Host::publish_views() {
  for (int i = 0; i < kMaxDecks; ++i) {
    const DeckView v = engine_.deck(i);
    sh_->deck[i].write(v);
    DeckMeta m = meta_[i];
    m.loaded = v.loaded && v.track_id == m.track_id ? 1u : 0u;
    sh_->meta[i].write(m);
  }
  sh_->mixer.write(engine_.mixer());
  for (int i = 0; i < kSamplerSlots; ++i) sh_->sampler[i].write(engine_.sampler(i));
  RecInfo r;
  r.st = engine_.recorder_status();
  {
    std::lock_guard<std::mutex> lock(rec_mutex_);
    r.path_len = static_cast<std::uint32_t>(std::min<std::size_t>(rec_path_.size(), sizeof r.path - 1));
    std::memcpy(r.path, rec_path_.data(), r.path_len);
  }
  sh_->rec.write(r);
  sh_->hdr.clock_frames.store(engine_.clock_frames(), std::memory_order_release);
  sh_->hdr.scheduled_count.store(static_cast<std::uint32_t>(engine_.scheduled_count()), std::memory_order_relaxed);
}

void Host::publish_device() {
  DeviceInfo d;
  d.running = engine_.running() ? 1 : 0;
  d.channels = engine_.output_channels();
  d.sample_rate = engine_.sample_rate();
  const std::string name = engine_.device_name();
  d.name_len = static_cast<std::uint32_t>(std::min<std::size_t>(name.size(), sizeof d.name - 1));
  std::memcpy(d.name, name.data(), d.name_len);
  dev_ = d;
  sh_->device.write(d);
}

void Host::publish_hub() {
  if (!hub_) return;
  std::lock_guard<std::mutex> lock(hub_mutex_);
  HubInfo h;
  const std::vector<std::string> names = hub_->connected();
  h.n_connected = static_cast<std::uint32_t>(std::min<std::size_t>(names.size(), kHubNames));
  for (std::uint32_t i = 0; i < h.n_connected; ++i) copy_text(h.names[i], sizeof h.names[i], names[i]);
  std::string match;
  int m = 0, p = 2;
  if (hub_->audio_hint(match, m, p)) {
    h.audio_valid = 1;
    h.audio_master = m;
    h.audio_phones = p;
    copy_text(h.audio_match, sizeof h.audio_match, match);
  }
  const LearnState st = hub_->learn_state();
  h.learn_state = static_cast<std::uint32_t>(st);
  copy_text(h.learn_device, sizeof h.learn_device, hub_->learn_device());
  copy_text(h.learn_error, sizeof h.learn_error, hub_->learn_error());
  if (st == LearnState::Observing || st == LearnState::Done) copy_text(h.learn_line, sizeof h.learn_line, hub_->learn_result(false));
  sh_->hub.write(h);
}

void Host::hub_tick() {
  if (!hub_) return;
  DeckView views[kMaxDecks];
  for (int i = 0; i < kMaxDecks; ++i) views[i] = engine_.deck(i);
  const MixerView mixer = engine_.mixer();
  SamplerView samplers[kSamplerSlots];
  for (int i = 0; i < kSamplerSlots; ++i) samplers[i] = engine_.sampler(i);
  const double now = static_cast<double>(now_ns()) / 1e9;
  std::lock_guard<std::mutex> lock(hub_mutex_);
  hub_->tick(views, kMaxDecks, &mixer, samplers, kSamplerSlots, now);
}

void Host::service_main() {
  std::uint64_t last_pub = 0, last_hub = 0, last_hubpub = 0, last_gc = 0;
  constexpr std::uint64_t ms = 1000000ull;
  while (!service_stop_) {
    const std::uint64_t t = now_ns();
    const int n = drain_commands();
    pump_events();
    pump_stream();
    if (t - last_pub >= 2 * ms) {
      publish_views();
      last_pub = t;
    }
    if (t - last_hub >= 10 * ms) {
      hub_tick();
      last_hub = t;
    }
    if (t - last_hubpub >= 50 * ms) {
      publish_hub();
      last_hubpub = t;
    }
    if (t - last_gc >= 100 * ms) {
      engine_.collect_garbage();
      last_gc = t;
    }
    sh_->hdr.engine_heartbeat_ns.store(t, std::memory_order_relaxed);
    const std::uint32_t ui = sh_->hdr.ui_pid.load(std::memory_order_relaxed);
    const bool alive = ui != 0 && t - sh_->hdr.ui_heartbeat_ns.load(std::memory_order_relaxed) < 3000 * ms;
    if (alive != ui_alive_.exchange(alive))
      DH_LOG(Info, alive ? "UI attached (pid %u)" : "UI heartbeat lost (pid %u): audio and controllers keep running", ui);
    if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
}

// ---- audio device -----------------------------------------------------------------------------------------------------
bool Host::open_device(OutputConfig cfg, std::string& error) {
  std::lock_guard<std::mutex> lock(dev_mutex_);
  cfg.null_backend = opt_.null_audio;
  cfg.period_frames = std::clamp<std::uint32_t>(cfg.period_frames, 64, 2048);
  want_open_ = true;
  last_cfg_ = cfg;
  choice_ = cfg.device_hint;
  const bool ok = engine_.start(cfg, error);
  publish_device();
  return ok;
}

// What the UI's recheck did in-process: follow a controller's audio interface, and reopen a device that went away.
void Host::audio_recheck() {
  std::lock_guard<std::mutex> lock(dev_mutex_);
  if (!want_open_) return;
  std::string error;
  if (last_cfg_.follow_controller && hub_ && !opt_.null_audio) {
    std::string match, want;
    int master = 0, phones = 2;
    if (hub_->audio_hint(match, master, phones)) {
      for (const std::string& name : engine_.devices())
        if (lower(name).find(match) != std::string::npos) want = match;
    }
    if (want != choice_) {
      OutputConfig c = last_cfg_;
      c.device_hint = want;
      c.master_channel = want.empty() ? 0 : master;
      c.phones_channel = want.empty() ? 2 : phones;
      choice_ = want;
      engine_.start(c, error);
      publish_device();
      return;
    }
  }
  if (!engine_.running()) {
    engine_.start(last_cfg_, error);
    publish_device();
  }
}

// ---- control socket ---------------------------------------------------------------------------------------------------
void Host::server_main() {
  std::uint64_t last_check = now_ns();
  while (!server_stop_) {
    Conn c;
    if (listener_.accept(c, 100)) {
      for (auto it = handlers_.begin(); it != handlers_.end();) {
        if (it->done->load()) {
          it->th.join();
          it = handlers_.erase(it);
        } else {
          ++it;
        }
      }
      if (handlers_.size() >= 16) continue;  // refuse: the connection closes
      auto done = std::make_shared<std::atomic<bool>>(false);
      handlers_.push_back({std::thread([this, done, conn = std::make_shared<Conn>(std::move(c))] {
                             handle_conn(*conn);
                             done->store(true);
                           }),
                           done});
    }
    const std::uint64_t t = now_ns();
    if (recheck_.exchange(false) || t - last_check >= 2000ull * 1000000ull) {
      last_check = t;
      audio_recheck();
    }
  }
}

void Host::handle_conn(Conn& c) {
  while (!server_stop_) {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> payload;
    bool timed_out = false;
    if (!recv_frame(c, type, payload, 500, &timed_out)) {
      if (timed_out) continue;
      return;
    }
    Rd rd(payload.data(), payload.size());
    Wr out;
    dispatch(type, rd, out);
    if (!send_frame(c, type, out.bytes())) return;
  }
}

void Host::dispatch(std::uint8_t type, Rd& rd, Wr& out) {
  switch (type) {
    case kReqHello:
      out.u32(kProtocolVersion);
      out.u64(sh_->hdr.session_id.load());
      out.u32(current_pid());
      return;
    case kReqPing: return;
    case kReqAttach: {
      const std::uint32_t pid = rd.u32(), version = rd.u32();
      const std::uint64_t t = now_ns();
      const std::uint32_t cur = sh_->hdr.ui_pid.load();
      if (!rd.ok() || version != kProtocolVersion) {
        out.u8(0), out.str("protocol mismatch"), out.u32(0);
        return;
      }
      if (cur != 0 && cur != pid && process_alive(cur) && t - sh_->hdr.ui_heartbeat_ns.load() < 5000ull * 1000000ull) {
        out.u8(0), out.str("another DeckHand window is attached to this engine"), out.u32(0);
        return;
      }
      const std::uint32_t epoch = sh_->hdr.ui_epoch.fetch_add(1) + 1;  // producers of the previous UI stop here
      const std::uint32_t acks = reset_acks_.load();
      reset_request_ = true;
      for (int i = 0; i < 250 && reset_acks_.load() == acks; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      sh_->hdr.ui_pid.store(pid);
      sh_->hdr.ui_heartbeat_ns.store(t);
      out.u8(1), out.str(""), out.u32(epoch);
      DH_LOG(Info, "UI pid %u attached (epoch %u)", pid, epoch);
      return;
    }
    case kReqDetach: sh_->hdr.ui_pid.store(0); return;
    case kReqOpenDevice: {
      OutputConfig cfg;
      cfg.device_hint = rd.str();
      cfg.master_channel = std::clamp(rd.i32(), 0, 62);
      cfg.phones_channel = std::clamp(rd.i32(), -1, 62);
      cfg.period_frames = rd.u32();
      cfg.follow_controller = rd.u8() != 0;
      std::string error;
      if (!rd.ok()) {
        out.u8(0), out.str("bad request");
        return;
      }
      const bool ok = open_device(cfg, error);
      out.u8(ok ? 1 : 0), out.str(error);
      return;
    }
    case kReqCloseDevice: {
      std::lock_guard<std::mutex> lock(dev_mutex_);
      want_open_ = false;
      engine_.stop();
      publish_device();
      return;
    }
    case kReqDevices: {
      std::vector<std::string> d;
      if (opt_.null_audio) d.push_back("Null Device");
      else d = engine_.devices();
      out.u32(static_cast<std::uint32_t>(std::min<std::size_t>(d.size(), 256)));
      for (std::size_t i = 0; i < d.size() && i < 256; ++i) out.str(d[i]);
      return;
    }
    case kReqRecOpen: {
      const std::string path = rd.str();
      const bool pre = rd.u8() != 0;
      std::string error;
      if (!rd.ok() || path.empty() || path.size() > 1000 || path[0] != '/' || path.find('\0') != std::string::npos) {
        out.u8(0), out.str("bad recording path");
        return;
      }
      std::lock_guard<std::mutex> lock(rec_mutex_);
      const bool ok = engine_.record_open(path, pre, error);
      if (ok) rec_path_ = path;
      out.u8(ok ? 1 : 0), out.str(error);
      return;
    }
    case kReqRecStop: engine_.record_stop(); return;
    case kReqRecClose: {
      std::lock_guard<std::mutex> lock(rec_mutex_);
      engine_.record_close();
      return;
    }
    case kReqSamplerLoad: {
      const int slot = rd.i32(), block = rd.i32();
      const std::uint64_t gen = rd.u64();
      std::string error;
      if (!rd.ok() || slot < 0 || slot >= kSamplerSlots || block < 0 || block >= kBlockSlots) {
        out.u8(0), out.str("bad request");
        return;
      }
      const std::string name = names_.block(block);
      auto s = map_sample_block(name, block, gen, error);
      if (s || error != "stale block") Shm::remove(name);
      if (!s) {
        out.u8(0), out.str(error);
        return;
      }
      engine_.sampler_load(slot, std::move(s));
      out.u8(1);
      return;
    }
    case kReqSchedule: {
      WireCommand w;
      w.op = rd.u8();
      w.deck = rd.u8();
      w.i = rd.i32();
      w.f = static_cast<float>(rd.f64());
      w.d = rd.f64();
      Schedule s;
      const std::uint8_t kind = rd.u8();
      s.clock_frame = rd.u64();
      s.deck = rd.u8();
      s.frame = rd.f64();
      s.beat = rd.f64();
      const std::uint8_t flags = rd.u8();
      Command c;
      if (!rd.ok() || kind > static_cast<std::uint8_t>(ScheduleKind::DeckBeat) || !validate_command(w, c) ||
          !std::isfinite(s.frame) || !std::isfinite(s.beat)) {
        out.u64(0);
        return;
      }
      s.kind = static_cast<ScheduleKind>(kind);
      s.relative = flags & 1;
      s.bar_aligned = flags & 2;
      s.fire_if_passed = flags & 4;
      out.u64(engine_.schedule(c, s));
      return;
    }
    case kReqCancel: {
      const std::uint64_t id = rd.u64();
      out.u8(rd.ok() && engine_.cancel(id) ? 1 : 0);
      return;
    }
    case kReqCancelAll: engine_.cancel_all(); return;
    case kReqClockAtWall: {
      const std::int64_t ns = rd.i64();
      out.u64(engine_.clock_at_wall(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns))));
      return;
    }
    case kReqStreamEnable: {
      const bool on = rd.u8() != 0, pre = rd.u8() != 0;
      engine_.stream_enable(on, pre);
      stream_on_ = on;
      return;
    }
    case kReqQuit:
      quit_fade_ms_ = static_cast<int>(std::min<std::uint32_t>(rd.u32(), 2000));
      quit_ = true;
      return;
    case kReqHubLearnBegin: {
      const std::string action = rd.str();
      std::string error;
      bool ok = false;
      if (hub_ && rd.ok()) {
        std::lock_guard<std::mutex> lock(hub_mutex_);
        ok = hub_->learn_begin(action);
        if (!ok) error = hub_->learn_error();
      } else {
        error = "no controller support in this engine";
      }
      publish_hub();
      out.u8(ok ? 1 : 0), out.str(error);
      return;
    }
    case kReqHubLearnCancel:
      if (hub_) {
        std::lock_guard<std::mutex> lock(hub_mutex_);
        hub_->learn_cancel();
      }
      publish_hub();
      return;
    case kReqHubLearnCommit: {
      bool ok = false;
      if (hub_) {
        std::lock_guard<std::mutex> lock(hub_mutex_);
        ok = hub_->learn_commit();
      }
      publish_hub();
      out.u8(ok ? 1 : 0);
      return;
    }
    case kReqHubLearnResult: {
      const bool finalize = rd.u8() != 0;
      std::string line;
      if (hub_) {
        std::lock_guard<std::mutex> lock(hub_mutex_);
        line = hub_->learn_result(finalize);
      }
      publish_hub();
      out.str(line);
      return;
    }
    default: return;  // unknown request: empty reply
  }
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--data-dir") o.data_dir = value();
    else if (a == "--resources") o.resource_dir = value();
    else if (a == "--log-level") o.log_level = std::atoi(value().c_str());
    else if (a == "--null-audio") o.null_audio = true;
    else if (a == "--no-midi") o.no_midi = true;
    else if (a == "--probe") o.probe = true;
    else if (a == "--version") {
      std::printf("deckhand-engine protocol %u\n", kProtocolVersion);
      std::exit(0);
    } else {
      std::fprintf(stderr, "usage: deckhand-engine --data-dir DIR [--resources DIR] [--log-level N] [--null-audio] [--no-midi] [--probe]\n");
      return false;
    }
  }
  if (o.data_dir.empty()) {
    std::fprintf(stderr, "deckhand-engine: --data-dir is required\n");
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parse_args(argc, argv, o)) return 2;
  setsid();  // own session: closing the UI's terminal or process group must not take the engine down
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  if (o.probe) {  // diagnostics: which per-session objects exist for this data folder
    SessionNames n;
    std::string e;
    if (!session_names(o.data_dir, n, e)) return 2;
    Shm shm;
    int blocks = 0;
    const bool main_seg = Shm::open(n.shm_main, false, shm, e);
    for (int i = 0; i < kBlockSlots; ++i) {
      Shm b;
      if (Shm::open(n.block(i), false, b, e)) ++blocks;
    }
    Conn c;
    const bool sock = Conn::connect(n.socket, c);
    std::printf("engine %s, main segment %s, %d block segments, socket %s\n", sock ? "running" : "not running",
                main_seg ? "present" : "absent", blocks, std::filesystem::exists(n.socket) ? "present" : "absent");
    return 0;
  }

  Host host(std::move(o));
  std::string error;
  if (!host.init(error)) {
    // Losing the single-instance race is normal (two UIs starting at once); anything else is worth a line.
    std::fprintf(stderr, "deckhand-engine: %s\n", error.c_str());
    return 3;
  }
  host.run();
  return 0;
}
