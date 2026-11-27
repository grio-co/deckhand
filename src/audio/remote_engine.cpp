#include "deckhand/audio/remote_engine.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <filesystem>

#include "deckhand/audio/ipc.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {

using namespace ipc;

struct RemoteEngine::Names : SessionNames {};

struct RemoteEngine::Link {
  Shm shm;
  Shared* sh = nullptr;
  Conn conn;
  std::mutex rpc_mutex;
  std::uint32_t pid = 0, epoch = 0;
  std::uint64_t session = 0;
  bool ours = false;  // we spawned it, so it is our child until reaped
  std::uint64_t born_ns = 0;
};

namespace {
std::string text_of(const char* p, std::size_t cap, std::uint32_t n) { return std::string(p, std::min<std::size_t>(n, cap)); }
std::string cstr(const char* p, std::size_t cap) { return std::string(p, strnlen(p, cap)); }
}  // namespace

RemoteEngine::RemoteEngine(const Options& options)
    : opt_(options), names_(std::make_unique<Names>()), generation_(now_ns()) {
  if (!session_names(opt_.data_dir, *names_, names_error_)) DH_LOG(Error, "engine ipc: %s", names_error_.c_str());
}

RemoteEngine::~RemoteEngine() { detach(); }

std::shared_ptr<RemoteEngine::Link> RemoteEngine::link() const {
  std::lock_guard<std::mutex> lock(link_mutex_);
  return link_;
}

bool RemoteEngine::connected() const { return link() != nullptr; }
std::uint32_t RemoteEngine::engine_pid() const {
  const auto l = link();
  return l ? l->pid : 0;
}
std::uint64_t RemoteEngine::session_id() const {
  const auto l = link();
  return l ? l->session : 0;
}

void RemoteEngine::set_status_callback(StatusFn fn) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  status_fn_ = std::move(fn);
}
void RemoteEngine::set_ui_request_handler(UiRequestFn fn) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  ui_fn_ = std::move(fn);
}
void RemoteEngine::notify(Status s, const std::string& message) {
  StatusFn fn;
  {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    fn = status_fn_;
  }
  DH_LOG(Info, "engine: %s", message.c_str());
  if (fn) fn(s, message);
}

// ---- connection ---------------------------------------------------------------------------------------
bool RemoteEngine::rpc(Link& l, std::uint8_t type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& reply,
                       int timeout_ms) {
  std::lock_guard<std::mutex> lock(l.rpc_mutex);
  if (!l.conn.valid()) return false;
  std::uint8_t got = 0;
  if (!send_frame(l.conn, type, payload) || !recv_frame(l.conn, got, reply, timeout_ms) || got != type) {
    l.conn.close();  // the stream is out of step; the monitor reconnects if the engine is gone
    return false;
  }
  return true;
}

bool RemoteEngine::rpc(std::uint8_t type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& reply, int timeout_ms) {
  const auto l = link();
  return l && rpc(*l, type, payload, reply, timeout_ms);
}

