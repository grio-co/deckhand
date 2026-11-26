// Low-priority background separation queue: one track at a time, pausable (e.g. while decks are playing),
// with progress and cancel. Results go to the StemCache; the model is created lazily and released when idle.
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "deckhand/stems/cache.hpp"
#include "deckhand/stems/stems.hpp"

namespace deckhand::stems {

using ModelFactory = std::function<std::unique_ptr<StemModel>(std::string* error)>;
using AudioLoader = std::function<bool(Audio& out, std::string* error)>;  // decodes the track; runs on the worker

struct JobSpec {
  std::string label;  // shown in the UI (track title / path); not used for keying
  AudioLoader load;
};
enum class JobState { Queued, Loading, Running, Done, AlreadyCached, Failed, Cancelled };
const char* job_state_name(JobState s);

struct JobInfo {
  std::uint64_t id = 0;
  std::string label, key, error;
  JobState state = JobState::Queued;
  float progress = 0;  // 0..1
  double seconds = 0;
  bool finished() const { return state == JobState::Done || state == JobState::AlreadyCached || state == JobState::Failed || state == JobState::Cancelled; }
};

struct QueueOptions {
  SeparateOptions separate;  // overlap / mixture mode (progress, cancel and pause are managed by the queue)
  bool low_priority = true;  // lowers the worker thread's OS priority
};

class StemQueue {
 public:
  // `model_id` must equal the id the factory's models report; it keys the cache before the model is built.
  StemQueue(StemCache& cache, std::string model_id, ModelFactory factory, QueueOptions opt = {});
  ~StemQueue();  // cancels the running job and joins

  std::uint64_t enqueue(JobSpec job);
  bool cancel(std::uint64_t id);  // queued or running
  void cancel_all();
  void pause();   // takes effect at the next chunk boundary (and before the next job)
  void resume();
  bool paused() const { return paused_.load(); }
  JobInfo job(std::uint64_t id) const;
  std::vector<JobInfo> jobs() const;  // all jobs since construction, oldest first
  std::size_t pending() const;        // queued + running
  bool wait_idle(int timeout_ms);     // true when nothing is queued or running
  void set_on_change(std::function<void(const JobInfo&)> fn);  // called from the worker thread

 private:
  struct Job {
    JobInfo info;
    JobSpec spec;
    std::atomic<bool> cancel{false};
  };
  void run();
  void update(Job& j, const std::function<void(JobInfo&)>& f);

  StemCache& cache_;
  std::string model_id_;
  ModelFactory factory_;
  QueueOptions opt_;
  mutable std::mutex mu_;
  std::condition_variable cv_, idle_cv_;
  std::deque<std::shared_ptr<Job>> todo_;
  std::vector<std::shared_ptr<Job>> all_;
  std::shared_ptr<Job> running_;
  std::atomic<bool> paused_{false}, quit_{false};
  std::uint64_t next_id_ = 1;
  std::function<void(const JobInfo&)> on_change_;
  std::thread worker_;
};

}  // namespace deckhand::stems
