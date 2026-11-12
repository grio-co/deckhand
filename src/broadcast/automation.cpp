#include "deckhand/broadcast/automation.hpp"

#include <algorithm>

#include "sock.hpp"

namespace deckhand::broadcast {

Automation::Automation(ObsClient& obs)
    : send_([&obs](const std::string& type, const std::string& json, ObsCallback cb) {
        obs.request(type, json, std::move(cb));
      }) {}

Automation::Automation(Sender send) : send_(std::move(send)) {}

void Automation::set_rules(std::vector<AutomationRule> rules) {
  std::lock_guard<std::mutex> lk(m_);
  rules_ = std::move(rules);
}

void Automation::run_rule(const AutomationRule& r, const TrackInfo& t) {
  if (!send_) return;
  if (r.action == AutomationRule::Action::SetText) {
    if (r.source.empty()) return;
    Json settings = Json::object();
    settings.set("text", Json(NowPlaying::render(r.text, t)));
    Json d = Json::object();
    d.set("inputName", Json(r.source));
    d.set("inputSettings", std::move(settings));
    d.set("overlay", Json(true));
    send_("SetInputSettings", d.dump(), nullptr);
  } else if (r.action == AutomationRule::Action::SwitchScene) {
    if (r.scene.empty()) return;
    Json d = Json::object();
    d.set("sceneName", Json(r.scene));
    send_("SetCurrentProgramScene", d.dump(), nullptr);
  } else if (r.action == AutomationRule::Action::ShowSource) {
    if (r.scene.empty() || r.source.empty()) return;
    Json d = Json::object();
    d.set("sceneName", Json(r.scene));
    d.set("sourceName", Json(r.source));
    const std::string scene = r.scene;
    const double seconds = std::max(0.0, r.seconds);
    send_("GetSceneItemId", d.dump(), [this, scene, seconds](const ObsResponse& resp) {
      if (!resp.ok) return;
      const long long id = static_cast<long long>(resp.data["sceneItemId"].as_number(-1));
      Json e = Json::object();
      e.set("sceneName", Json(scene));
      e.set("sceneItemId", Json(id));
      e.set("sceneItemEnabled", Json(true));
      {
        std::lock_guard<std::mutex> lk(m_);
        // A repeated trigger extends the visible time instead of stacking timers.
        hides_.erase(std::remove_if(hides_.begin(), hides_.end(),
                                    [&](const Hide& h) { return h.scene == scene && h.item == id; }),
                     hides_.end());
        hides_.push_back({scene, id, clock_ + seconds});
      }
      send_("SetSceneItemEnabled", e.dump(), nullptr);
    });
  }
}

void Automation::on_track_change(const TrackInfo& t) {
  std::vector<AutomationRule> todo;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& r : rules_)
      if (r.enabled && r.trigger == AutomationRule::Trigger::TrackChange && (r.deck == 0 || r.deck == t.deck))
        todo.push_back(r);
  }
  for (const auto& r : todo) run_rule(r, t);
}

void Automation::update(const NowPlayingState& st, double now) {
  if (now < 0) now = net::now_seconds();
  struct Edge {
    int deck;
    bool started;
  };
  std::vector<Edge> edges;
  std::vector<AutomationRule> rules;
  {
    std::lock_guard<std::mutex> lk(m_);
    clock_ = now;
    std::vector<char> cur(st.decks.size());
    for (std::size_t i = 0; i < st.decks.size(); ++i) cur[i] = st.decks[i].loaded && st.decks[i].playing;
    if (have_baseline_) {
      for (std::size_t i = 0; i < cur.size(); ++i) {
        const bool before = i < was_playing_.size() && was_playing_[i];
        if (cur[i] && !before) edges.push_back({static_cast<int>(i) + 1, true});
        if (!cur[i] && before) edges.push_back({static_cast<int>(i) + 1, false});
      }
      for (std::size_t i = cur.size(); i < was_playing_.size(); ++i)
        if (was_playing_[i]) edges.push_back({static_cast<int>(i) + 1, false});
    }
    was_playing_ = std::move(cur);
    have_baseline_ = true;
    if (!edges.empty()) rules = rules_;
  }
  for (const auto& e : edges) {
    TrackInfo t;
    t.deck = e.deck;
    if (static_cast<std::size_t>(e.deck) <= st.decks.size()) {
      const DeckState& d = st.decks[static_cast<std::size_t>(e.deck) - 1];
      t.title = d.title;
      t.artist = d.artist;
      t.album = d.album;
      t.key = d.key;
      t.bpm = d.bpm;
      t.length_seconds = d.length_seconds;
    }
    for (const auto& r : rules) {
      const auto want = e.started ? AutomationRule::Trigger::DeckStart : AutomationRule::Trigger::DeckStop;
      if (r.enabled && r.trigger == want && (r.deck == 0 || r.deck == e.deck)) run_rule(r, t);
    }
  }
  tick(now);
}

void Automation::tick(double now) {
  if (now < 0) now = net::now_seconds();
  std::vector<Hide> due;
  {
    std::lock_guard<std::mutex> lk(m_);
    clock_ = now;
    for (auto it = hides_.begin(); it != hides_.end();) {
      if (it->due <= now) {
        due.push_back(*it);
        it = hides_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto& h : due) {
    Json e = Json::object();
    e.set("sceneName", Json(h.scene));
    e.set("sceneItemId", Json(h.item));
    e.set("sceneItemEnabled", Json(false));
    if (send_) send_("SetSceneItemEnabled", e.dump(), nullptr);
  }
}

}  // namespace deckhand::broadcast
