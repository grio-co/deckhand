#pragma once
// A small reusable confirmation: a question with two answers, shown as a panel (Panel::Confirm). Pure logic (no window), so
// it is tested headless. Anything may ask from any thread (ConfirmQueue); the UI thread shows one request at a time and
// calls exactly one of on_ok / on_cancel when it is answered, replaced or dismissed.
//
//   ConfirmRequest r;  r.id = "quit.capture";  r.message = "...";  r.on_ok = [..] {..};
//   app.ask_confirm(std::move(r));      // from any thread
//
// A request with an id equal to one already shown or waiting is dropped (a repeated "quit" asks once).
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace deckhand {

struct ConfirmRequest {
  std::string id;                    // what is asked (de-duplicates); "" = never de-duplicated
  std::string title;                 // panel title
  std::string message;               // the question
  std::vector<std::string> details;  // extra lines under it (what would be lost, how many)
  std::string ok_label, cancel_label;  // button texts (already translated)
  bool danger = false;               // the ok button is drawn as a warning
  std::function<void()> on_ok;       // may be empty
  std::function<void()> on_cancel;   // may be empty; also runs when the request is replaced or the panel dismissed
};

// Requests from any thread, taken by the UI thread.
class ConfirmQueue {
 public:
  // False when a request with the same id is already waiting.
  bool ask(ConfirmRequest r) {
    std::lock_guard<std::mutex> lock(m_);
    if (!r.id.empty())
      for (const ConfirmRequest& w : waiting_)
        if (w.id == r.id) return false;
    waiting_.push_back(std::move(r));
    return true;
  }
  bool take(ConfirmRequest& out) {
    std::lock_guard<std::mutex> lock(m_);
    if (waiting_.empty()) return false;
    out = std::move(waiting_.front());
    waiting_.pop_front();
    return true;
  }
  bool waiting(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_);
    for (const ConfirmRequest& w : waiting_)
      if (w.id == id) return true;
    return false;
  }
  std::size_t size() {
    std::lock_guard<std::mutex> lock(m_);
    return waiting_.size();
  }

 private:
  std::mutex m_;
  std::deque<ConfirmRequest> waiting_;
};

enum class ConfirmKey { Enter, Escape };

// What the panel shows right now. UI thread only.
class ConfirmState {
 public:
  bool open() const { return open_; }
  const ConfirmRequest& request() const { return req_; }

  // Shows `r`. A request already shown is cancelled first (its on_cancel runs), unless it has the same id: then `r` is dropped.
  bool show(ConfirmRequest r) {
    if (open_) {
      if (!r.id.empty() && r.id == req_.id) return false;
      cancel();
    }
    req_ = std::move(r);
    open_ = true;
    return true;
  }
  // The answers. Each closes the dialog first, then runs its callback (which may ask again).
  bool accept() { return finish(true); }
  bool cancel() { return finish(false); }
  // Enter accepts, Escape cancels. True when the key was used.
  bool key(ConfirmKey k) {
    if (!open_) return false;
    return k == ConfirmKey::Enter ? accept() : cancel();
  }

 private:
  bool finish(bool ok) {
    if (!open_) return false;
    open_ = false;
    std::function<void()> run = ok ? std::move(req_.on_ok) : std::move(req_.on_cancel);
    req_ = ConfirmRequest{};
    if (run) run();
    return true;
  }
  bool open_ = false;
  ConfirmRequest req_;
};

}  // namespace deckhand
