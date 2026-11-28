// Rate-limited HTTP client (one request per second, back-off on 503/429) and the real clock.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "deckhand/metadata/metadata.hpp"

namespace deckhand::metadata {
namespace {

class SystemClock : public Clock {
 public:
  std::int64_t now_ms() override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void sleep_ms(std::int64_t ms) override {
    // sleep in slices so a cancelled lookup stops quickly
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
      std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds(100)));
    }
  }
};

}  // namespace

std::shared_ptr<Clock> system_clock() { return std::make_shared<SystemClock>(); }

RateLimitedClient::RateLimitedClient(std::shared_ptr<Transport> t, std::shared_ptr<Clock> c, std::string user_agent, RatePolicy p)
    : t_(std::move(t)), c_(std::move(c)), ua_(std::move(user_agent)), p_(p) {}

HttpResponse RateLimitedClient::get(const std::string& url, const std::atomic<bool>* cancel) {
  HttpResponse r;
  if (!t_ || !c_) {
    r.error = "no transport";
    return r;
  }
  std::int64_t backoff = p_.backoff_base_ms;
  for (int attempt = 0;; ++attempt) {
    if (cancel && cancel->load()) {
      r.status = 0;
      r.error = "cancelled";
      return r;
    }
    if (started_) {
      const std::int64_t wait = last_start_ + p_.min_interval_ms - c_->now_ms();
      if (wait > 0) c_->sleep_ms(wait);
    }
    if (cancel && cancel->load()) {
      r = HttpResponse{};
      r.error = "cancelled";
      return r;
    }
    last_start_ = c_->now_ms();
    started_ = true;
    ++requests_;
    HttpRequest q;
    q.url = url;
    q.user_agent = ua_;
    r = t_->get(q);
    if (r.status != 503 && r.status != 429) return r;
    if (attempt >= p_.max_retries) {
      r.error = "service busy (HTTP " + std::to_string(r.status) + "), gave up after " + std::to_string(attempt + 1) + " attempts";
      return r;
    }
    std::int64_t wait = backoff;
    const long ra = std::strtol(r.retry_after.c_str(), nullptr, 10);
    if (ra > 0) wait = std::max<std::int64_t>(wait, static_cast<std::int64_t>(ra) * 1000);
    wait = std::min(wait, p_.backoff_cap_ms);
    c_->sleep_ms(wait);
    backoff = std::min(backoff * 2, p_.backoff_cap_ms);
  }
}

}  // namespace deckhand::metadata
