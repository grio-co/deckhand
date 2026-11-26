#include "deckhand/stems/queue.hpp"

#include <chrono>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace deckhand::stems {

const char* job_state_name(JobState s) {
  switch (s) {
    case JobState::Queued: return "queued";
    case JobState::Loading: return "loading";
    case JobState::Running: return "running";
    case JobState::Done: return "done";
    case JobState::AlreadyCached: return "cached";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
  }
  return "?";
}

namespace {
void lower_thread_priority() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#elif defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 15);  // per-thread nice on Linux
#endif
}
}  // namespace

StemQueue::StemQueue(StemCache& cache, std::string model_id, ModelFactory factory, QueueOptions opt)
    : cache_(cache), model_id_(std::move(model_id)), factory_(std::move(factory)), opt_(std::move(opt)) {
  worker_ = std::thread([this] { run(); });
}

StemQueue::~StemQueue() {
  quit_ = true;
  paused_ = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& j : todo_) j->cancel = true;
    if (running_) running_->cancel = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::uint64_t StemQueue::enqueue(JobSpec spec) {
  auto j = std::make_shared<Job>();
  std::uint64_t id;
  {
    std::lock_guard<std::mutex> g(mu_);
    id = next_id_++;
    j->info.id = id;
    j->info.label = spec.label;
    j->spec = std::move(spec);
    todo_.push_back(j);
    all_.push_back(j);
  }
  cv_.notify_all();
  return id;
}

bool StemQueue::cancel(std::uint64_t id) {
  std::lock_guard<std::mutex> g(mu_);
  for (auto& j : all_)
    if (j->info.id == id && !j->info.finished()) { j->cancel = true; cv_.notify_all(); return true; }
  return false;
}
void StemQueue::cancel_all() {
  std::lock_guard<std::mutex> g(mu_);
  for (auto& j : all_) if (!j->info.finished()) j->cancel = true;
  cv_.notify_all();
}
void StemQueue::pause() { paused_ = true; }
void StemQueue::resume() { paused_ = false; cv_.notify_all(); }

JobInfo StemQueue::job(std::uint64_t id) const {
  std::lock_guard<std::mutex> g(mu_);
  for (auto& j : all_) if (j->info.id == id) return j->info;
  return JobInfo{};
}
std::vector<JobInfo> StemQueue::jobs() const {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<JobInfo> v;
  for (auto& j : all_) v.push_back(j->info);
  return v;
}
std::size_t StemQueue::pending() const {
  std::lock_guard<std::mutex> g(mu_);
  return todo_.size() + (running_ ? 1 : 0);
}
bool StemQueue::wait_idle(int timeout_ms) {
  std::unique_lock<std::mutex> lk(mu_);
  return idle_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return todo_.empty() && !running_; });
}
void StemQueue::set_on_change(std::function<void(const JobInfo&)> fn) {
  std::lock_guard<std::mutex> g(mu_);
  on_change_ = std::move(fn);
}

void StemQueue::update(Job& j, const std::function<void(JobInfo&)>& f) {
  JobInfo copy;
  std::function<void(const JobInfo&)> cb;
  {
    std::lock_guard<std::mutex> g(mu_);
    f(j.info);
    copy = j.info;
    cb = on_change_;
  }
  if (cb) cb(copy);
}

void StemQueue::run() {
  if (opt_.low_priority) lower_thread_priority();
  std::unique_ptr<StemModel> model;
  for (;;) {
    std::shared_ptr<Job> j;
    {
      std::unique_lock<std::mutex> lk(mu_);
      if (todo_.empty()) {
        model.reset();  // idle: give the memory back
        running_.reset();
        idle_cv_.notify_all();
      }
      cv_.wait(lk, [&] { return quit_ || (!todo_.empty() && !paused_.load()); });
      if (quit_ && todo_.empty()) return;
      if (todo_.empty()) continue;
      j = todo_.front();
      todo_.pop_front();
      running_ = j;
    }
    auto finish = [&](JobState st, const std::string& err = {}) {
      update(*j, [&](JobInfo& i) { i.state = st; i.error = err; if (st == JobState::Done || st == JobState::AlreadyCached) i.progress = 1; });
    };
    if (j->cancel) { finish(JobState::Cancelled); continue; }
    auto t0 = std::chrono::steady_clock::now();
    update(*j, [](JobInfo& i) { i.state = JobState::Loading; });
    Audio audio;
    std::string err;
    if (!j->spec.load || !j->spec.load(audio, &err)) { finish(JobState::Failed, err.empty() ? "could not decode audio" : err); continue; }
    if (j->cancel) { finish(JobState::Cancelled); continue; }
    std::string key = content_key(audio, model_id_);
    update(*j, [&](JobInfo& i) { i.key = key; });
    if (cache_.has_stems(key)) { cache_.open(key); finish(JobState::AlreadyCached); continue; }
    if (!model) {
      model = factory_ ? factory_(&err) : nullptr;
      if (!model) { finish(JobState::Failed, err.empty() ? "no model available" : err); continue; }
    }
    update(*j, [](JobInfo& i) { i.state = JobState::Running; });
    SeparateOptions so = opt_.separate;
    so.cancel = &j->cancel;
    so.pause = &paused_;
    so.progress = [&](float f) { update(*j, [&](JobInfo& i) { i.progress = f * 0.97f; }); };
    SeparateResult r = separate(audio, *model, so);
    audio = Audio();
    if (r.status == SeparateResult::Status::Cancelled) { finish(JobState::Cancelled); continue; }
    if (r.status != SeparateResult::Status::Ok) { finish(JobState::Failed, r.error); continue; }
    if (!cache_.store(key, r.stems, &err)) { finish(JobState::Failed, err); continue; }
    update(*j, [&](JobInfo& i) { i.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); });
    finish(JobState::Done);
  }
}

}  // namespace deckhand::stems