bool RemoteEngine::establish(std::string& error, bool allow_spawn, bool& spawned) {
  spawned = false;
  if (!names_error_.empty()) {
    error = names_error_;
    return false;
  }
  auto l = std::make_shared<Link>();
  const auto hello = [&](bool& version_mismatch) {
    version_mismatch = false;
    std::vector<std::uint8_t> r;
    if (!rpc(*l, kReqHello, {}, r, 2000)) return false;
    Rd rd(r.data(), r.size());
    const std::uint32_t version = rd.u32();
    l->session = rd.u64();
    l->pid = rd.u32();
    if (!rd.ok()) return false;
    if (version != kProtocolVersion) {
      version_mismatch = true;
      return false;
    }
    return true;
  };

  bool need_spawn = false;
  if (Conn::connect(names_->socket, l->conn)) {
    bool mismatch = false;
    if (!hello(mismatch)) {
      if (!mismatch) {
        error = "the engine did not answer";
        return false;
      }
      // An engine from another build is running: ask it to leave (the framing is stable across versions).
      Wr q;
      q.u32(0);
      std::vector<std::uint8_t> r;
      const std::uint32_t old_pid = l->pid;
      rpc(*l, kReqQuit, q.bytes(), r, 1000);
      for (int i = 0; i < 150 && process_alive(old_pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      l->conn.close();
      need_spawn = true;
    } else {
      attached_existing_ = true;
    }
  } else {
    need_spawn = true;
  }

  if (need_spawn) {
    if (!allow_spawn) {
      error = "no engine is running";
      return false;
    }
    attached_existing_ = false;
    std::string helper = opt_.helper_path;
    if (helper.empty()) helper = executable_dir() + "/deckhand-engine";
#if defined(_WIN32)
    // spawn_detached adds ".exe" and reports a missing file; std::filesystem::exists would read UTF-8 as the ANSI page.
#else
    if (!std::filesystem::exists(helper)) {
      error = "engine helper not found: " + helper;
      return false;
    }
#endif
    std::vector<std::string> args = {"--data-dir", opt_.data_dir, "--log-level", std::to_string(opt_.log_level)};
    if (!opt_.resource_dir.empty()) {
      args.push_back("--resources");
      args.push_back(opt_.resource_dir);
    }
    if (opt_.null_audio) args.push_back("--null-audio");
    for (const std::string& a : opt_.extra_args) args.push_back(a);
    // A helper that comes up after we gave up must not stay beside the in-process fallback: it leaves by itself when no
    // UI attached (and nothing is loaded) shortly after our own wait would have ended.
    args.push_back("--exit-unattached");
    args.push_back(std::to_string(opt_.start_wait_s + 15));
    std::error_code rm_ec;
    std::filesystem::remove(std::filesystem::u8path(opt_.data_dir) / "deckhand-engine.abandon", rm_ec);  // a note from an earlier run
    std::uint32_t child = 0;
    if (!spawn_detached(helper, args, opt_.data_dir + "/deckhand-engine.out", child, error)) return false;
    spawned = true;
    // On a heavily loaded machine the helper has taken over 20 s to open its devices and answer.
    // The loop below already stops early if the helper exits, so a generous limit costs nothing
    // in the normal case and avoids falling back to in-process audio with a late helper left running.
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(opt_.start_wait_s);
    bool up = false;
    int reported = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const int waited = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
      if (waited >= 2 && waited != reported) {  // once a second: a host that can paint shows "Starting the audio engine..."
        reported = waited;
        if (opt_.on_start_wait) opt_.on_start_wait(waited);
      }
      bool mismatch = false;
      if (Conn::connect(names_->socket, l->conn)) {
        if (hello(mismatch)) {
          up = true;
          break;
        }
        l->conn.close();
      }
      if (reap_child(child)) {
        error = "the engine exited while starting (see deckhand-engine.log)";
        // Another engine may have won the race for the single-instance lock: one more try to attach.
        if (Conn::connect(names_->socket, l->conn) && hello(mismatch)) {
          up = true;
          spawned = false;
          attached_existing_ = true;
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!up) {
      if (error.empty()) {
        error = "the engine did not start in time";
        // Tell the late helper to go: it checks for this note once it runs, and also leaves by itself (see above).
        const std::filesystem::path note = std::filesystem::u8path(opt_.data_dir) / "deckhand-engine.abandon";
        std::ofstream(note, std::ios::binary | std::ios::trunc) << child;  // closed at the end of the statement
      }
      return false;
    }
    l->ours = spawned;
  }

  std::string shm_error;
  if (!Shm::open(names_->shm_main, /*writable=*/true, l->shm, shm_error) || !check_shared(l->shm.data(), l->shm.size(), shm_error)) {
    error = "cannot attach to the engine: " + shm_error;
    return false;
  }
  l->sh = static_cast<Shared*>(l->shm.data());

  Wr a;
  a.u32(current_pid());
  a.u32(kProtocolVersion);
  std::vector<std::uint8_t> r;
  if (!rpc(*l, kReqAttach, a.bytes(), r, 3000)) {
    error = "the engine did not answer";
    return false;
  }
  Rd rd(r.data(), r.size());
  const bool ok = rd.u8() != 0;
  const std::string why = rd.str();
  l->epoch = rd.u32();
  if (!ok || !rd.ok()) {
    error = why.empty() ? "attach refused" : why;
    return false;
  }
  l->born_ns = l->sh->hdr.engine_start_ns.load();
  l->sh->hdr.ui_heartbeat_ns.store(now_ns());
  // Blocks a dead UI left behind: nothing of ours is in flight yet.
  for (int i = 0; i < kBlockSlots; ++i) Shm::remove(names_->block(i));
  {
    std::lock_guard<std::mutex> lock(link_mutex_);
    link_ = std::move(l);
  }
  return true;
}

bool RemoteEngine::connect(std::string& error) {
  bool spawned = false;
  if (!establish(error, true, spawned)) return false;
  notify(spawned ? Status::Spawned : Status::Attached,
         spawned ? "audio engine started" : "attached to the running audio engine");
  if (!supervise_.exchange(true)) monitor_ = std::thread([this] { monitor_main(); });
  return true;
}

void RemoteEngine::monitor_main() {
  int quick_failures = 0;
  std::uint64_t last_start_ns = now_ns();
  while (!stopping_) {
    {
      std::unique_lock<std::mutex> lock(wake_mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping_.load(); });
    }
    if (stopping_) break;
    const auto l = link();
    if (l) snapshot_chains(false);
    if (l) {
      const bool gone = (l->ours && reap_child(l->pid)) || !process_alive(l->pid) ||
                        l->sh->hdr.session_id.load() != l->session;
      if (!gone) continue;
      {
        std::lock_guard<std::mutex> lock(link_mutex_);
        link_.reset();
      }
      const double ran = static_cast<double>(now_ns() - last_start_ns) / 1e9;
      quick_failures = ran < 15.0 ? quick_failures + 1 : 1;
      notify(Status::Died, "the audio engine stopped unexpectedly");
      if (!opt_.auto_restart) continue;
    } else if (!opt_.auto_restart) {
      continue;
    }
    if (quick_failures >= 5) {
      notify(Status::GaveUp, "the audio engine keeps failing; not restarting it again");
      opt_.auto_restart = false;
      continue;
    }
    // Back off: 0.5 s, 1 s, 2 s, 4 s between quick failures.
    const int delay_ms = std::min(8000, opt_.backoff_ms << std::max(0, std::min(quick_failures, 6) - 1));
    for (int waited = 0; waited < delay_ms && !stopping_; waited += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (stopping_) break;
    std::string error;
    bool spawned = false;
    last_start_ns = now_ns();
    if (establish(error, true, spawned)) {
      if (spawned) restore_chains();
      notify(Status::Restarted, spawned ? "the audio engine was restarted; load your tracks again" : "re-attached to the audio engine");
    } else {
      DH_LOG(Warn, "engine restart failed: %s", error.c_str());
      ++quick_failures;
    }
  }
}

void RemoteEngine::snapshot_chains(bool force) {
  const auto now = std::chrono::steady_clock::now();
  for (int t = 0; t < kFxTargetCount; ++t) {
    if (!fx_target_valid(t)) continue;
    {
      std::lock_guard<std::mutex> lock(snap_mutex_);
      if (!snap_dirty_[t] || (!force && now - snap_edit_[t] < std::chrono::milliseconds(300))) continue;
      snap_dirty_[t] = false;
    }
    const FxChainInfo info = fx_chain(t);
    std::string text = info.exists || info.count > 0 ? fx_chain_save_full(*this, t) : std::string();
    std::lock_guard<std::mutex> lock(snap_mutex_);
    if (!snap_dirty_[t]) snap_[t] = std::move(text);
  }
}

void RemoteEngine::restore_chains() {
  for (int t = 0; t < kFxTargetCount; ++t) {
    std::string text;
    {
      std::lock_guard<std::mutex> lock(snap_mutex_);
      text = snap_[t];
    }
    if (text.empty()) continue;
    std::string err;
    if (!fx_chain_load_full(*this, t, text, &err)) DH_LOG(Warn, "could not restore effect chain %d after the engine restart: %s", t, err.c_str());
    else if (!err.empty()) DH_LOG(Warn, "effect chain %d restored with notes: %s", t, err.c_str());
  }
}

void RemoteEngine::heartbeat() {
  if (const auto l = link()) l->sh->hdr.ui_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
}

void RemoteEngine::detach() {
  stopping_ = true;
  wake_.notify_all();
  if (monitor_.joinable()) monitor_.join();
  if (const auto l = link()) {
    std::vector<std::uint8_t> r;
    rpc(*l, kReqDetach, {}, r, 500);
  }
  std::lock_guard<std::mutex> lock(link_mutex_);
  link_.reset();
}

void RemoteEngine::shutdown_remote(int fade_ms) {
  stopping_ = true;
  wake_.notify_all();
  if (monitor_.joinable()) monitor_.join();
  const auto l = link();
  if (!l) return;
  Wr q;
  q.u32(static_cast<std::uint32_t>(std::max(0, fade_ms)));
  std::vector<std::uint8_t> r;
  rpc(*l, kReqQuit, q.bytes(), r, 3000);
  for (int i = 0; i < 300; ++i) {
    if (l->ours) reap_child(l->pid);
    if (!process_alive(l->pid)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // On Windows a block's name lives as long as our handle to it (the engine's Shm::remove cannot drop it): release them.
  if (!process_alive(l->pid))
    for (int i = 0; i < kBlockSlots; ++i) Shm::remove(names_->block(i));
  std::lock_guard<std::mutex> lock(link_mutex_);
  link_.reset();
}

// ---- IEngine ------------------------------------------------------------------------------------------
bool RemoteEngine::start(const OutputConfig& c, std::string& error) {
  Wr w;
  w.str(c.device_hint);
  w.i32(c.master_channel);
  w.i32(c.phones_channel);
  w.u32(c.period_frames);
  w.u8(c.follow_controller ? 1 : 0);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqOpenDevice, w.bytes(), r, 8000)) {
    error = "the audio engine is not responding";
    return false;
  }
  Rd rd(r.data(), r.size());
  const bool ok = rd.u8() != 0;
  error = rd.str();
  return ok;
}

void RemoteEngine::stop() {
  std::vector<std::uint8_t> r;
  rpc(kReqCloseDevice, {}, r, 5000);
}

bool RemoteEngine::running() const {
  const auto l = link();
  return l && l->sh->device.read().running != 0;
}
std::string RemoteEngine::device_name() const {
  const auto l = link();
  if (!l) return {};
  const DeviceInfo d = l->sh->device.read();
  return text_of(d.name, sizeof d.name, d.name_len);
}
int RemoteEngine::output_channels() const {
  const auto l = link();
  return l ? l->sh->device.read().channels : 0;
}
std::uint32_t RemoteEngine::sample_rate() const {
  const auto l = link();
  return l ? l->sh->device.read().sample_rate : 0;
}

std::vector<std::string> RemoteEngine::devices() {
  std::vector<std::string> out;
  std::vector<std::uint8_t> r;
  if (!rpc(kReqDevices, {}, r, 4000)) return out;
  Rd rd(r.data(), r.size());
  const std::uint32_t n = rd.u32();
  for (std::uint32_t i = 0; i < n && i < 256 && rd.ok(); ++i) out.push_back(rd.str());
  return out;
}

void RemoteEngine::invalidate_chain(int target) {
  if (!fx_target_valid(target)) return;
  std::lock_guard<std::mutex> lock(chain_mutex_);
  chain_cache_[target].valid = false;
}

namespace {
bool fx_chain_rpc_call(const FxChainEdit& e, std::vector<std::uint8_t>& payload) {
  if (e.text.size() > 65535) return false;
  Wr w;
  w.u8(static_cast<std::uint8_t>(e.verb));
  w.u8(static_cast<std::uint8_t>(e.target));
  w.i32(e.id);
  w.i32(e.pos);
  w.u8(static_cast<std::uint8_t>(e.kind));
  w.i32(e.field);
  w.f64(e.value);
  w.str(e.text);
  payload = w.bytes();
  return true;
}
}  // namespace

bool RemoteEngine::fx_chain_edit(const FxChainEdit& edit, FxChainResult& result) {
  result = FxChainResult{};
  std::vector<std::uint8_t> payload, reply;
  if (!fx_chain_rpc_call(edit, payload)) {
    result.error = "request too large";
    return false;
  }
  // Loading a plug-in may probe it first (a throw-away process, seconds); a preset can name several.
  const int timeout = (edit.kind == EffectKind::Plugin || edit.verb == FxChainVerb::Load) ? 30000 : 4000;
  if (!rpc(kReqFxChain, payload, reply, timeout)) {
    result.error = "the engine did not answer";
    return false;
  }
  Rd rd(reply.data(), reply.size());
  const bool ok = rd.u8() != 0;
  result.id = rd.i32();
  result.error = rd.str();
  result.text = rd.str();
  if (!get_chain_info(rd, result.info)) {
    result = FxChainResult{};
    result.error = "bad reply from the engine";
    return false;
  }
  result.ok = ok;
  if (fx_target_valid(edit.target)) {
    {
      std::lock_guard<std::mutex> lock(chain_mutex_);
      chain_cache_[edit.target] = ChainCache{result.info, std::chrono::steady_clock::now(), true};
    }
    if (ok && edit.verb != FxChainVerb::Get && edit.verb != FxChainVerb::Save) {
      std::lock_guard<std::mutex> lock(snap_mutex_);
      snap_dirty_[edit.target] = true;
      snap_edit_[edit.target] = std::chrono::steady_clock::now();
    }
  }
  return ok;
}

bool RemoteEngine::plugin_op(const PluginOp& op, PluginOpResult& result) {
  result = PluginOpResult{};
  if (op.data.size() > kPluginBlobMax) {
    result.error = "request too large";
    return false;
  }
  Wr w;
  w.u8(static_cast<std::uint8_t>(op.kind));
  w.u8(static_cast<std::uint8_t>(op.target));
  w.i32(op.id);
  w.blob(op.data);
  std::vector<std::uint8_t> reply;
  if (!rpc(kReqPluginOp, w.bytes(), reply, 8000)) {
    result.error = "the engine did not answer";
    return false;
  }
  Rd rd(reply.data(), reply.size());
  const bool ok = rd.u8() != 0;
  result.error = rd.str();
  result.data = rd.blob(kPluginBlobMax);
  if (!rd.ok()) {
    result = PluginOpResult{};
    result.error = "bad reply from the engine";
    return false;
  }
  result.ok = ok;
  return ok;
}

FxChainInfo RemoteEngine::fx_chain(int target) const {
  if (!fx_target_valid(target)) return FxChainInfo{};
  {
    std::lock_guard<std::mutex> lock(chain_mutex_);
    const ChainCache& c = chain_cache_[target];
    if (c.valid && std::chrono::steady_clock::now() - c.at < std::chrono::milliseconds(100)) return c.info;
  }
  FxChainEdit e;
  e.verb = FxChainVerb::Get;
  e.target = target;
  FxChainResult r;
  const_cast<RemoteEngine*>(this)->fx_chain_edit(e, r);
  return r.info;
}

void RemoteEngine::send(const Command& c) {
  if (c.op >= Op::FxSelect && c.op <= Op::FxParam) invalidate_chain(c.deck);
  const auto l = link();
  if (!l || c.track || c.sample || c.op == Op::Load || c.op == Op::SamplerLoad) {
    delete c.track;  // a Command owns its payload once handed to send()
    delete c.sample;
    if (c.track || c.sample) DH_LOG(Warn, "engine: pointer commands must use load() / sampler_load()");
    return;
  }
  if (l->sh->hdr.ui_epoch.load(std::memory_order_relaxed) != l->epoch) return;  // another UI took over
  if (!l->sh->cmd.push(to_wire(c))) {
    ++dropped_;
    DH_LOG(Warn, "engine command ring full; dropped a command");
  }
}

int RemoteEngine::next_slot() {
  std::lock_guard<std::mutex> lock(block_mutex_);
  return slot_cursor_++ % kBlockSlots;
}

void RemoteEngine::load(int deck, std::unique_ptr<LoadedTrack> track) { share_track(Op::Load, deck, std::move(track)); }
void RemoteEngine::preview_load(std::unique_ptr<LoadedTrack> track) { share_track(Op::PreviewLoad, 0, std::move(track)); }
PreviewView RemoteEngine::preview() const {
  const auto l = link();
  return l ? l->sh->preview.read() : PreviewView{};
}

// Copies a decoded track into a shared-memory block and tells the engine to take it (as a deck's track or the preview).
void RemoteEngine::share_track(Op op, int deck, std::unique_ptr<LoadedTrack> track) {
  const auto l = link();
  if (!l || !track || !track->audio || deck < 0 || deck >= kMaxDecks) return;
  std::string error;
  int slot = -1;
  std::uint64_t gen = 0;
  for (int attempt = 0; attempt < kBlockSlots; ++attempt) {
    const int s = next_slot();
    gen = ++generation_;
    if (create_track_block(names_->block(s), s, gen, *track, error)) {
      slot = s;
      break;
    }
  }
  if (slot < 0) {
    DH_LOG(Error, "engine: could not share the track with the engine: %s", error.c_str());
    return;
  }
  WireCommand w;
  w.op = static_cast<std::uint8_t>(op);
  w.deck = static_cast<std::uint8_t>(deck);
  w.i = slot;
  w.aux = gen;
  if (l->sh->hdr.ui_epoch.load(std::memory_order_relaxed) != l->epoch || !l->sh->cmd.push(w)) {
    Shm::remove(names_->block(slot));
    ++dropped_;
  }
}

bool RemoteEngine::stems_attach(int deck, std::int64_t track_id, const std::string& path, std::string& error) {
  if (deck < 0 || deck >= kMaxDecks || path.empty() || path.size() > 1024) {
    error = "bad request";
    return false;
  }
  Wr w;
  w.u8(static_cast<std::uint8_t>(deck));
  w.i64(track_id);
  w.str(path);
  std::vector<std::uint8_t> reply;
  if (!rpc(kReqStemsAttach, w.bytes(), reply, 4000)) {
    error = "the engine did not answer";
    return false;
  }
  Rd rd(reply.data(), reply.size());
  const bool ok = rd.u8() != 0;
  error = rd.str();
  return ok && rd.ok();
}

void RemoteEngine::set_track_meta(int deck, std::int64_t track_id, const TrackMeta& meta) {
  if (deck < 0 || deck >= kMaxDecks || meta.grid.size() > kMaxMetaGrid || meta.memory_cues.size() > kMaxMetaCues) return;
  Wr w;
  w.u8(static_cast<std::uint8_t>(deck));
  w.i64(track_id);
  w.f64(meta.bpm);
  w.u32(static_cast<std::uint32_t>(meta.grid.size()));
  for (const GridSection& g : meta.grid) {
    w.f64(g.first_beat_frame);
    w.f64(g.bpm);
    w.i32(g.bar_beat);
  }
  w.u32(static_cast<std::uint32_t>(meta.memory_cues.size()));
  for (double c : meta.memory_cues) w.f64(c);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqSetTrackMeta, w.bytes(), r, 3000)) DH_LOG(Warn, "engine: could not send the beat grid to the engine");
}

void RemoteEngine::sampler_load(int slot, std::unique_ptr<LoadedSample> sample) {
  const auto l = link();
  if (!l || !sample || !sample->audio || slot < 0 || slot >= kSamplerSlots) return;
  std::string error;
  int block = -1;
  std::uint64_t gen = 0;
  for (int attempt = 0; attempt < kBlockSlots; ++attempt) {
    const int s = next_slot();
    gen = ++generation_;
    if (create_sample_block(names_->block(s), s, gen, *sample, error)) {
      block = s;
      break;
    }
  }
  if (block < 0) {
    DH_LOG(Error, "engine: could not share the sample with the engine: %s", error.c_str());
    return;
  }
  Wr w;
  w.i32(slot);
  w.i32(block);
  w.u64(gen);
  std::vector<std::uint8_t> r;
  if (!rpc(*l, kReqSamplerLoad, w.bytes(), r, 10000)) {
    Shm::remove(names_->block(block));
    return;
  }
  Rd rd(r.data(), r.size());
  if (rd.u8() == 0) {
    const std::string why = rd.str();
    notify(Status::LoadRejected, "the engine refused the sample: " + why);
  }
}

StepSeqView RemoteEngine::step_seq() const {
  const auto l = link();
  return l ? l->sh->stepseq.read() : StepSeqView{};
}

bool RemoteEngine::seq_pattern_set(int pattern, const std::string& text) {
  const auto l = link();
  SeqPatternData probe;
  if (!l || pattern < 0 || pattern >= kSeqPatterns || !seq_pattern_from_text(text, probe)) return false;
  Wr w;
  w.i32(pattern);
  w.str(text);
  std::vector<std::uint8_t> r;
  if (!rpc(*l, kReqSeqPattern, w.bytes(), r, 3000)) return false;
  Rd rd(r.data(), r.size());
  return rd.u8() != 0;
}

bool RemoteEngine::deck_info(int deck, std::int64_t& track_id, std::string& path) const {
  const auto l = link();
  if (!l || deck < 0 || deck >= kMaxDecks) return false;
  const DeckMeta m = l->sh->meta[deck].read();
  if (!m.loaded) return false;
  track_id = m.track_id;
  path = text_of(m.path, sizeof m.path, m.path_len);
  return true;
}

DeckView RemoteEngine::deck(int index) const {
  const auto l = link();
  return l ? l->sh->deck[std::clamp(index, 0, kMaxDecks - 1)].read() : DeckView{};
}
MixerView RemoteEngine::mixer() const {
  const auto l = link();
  return l ? l->sh->mixer.read() : MixerView{};
}
SamplerView RemoteEngine::sampler(int slot) const {
  const auto l = link();
  return l ? l->sh->sampler[std::clamp(slot, 0, kSamplerSlots - 1)].read() : SamplerView{};
}

bool RemoteEngine::record_open(const std::string& path, bool pre_clip, std::string& error, bool include_mic) {
  Wr w;
  w.str(path);
  w.u8(pre_clip ? 1 : 0);
  w.u8(include_mic ? 1 : 0);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqRecOpen, w.bytes(), r, 5000)) {
    error = "the audio engine is not responding";
    return false;
  }
  Rd rd(r.data(), r.size());
  const bool ok = rd.u8() != 0;
  error = rd.str();
  return ok;
}
void RemoteEngine::record_stop() {
  std::vector<std::uint8_t> r;
  rpc(kReqRecStop, {}, r, 3000);
}
void RemoteEngine::record_close() {
  std::vector<std::uint8_t> r;
  rpc(kReqRecClose, {}, r, 8000);
}
RecorderStatus RemoteEngine::recorder_status() const {
  const auto l = link();
  return l ? l->sh->rec.read().st : RecorderStatus{};
}
std::string RemoteEngine::recorder_path() const {
  const auto l = link();
  if (!l) return {};
  const RecInfo r = l->sh->rec.read();
  return text_of(r.path, sizeof r.path, r.path_len);
}

