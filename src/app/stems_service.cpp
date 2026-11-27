#include "deckhand/app/stems_service.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "deckhand/audio/track.hpp"
#include "deckhand/log/log.hpp"
#include "deckhand/stems/manifest.hpp"

namespace deckhand {
namespace fs = std::filesystem;

namespace {
stems::CacheConfig cache_config(const std::string& data_dir) {
  stems::CacheConfig c;
  c.root = stems::cache_dir(data_dir);
  return c;
}
}  // namespace

StemService::StemService(std::string data_dir, std::string resource_dir, std::function<bool()> deck_playing)
    : data_dir_(std::move(data_dir)), resource_dir_(std::move(resource_dir)), deck_playing_(std::move(deck_playing)), cache_(cache_config(data_dir_)) {
  load_index();
  watcher_ = std::thread([this] { watch(); });
}

StemService::~StemService() {
  quit_ = true;
  install_cancel_ = true;
  if (watcher_.joinable()) watcher_.join();
  if (install_thread_.joinable()) install_thread_.join();
  std::unique_ptr<stems::StemQueue> q;
  {
    std::lock_guard<std::mutex> lock(mu_);
    q = std::move(queue_);
    sink_ = nullptr;
  }
  q.reset();  // cancels the running job and joins (outside the lock: its callbacks take it)
}

void StemService::set_event_sink(EventFn fn) {
  std::lock_guard<std::mutex> lock(mu_);
  sink_ = std::move(fn);
}

void StemService::emit(const std::string& type, const std::string& id, const std::string& state, float progress, const std::string& error) {
  EventFn fn;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fn = sink_;
  }
  if (fn) fn(type, id, state, progress, error);
}

bool StemService::available(std::string* why) const {
  if (stems::stems_available()) return true;
  if (why)
    *why = std::string(stems::backend_name()) == "none"
               ? "this build has no stem separation runtime (ONNX Runtime is not compiled in)"
               : "stem separation is disabled in this build";
  return false;
}

// ---- models -------------------------------------------------------------------------------------------------------------

std::vector<StemModelRow> StemService::models() {
  const fs::path dir = fs::path(resource_dir_) / "stem-models";
  std::vector<StemModelRow> rows;
  std::vector<std::string> problems;
  const auto installed = stems::installed_models(data_dir_);
  for (const stems::CatalogueEntry& e : stems::list_catalogue(dir, &problems)) {
    StemModelRow r;
    r.entry = e;
    for (const stems::InstalledModel& m : installed) {
      if (m.id != e.id) continue;
      r.installed = true;
      r.usable = m.usable;
      r.installed_bytes = m.bytes;
      r.licence_accepted_at = m.licence_accepted_at;
    }
    rows.push_back(std::move(r));
  }
  return rows;
}

StemService::Result StemService::install(const std::string& model_id, bool accept_licence, std::string& error) {
  std::string why;
  if (!available(&why)) {
    error = why;
    return Result::Unavailable;
  }
  stems::CatalogueEntry entry;
  bool found = false;
  for (const StemModelRow& r : models())
    if (r.entry.id == model_id) {
      entry = r.entry;
      found = true;
    }
  if (!found) {
    error = "no such model";
    return Result::NotFound;
  }
  if (!accept_licence) {
    error = "the licence must be accepted";
    return Result::NeedLicence;
  }
  if (!entry.requires_ids.empty()) {  // a recipe combines models that are already installed
    std::string missing;
    for (const std::string& need : entry.requires_ids) {
      bool have = false;
      for (const StemModelRow& r : models())
        if (r.entry.id == need && r.installed && r.usable) have = true;
      if (!have) missing += (missing.empty() ? "" : ", ") + need;
    }
    if (!missing.empty()) {
      error = "install these models first: " + missing;
      return Result::Refused;
    }
  }
  if (!entry.install_supported) {
    error = "this model cannot be installed yet: " + (entry.blocked_by.empty() ? std::string("conversion step missing") : entry.blocked_by);
    return Result::Refused;
  }
  {
    std::lock_guard<std::mutex> lock(install_mu_);
    if (install_.state == "running") {
      error = "an install is already running";
      return Result::Busy;
    }
    install_ = StemInstallState{};
    install_.id = model_id;
    install_.state = "running";
  }
  if (install_thread_.joinable()) install_thread_.join();
  install_cancel_ = false;
  install_thread_ = std::thread([this, entry] {
    stems::InstallHooks hooks;
    hooks.download = downloader_;
    hooks.convert = stems::demucs_converter(data_dir_, fs::path(resource_dir_) / "stem-models" / "skeletons");
    float last = -1;
    const stems::InstallResult res = stems::install_model(
        data_dir_, entry, true,
        [&](float f) {
          {
            std::lock_guard<std::mutex> lock(install_mu_);
            install_.progress = f;
          }
          if (f - last >= 0.01f || f >= 1.0f) {
            last = f;
            emit("stems.install", entry.id, "running", f, "");
          }
        },
        &install_cancel_, hooks);
    {
      std::lock_guard<std::mutex> lock(install_mu_);
      install_.state = res.ok ? "done" : res.cancelled ? "cancelled" : "failed";
      install_.error = res.error;
      if (res.ok) install_.progress = 1;
    }
    emit("stems.install", entry.id, res.ok ? "done" : res.cancelled ? "cancelled" : "failed", res.ok ? 1.0f : 0.0f, res.error);
  });
  return Result::Ok;
}

