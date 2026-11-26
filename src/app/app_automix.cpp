// Automix and the play queue inside the application: the host adapter the engine room needs, the control table's
// automix.* actions, and the queue actions of the browser.
#include "app_impl.hpp"

namespace deckhand {

namespace {

// Reads track facts through a library connection of its own and loads tracks through the application.
class AppAutomixHost final : public AutomixHost {
 public:
  AppAutomixHost(IEngine& engine, const std::string& db, std::function<bool(int, std::int64_t)> load,
                 std::function<LoadState(int, std::int64_t)> state)
      : engine_(engine), lib_(db), load_(std::move(load)), state_(std::move(state)) {}
  IEngine& engine() override { return engine_; }
  TrackFacts facts(std::int64_t id) override {
    std::lock_guard<std::mutex> lock(m_);
    return read_track_facts(lib_, id);
  }
  bool request_load(int deck, std::int64_t id) override { return load_(deck, id); }
  LoadState load_state(int deck, std::int64_t id) override { return state_(deck, id); }
  std::vector<std::int64_t> source_tracks(const std::string& source) override {
    std::lock_guard<std::mutex> lock(m_);
    return resolve_track_source(lib_, source);
  }

 private:
  IEngine& engine_;
  std::mutex m_;
  Library lib_;
  std::function<bool(int, std::int64_t)> load_;
  std::function<LoadState(int, std::int64_t)> state_;
};

}  // namespace

void App::setup_automix() {
  const std::string db = data_dir_ + "/library.db";
  queue_lib_ = std::make_unique<Library>(db);
  amx_lib_ = std::make_unique<Library>(db);
  queue_ = std::make_unique<PlayQueue>(queue_lib_.get());
  amx_host_ = std::make_unique<AppAutomixHost>(
      *engine_, db,
      [this](int deck, std::int64_t id) {
        if (deck < 0 || deck >= kMaxDecks || engine_->deck(deck).playing) return false;
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          load_failed_[deck] = 0;
        }
        request_load(deck, id);
        return true;
      },
      [this](int deck, std::int64_t id) {
        const DeckView v = engine_->deck(deck);
        if (v.loaded && v.track_id == id) return LoadState::Ready;
        std::lock_guard<std::mutex> lock(state_mutex_);
        return load_failed_[deck] == id ? LoadState::Failed : LoadState::Pending;
      });
  automix_ = std::make_unique<AutomixService>(*amx_host_, *queue_, amx_lib_.get());
  prep_ = std::make_unique<PrepMode>(*amx_host_);
  prep_->set_blocker([this]() -> std::string { return automix_ && automix_->status().running ? T("prep.refused_automix") : std::string(); });
  next_set_ = next_settings(*lib_);
}

bool App::automix_control(const std::string& action, bool has_value, double value, std::string& error) {
  if (!automix_) {
    error = "automix is not available";
    return false;
  }
  const AutomixStatus st = automix_->status();
  if (action == "run") return automix_control(has_value ? (value > 0.5 ? "start" : "stop") : (st.running ? "stop" : "start"), false, 0, error);
  if (action == "start") {
    if (prep_ && prep_->active()) {
      error = T("prep.refused_prep_on");
      return false;
    }
    if (!automix_->start(error)) return false;
    status_note_ = T("automix.on");
  } else if (action == "stop") {
    automix_->stop();
    status_note_ = T("automix.off");
  } else if (action == "pause") {
    const bool on = has_value ? value > 0.5 : !st.paused;
    if (on) {
      automix_->pause();
      status_note_ = T("automix.paused");
    } else {
      automix_->resume();
      status_note_ = T("automix.on");
    }
  } else if (action == "skip") {
    if (!automix_->skip()) {
      error = "automix is not running, or a transition is already under way";
      return false;
    }
    status_note_ = T("automix.skipping");
  } else if (action == "next_type") {
    static const char* const kTypes[] = {"crossfade", "cut", "fade", "echo_out"};
    AutomixSettings s = automix_->settings();
    std::size_t i = 0;
    while (i < 4 && s.transition_type != kTypes[i]) ++i;
    s.transition_type = kTypes[(i + 1) % 4];
    automix_->set_settings(s);
    status_note_ = T("automix.transition", {{"type", s.transition_type}, {"beats", fmt("%g", s.transition_beats)}});
  } else if (action == "next_length") {
    AutomixSettings s = automix_->settings();
    s.transition_beats = s.transition_beats < 4 ? 4 : s.transition_beats < 8 ? 8 : s.transition_beats < 16 ? 16 : s.transition_beats < 32 ? 32 : 4;
    automix_->set_settings(s);
    status_note_ = T("automix.transition", {{"type", s.transition_type}, {"beats", fmt("%g", s.transition_beats)}});
  } else if (action == "queue_clear") {
    queue_->clear();
  } else if (action == "queue_shuffle") {
    queue_->shuffle();
  } else if (action == "queue_skip") {
    queue_->skip();
  } else if (action == "queue_back") {
    if (!queue_->back()) {
      error = "nothing was played before";
      return false;
    }
    automix_->skip();
  } else {
    error = "unknown automix control";
    return false;
  }
  return true;
}

// Adds the browser's selected tracks to the play queue: at the end, or to play next.
void App::queue_selected(bool play_next) {
  if (!queue_) return;
  std::vector<std::int64_t> ids;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    ids = selected_ids();
  }
  if (ids.empty()) return;
  const QueueAdd r = play_next ? queue_->play_next(ids) : queue_->add_end(ids);
  if (r.entry_ids.empty() && r.skipped > 0) status_note_ = T("queue.refused");
  else status_note_ = T(play_next ? "queue.play_next_added" : "queue.added", {{"count", std::to_string(r.entry_ids.size())}});
}

}  // namespace deckhand