void RemoteEngine::stream_enable(bool on, bool pre_clip, bool include_mic) {
  Wr w;
  w.u8(on ? 1 : 0);
  w.u8(pre_clip ? 1 : 0);
  w.u8(include_mic ? 1 : 0);
  std::vector<std::uint8_t> r;
  rpc(kReqStreamEnable, w.bytes(), r, 3000);
  if (const auto l = link()) l->sh->stream.discard();
}
std::size_t RemoteEngine::stream_read(float* out, std::size_t max_frames) {
  const auto l = link();
  return l ? l->sh->stream.pop(out, max_frames) : 0;
}
std::uint64_t RemoteEngine::stream_dropped_frames() const {
  const auto l = link();
  return l ? l->sh->hdr.stream_dropped.load() : 0;
}

bool RemoteEngine::open_input(const InputConfig& config, std::string& error) {
  Wr w;
  w.str(config.device_hint);
  w.i32(config.channel);
  w.u8(config.stereo ? 1 : 0);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqOpenInput, w.bytes(), r, 8000)) {
    error = "the audio engine is not responding";
    return false;
  }
  Rd rd(r.data(), r.size());
  const bool ok = rd.u8() != 0;
  error = rd.str();
  return ok && rd.ok();
}
void RemoteEngine::close_input() {
  std::vector<std::uint8_t> r;
  rpc(kReqCloseInput, {}, r, 5000);
}
std::vector<std::string> RemoteEngine::input_devices() {
  std::vector<std::string> out;
  std::vector<std::uint8_t> r;
  if (!rpc(kReqInputDevices, {}, r, 4000)) return out;
  Rd rd(r.data(), r.size());
  const std::uint32_t n = rd.u32();
  for (std::uint32_t i = 0; i < n && i < 256 && rd.ok(); ++i) out.push_back(rd.str());
  return out;
}
InputStatus RemoteEngine::input_status() const {
  InputStatus st;
  const auto l = link();
  if (!l) return st;
  const InputInfo in = l->sh->input.read();
  st.open = in.open != 0;
  st.lost = in.lost != 0;
  st.sample_rate = in.sample_rate;
  st.latency_ms = std::isfinite(in.latency_ms) ? in.latency_ms : 0;
  st.underruns = in.underruns;
  st.overruns = in.overruns;
  st.name = text_of(in.name, sizeof in.name, in.name_len);
  return st;
}