bool StemService::cancel_install() {
  std::lock_guard<std::mutex> lock(install_mu_);
  if (install_.state != "running") return false;
  install_cancel_ = true;
  return true;
}

StemInstallState StemService::install_state() {
  std::lock_guard<std::mutex> lock(install_mu_);
  return install_;
}

bool StemService::remove(const std::string& model_id) {
  {
    std::lock_guard<std::mutex> lock(install_mu_);
    if (install_.state == "running" && install_.id == model_id) return false;
  }
  std::unique_ptr<stems::StemQueue> dropped;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ && queue_model_ == model_id) {
      dropped = std::move(queue_);  // cancels its jobs and joins after the lock is released, so the model files are free
      queue_model_.clear();
      job_track_.clear();
    }
  }
  dropped.reset();
  return stems::remove_model(data_dir_, model_id);
}

// ---- queue --------------------------------------------------------------------------------------------------------------

bool StemService::ensure_queue(std::string& error, std::unique_ptr<stems::StemQueue>& old) {  // mu_ held
  std::vector<stems::ModelManifest> usable = stems::list_models(stems::models_dir(data_dir_));
  if (usable.empty()) {
    error = "no stem model is installed";
    return false;
  }
  const auto pick = std::find_if(usable.begin(), usable.end(), [](const stems::ModelManifest& m) { return m.id.find("htdemucs") != std::string::npos && m.id.find("ft") == std::string::npos; });
  const stems::ModelManifest m = pick != usable.end() ? *pick : usable.front();
  if (queue_ && queue_model_ == m.id) return true;
  if (queue_ && queue_->pending() > 0) return true;  // keep working with the current model until it is idle
  old = std::move(queue_);
  job_track_.clear();
  queue_model_ = m.id;
  queue_ = std::make_unique<stems::StemQueue>(
      cache_, m.id, [m](std::string* err) { return stems::open_model(m, 2, err); }, stems::QueueOptions{});
  queue_->set_on_change([this](const stems::JobInfo& j) { job_changed(j); });
  apply_pause();
  return true;
}

void StemService::job_changed(const stems::JobInfo& j) {
  if (j.state == stems::JobState::Done || j.state == stems::JobState::AlreadyCached) {
    bool save = false;
    std::int64_t ready_track = 0;
    std::function<void(std::int64_t)> ready;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto it = job_track_.find(j.id);
      if (it != job_track_.end() && !j.key.empty()) {
        track_key_[it->second] = j.key;
        track_model_[it->second] = queue_model_;
        save = true;
        ready_track = it->second;
        ready = ready_;
      }
    }
    if (save) save_index();
    if (ready && ready_track != 0) ready(ready_track);
  }
  emit("stems.job", std::to_string(j.id), stems::job_state_name(j.state), j.progress, j.error);
}

StemService::Result StemService::enqueue(std::int64_t track_id, const std::string& path, const std::string& label, std::uint64_t& job, std::string& error) {
  std::string why;
  if (!available(&why)) {
    error = why;
    return Result::Unavailable;
  }
  std::unique_ptr<stems::StemQueue> old;  // declared before the lock: destroyed (joined) after it is released
  std::lock_guard<std::mutex> lock(mu_);
  if (!ensure_queue(error, old)) return Result::Unavailable;
  stems::JobSpec spec;
  spec.label = label.empty() ? path : label;
  spec.load = [path](stems::Audio& out, std::string* err) {
    std::string e;
    const std::shared_ptr<TrackAudio> a = decode_file(path, e);
    if (!a || a->frames == 0) {
      if (err) *err = e.empty() ? "could not decode the file" : e;
      return false;
    }
    out.sample_rate = static_cast<int>(a->sample_rate);
    out.l.resize(a->frames);
    out.r.resize(a->frames);
    const float* p = a->data();
    for (std::uint64_t i = 0; i < a->frames; ++i) {
      out.l[i] = p[2 * i];
      out.r[i] = p[2 * i + 1];
    }
    return true;
  };
  job = queue_->enqueue(std::move(spec));
  job_track_[job] = track_id;
  return Result::Ok;
}

