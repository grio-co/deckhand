#pragma once
// Stem separation as the application sees it: model install / remove, the background queue and the on-disk cache, owned in
// one place so the API bridge and the settings panel share state. Plain data in, plain data out (no JSON, no UI).
// The queue pauses itself while any deck plays (no CPU contention during a set) and resumes when all decks are stopped.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deckhand/stems/model_install.hpp"
#include "deckhand/stems/queue.hpp"

namespace deckhand {

struct StemModelRow {
  stems::CatalogueEntry entry;
  bool installed = false, usable = false;
  std::uint64_t installed_bytes = 0;
  std::string licence_accepted_at;
};

struct StemInstallState {
  std::string id;
  std::string state = "idle";  // idle | running | done | failed | cancelled
  float progress = 0;
  std::string error;
};

struct StemStatus {
  bool available = false;
  std::string why_not;   // when !available
  std::string backend;
  std::string model;     // the model the queue uses ("" = none installed)
  std::uint64_t cache_bytes = 0, cache_cap = 0;
  std::size_t cache_entries = 0;
  bool paused = false, user_paused = false, auto_paused = false;
  std::size_t pending = 0;
  std::vector<stems::JobInfo> jobs;
};

struct StemTrackInfo {
  bool cached = false;
  std::string key, model;
  std::uint64_t bytes = 0;
  bool queued = false;
  std::uint64_t job = 0;
  std::string state;  // job state when queued / known
  float progress = 0;
};

class StemService {
 public:
  // deck_playing: true while any deck plays (called from the watcher thread, must be thread-safe).
  StemService(std::string data_dir, std::string resource_dir, std::function<bool()> deck_playing);
  ~StemService();

  // type: "stems.install" | "stems.job"; id: model id or job id; state / progress / error as in the status structs.
  using EventFn = std::function<void(const std::string& type, const std::string& id, const std::string& state, float progress,
                                     const std::string& error)>;
  void set_event_sink(EventFn fn);
  // Replaces the downloader of model installs (tests use local files); empty = the default downloader.
  void set_downloader(stems::DownloadFn fn) { downloader_ = std::move(fn); }

  bool available(std::string* why = nullptr) const;
  StemStatus status();
  std::vector<StemModelRow> models();

  enum class Result { Ok, NotFound, NeedLicence, Unavailable, Busy, Refused };
  // Starts the download + conversion on a worker thread. `error` carries the reason unless Ok.
  Result install(const std::string& model_id, bool accept_licence, std::string& error);
  bool cancel_install();
  StemInstallState install_state();
  bool remove(const std::string& model_id);

  Result enqueue(std::int64_t track_id, const std::string& path, const std::string& label, std::uint64_t& job, std::string& error);
  bool cancel_job(std::uint64_t job);
  void pause();   // the user's pause; auto-pause is separate
  void resume();
  StemTrackInfo track(std::int64_t track_id);
  bool evict_track(std::int64_t track_id);  // drops the cached stems of this track; true when something was removed

 private:
  void watch();
  void apply_pause();
  void emit(const std::string& type, const std::string& id, const std::string& state, float progress, const std::string& error);
  bool ensure_queue(std::string& error, std::unique_ptr<stems::StemQueue>& old);
  void job_changed(const stems::JobInfo& j);
  void load_index();
  void save_index();

  std::string data_dir_, resource_dir_;
  std::function<bool()> deck_playing_;
  stems::StemCache cache_;

  std::mutex mu_;
  std::unique_ptr<stems::StemQueue> queue_;
  std::string queue_model_;
  std::map<std::uint64_t, std::int64_t> job_track_;      // job -> track id
  std::map<std::int64_t, std::string> track_key_;        // track id -> cache key (persisted)
  std::map<std::int64_t, std::string> track_model_;
  EventFn sink_;

  std::mutex install_mu_;
  StemInstallState install_;
  stems::DownloadFn downloader_;
  std::atomic<bool> install_cancel_{false};
  std::thread install_thread_;

  std::atomic<bool> user_paused_{false}, auto_paused_{false}, quit_{false};
  std::thread watcher_;
};

}  // namespace deckhand