std::uint64_t RemoteEngine::clock_frames() const {
  const auto l = link();
  return l ? l->sh->hdr.clock_frames.load(std::memory_order_acquire) : 0;
}
double RemoteEngine::clock_seconds() const {
  const std::uint32_t sr = sample_rate();
  return static_cast<double>(clock_frames()) / (sr ? sr : 44100);
}
std::uint64_t RemoteEngine::clock_at_wall(std::chrono::steady_clock::time_point t) const {
  Wr w;
  w.i64(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
  std::vector<std::uint8_t> r;
  const auto l = link();
  if (!l || !const_cast<RemoteEngine*>(this)->rpc(*l, kReqClockAtWall, w.bytes(), r, 2000)) return clock_frames();
  Rd rd(r.data(), r.size());
  const std::uint64_t f = rd.u64();
  return rd.ok() ? f : clock_frames();
}

std::uint64_t RemoteEngine::schedule(const Command& c, const Schedule& when) {
  if (c.track || c.sample || c.op == Op::Load || c.op == Op::SamplerLoad) return 0;
  const WireCommand wc = to_wire(c);
  Wr w;
  w.u8(wc.op);
  w.u8(wc.deck);
  w.i32(wc.i);
  w.f64(wc.f);
  w.f64(wc.d);
  w.u8(static_cast<std::uint8_t>(when.kind));
  w.u64(when.clock_frame);
  w.u8(when.deck);
  w.f64(when.frame);
  w.f64(when.beat);
  w.u8((when.relative ? 1 : 0) | (when.bar_aligned ? 2 : 0) | (when.fire_if_passed ? 4 : 0));
  std::vector<std::uint8_t> r;
  if (!rpc(kReqSchedule, w.bytes(), r, 3000)) return 0;
  Rd rd(r.data(), r.size());
  const std::uint64_t id = rd.u64();
  return rd.ok() ? id : 0;
}
bool RemoteEngine::cancel(std::uint64_t id) {
  Wr w;
  w.u64(id);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqCancel, w.bytes(), r, 3000)) return false;
  Rd rd(r.data(), r.size());
  return rd.u8() != 0;
}
std::size_t RemoteEngine::scheduled_count() const {
  const auto l = link();
  return l ? l->sh->hdr.scheduled_count.load() : 0;
}
void RemoteEngine::cancel_all() {
  std::vector<std::uint8_t> r;
  rpc(kReqCancelAll, {}, r, 3000);
}