bool StemService::cancel_job(std::uint64_t job) {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_ && queue_->cancel(job);
}

void StemService::pause() {
  user_paused_ = true;
  apply_pause();
}
void StemService::resume() {
  user_paused_ = false;
  apply_pause();
}

void StemService::apply_pause() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!queue_) return;
  if (user_paused_ || auto_paused_) queue_->pause();
  else queue_->resume();
}

void StemService::watch() {
  while (!quit_) {
    for (int i = 0; i < 5 && !quit_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool playing = deck_playing_ && deck_playing_();
    if (playing != auto_paused_) {
      auto_paused_ = playing;
      apply_pause();
    }
  }
}

StemStatus StemService::status() {
  StemStatus s;
  s.available = available(&s.why_not);
  s.backend = stems::backend_name();
  s.cache_bytes = cache_.total_bytes();
  s.cache_cap = cache_.config().cap_bytes;
  s.cache_entries = cache_.count();
  s.user_paused = user_paused_;
  s.auto_paused = auto_paused_;
  s.paused = user_paused_ || auto_paused_;
  std::lock_guard<std::mutex> lock(mu_);
  s.model = queue_model_;
  if (queue_) {
    s.pending = queue_->pending();
    s.jobs = queue_->jobs();
  }
  return s;
}

// ---- per-track ----------------------------------------------------------------------------------------------------------

std::string StemService::stems_file(std::int64_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto k = track_key_.find(id);
  if (k == track_key_.end() || !cache_.has_stems(k->second)) return {};
  return cache_.path_for(k->second).string();
}

void StemService::set_ready_sink(std::function<void(std::int64_t)> fn) {
  std::lock_guard<std::mutex> lock(mu_);
  ready_ = std::move(fn);
}

bool StemService::cache_ref(std::int64_t id, std::string& root, std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto k = track_key_.find(id);
  if (k == track_key_.end() || !cache_.has_stems(k->second)) return false;
  root = cache_.config().root.string();
  key = k->second;
  return true;
}

StemTrackInfo StemService::track(std::int64_t id) {
  StemTrackInfo t;
  std::lock_guard<std::mutex> lock(mu_);
  const auto k = track_key_.find(id);
  if (k != track_key_.end()) {
    t.key = k->second;
    t.cached = cache_.has_stems(k->second);
    if (const auto m = track_model_.find(id); m != track_model_.end()) t.model = m->second;
    if (t.cached) {
      std::error_code ec;
      const auto sz = fs::file_size(cache_.path_for(k->second), ec);
      if (!ec) t.bytes = sz;
    }
  }
  if (queue_) {
    for (const auto& [job, tid] : job_track_) {
      if (tid != id) continue;
      const stems::JobInfo j = queue_->job(job);
      if (j.finished()) continue;
      t.queued = true;
      t.job = job;
      t.state = stems::job_state_name(j.state);
      t.progress = j.progress;
    }
  }
  return t;
}

bool StemService::evict_track(std::int64_t id) {
  std::string key;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto k = track_key_.find(id);
    if (k == track_key_.end()) return false;
    key = k->second;
    track_key_.erase(k);
    track_model_.erase(id);
  }
  save_index();
  return cache_.evict(key);
}

// The track -> cache key index lives beside the cache: "<track id>\t<key>\t<model>" per line.
void StemService::load_index() {
  std::ifstream in(stems::cache_dir(data_dir_) / "tracks.tsv");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string id, key, model;
    if (std::getline(ss, id, '\t') && std::getline(ss, key, '\t')) {
      std::getline(ss, model);
      try {
        track_key_[std::stoll(id)] = key;
        track_model_[std::stoll(id)] = model;
      } catch (...) {
      }
    }
  }
}

void StemService::save_index() {
  std::map<std::int64_t, std::string> keys, models;
  {
    std::lock_guard<std::mutex> lock(mu_);
    keys = track_key_;
    models = track_model_;
  }
  std::error_code ec;
  fs::create_directories(stems::cache_dir(data_dir_), ec);
  const fs::path p = stems::cache_dir(data_dir_) / "tracks.tsv";
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    for (const auto& [id, key] : keys) out << id << '\t' << key << '\t' << models[id] << '\n';
  }
  fs::rename(tmp, p, ec);
}

}  // namespace deckhand