bool RemoteEngine::poll(EngineEvent& out) {
  const auto l = link();
  if (!l) return false;
  WireEvent e;
  while (l->sh->events.pop(e)) {
    if (e.kind <= static_cast<std::uint32_t>(EventType::ScheduledDropped)) {
      out = EngineEvent{};
      out.type = static_cast<EventType>(e.kind);
      out.deck = static_cast<std::uint8_t>(e.deck);
      out.slot = e.slot;
      out.frame = e.frame;
      out.track_id = e.track_id;
      out.sched_id = e.sched_id;
      out.clock_frame = e.clock_frame;
      return true;
    }
    if (e.kind == kEvLoadResult) {
      if (e.clock_frame != 0) notify(Status::LoadRejected, "the engine refused a track");
      continue;
    }
    UiRequest req;
    if (e.kind == kEvBrowseSteps) {
      req.kind = UiRequest::Browse;
      req.value = e.slot;
    } else if (e.kind == kEvBrowsePress) {
      req.kind = UiRequest::BrowsePress;
    } else if (e.kind == kEvLoadSelected) {
      req.kind = UiRequest::LoadSelected;
      req.value = static_cast<int>(e.deck);
    } else {
      continue;
    }
    UiRequestFn fn;
    {
      std::lock_guard<std::mutex> lock(cb_mutex_);
      fn = ui_fn_;
    }
    if (fn) fn(req);
  }
  return false;
}

// ---- controller hub -----------------------------------------------------------------------------------
bool RemoteEngine::hub_learn_begin(const std::string& action, std::string& error) {
  Wr w;
  w.str(action);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqHubLearnBegin, w.bytes(), r, 3000)) {
    error = "the audio engine is not responding";
    return false;
  }
  Rd rd(r.data(), r.size());
  const bool ok = rd.u8() != 0;
  error = rd.str();
  return ok;
}
void RemoteEngine::hub_learn_cancel() {
  std::vector<std::uint8_t> r;
  rpc(kReqHubLearnCancel, {}, r, 3000);
}
bool RemoteEngine::hub_learn_commit() {
  std::vector<std::uint8_t> r;
  if (!rpc(kReqHubLearnCommit, {}, r, 3000)) return false;
  Rd rd(r.data(), r.size());
  return rd.u8() != 0;
}
std::string RemoteEngine::hub_learn_result(bool finalize) {
  if (finalize) {
    Wr w;
    w.u8(1);
    std::vector<std::uint8_t> r;
    if (rpc(kReqHubLearnResult, w.bytes(), r, 3000)) {
      Rd rd(r.data(), r.size());
      return rd.str();
    }
    return {};
  }
  const auto l = link();
  if (!l) return {};
  const HubInfo h = l->sh->hub.read();
  return cstr(h.learn_line, sizeof h.learn_line);
}
int RemoteEngine::hub_learn_state() const {
  const auto l = link();
  return l ? static_cast<int>(l->sh->hub.read().learn_state) : 0;
}
std::string RemoteEngine::hub_learn_device() const {
  const auto l = link();
  if (!l) return {};
  const HubInfo h = l->sh->hub.read();
  return cstr(h.learn_device, sizeof h.learn_device);
}
std::string RemoteEngine::hub_learn_error() const {
  const auto l = link();
  if (!l) return {};
  const HubInfo h = l->sh->hub.read();
  return cstr(h.learn_error, sizeof h.learn_error);
}
bool RemoteEngine::hub_set_options(const std::string& mapping, const std::string& options_text, std::string* applied) {
  Wr w;
  w.u8(0);
  w.str(mapping);
  w.str(options_text);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqHubDevice, w.bytes(), r, 3000)) return false;
  Rd rd(r.data(), r.size());
  const bool ok = rd.u8() != 0;
  const std::string text = rd.str();
  if (applied && rd.ok()) *applied = text;
  return ok;
}
bool RemoteEngine::hub_switch_decks(const std::string& mapping, int layer, int* now) {
  Wr w;
  w.u8(4);
  w.str(mapping);
  w.i32(layer);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqHubDevice, w.bytes(), r, 3000)) return false;
  Rd rd(r.data(), r.size());
  const bool ok = rd.u8() != 0;
  const int l = rd.i32();
  if (now && rd.ok()) *now = l;
  return ok;
}
double RemoteEngine::hub_jog_ticks(const std::string& mapping) {
  Wr w;
  w.u8(1);
  w.str(mapping);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqHubDevice, w.bytes(), r, 3000)) return 0;
  Rd rd(r.data(), r.size());
  return rd.f64();
}
bool RemoteEngine::hub_reload_mappings() {
  Wr w;
  w.u8(2);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqHubDevice, w.bytes(), r, 8000)) return false;
  Rd rd(r.data(), r.size());
  return rd.u8() != 0;
}
bool RemoteEngine::hub_prefer_mappings(const std::vector<std::string>& names) {
  Wr w;
  w.u8(3);
  w.u16(static_cast<std::uint16_t>(std::min<std::size_t>(names.size(), 64)));
  for (std::size_t i = 0; i < names.size() && i < 64; ++i) w.str(names[i]);
  std::vector<std::uint8_t> r;
  if (!rpc(kReqHubDevice, w.bytes(), r, 3000)) return false;
  Rd rd(r.data(), r.size());
  return rd.u8() != 0;
}
std::vector<std::string> RemoteEngine::hub_connected() const {
  std::vector<std::string> out;
  const auto l = link();
  if (!l) return out;
  const HubInfo h = l->sh->hub.read();
  for (std::uint32_t i = 0; i < h.n_connected && i < kHubNames; ++i) out.push_back(cstr(h.names[i], sizeof h.names[i]));
  return out;
}
bool RemoteEngine::hub_audio_hint(std::string& device_match, int& master_channel, int& phones_channel) const {
  const auto l = link();
  if (!l) return false;
  const HubInfo h = l->sh->hub.read();
  if (!h.audio_valid) return false;
  device_match = cstr(h.audio_match, sizeof h.audio_match);
  master_channel = h.audio_master;
  phones_channel = h.audio_phones;
  return true;
}

}  // namespace deckhand
