#pragma once
// Declarative rules that connect now-playing events to OBS (no scripting). Plain structs, loadable from settings.
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "deckhand/broadcast/nowplaying.hpp"
#include "deckhand/broadcast/obs.hpp"

namespace deckhand::broadcast {

struct AutomationRule {
  enum class Trigger { TrackChange, DeckStart, DeckStop };
  enum class Action {
    SetText,       // set text input `source` to the rendered `text` template      (any trigger)
    SwitchScene,   // switch the program scene to `scene`                           (any trigger)
    ShowSource,    // show `source` in `scene`, hide it again after `seconds`       (any trigger)
  };
  Trigger trigger = Trigger::TrackChange;
  int deck = 0;  // 1-based deck the trigger applies to; 0 = any deck
  Action action = Action::SetText;
  std::string source;  // OBS text input / scene-item source name
  std::string scene;   // OBS scene name
  std::string text = "{artist} - {title}";  // template: {title} {artist} {album} {key} {bpm} {deck}
  double seconds = 5.0;
  bool enabled = true;
};

class Automation {
 public:
  using Sender = std::function<void(const std::string& type, const std::string& json, ObsCallback cb)>;

  explicit Automation(ObsClient& obs);  // the client must outlive this object
  explicit Automation(Sender send);     // e.g. for tests or a custom transport
  void set_rules(std::vector<AutomationRule> rules);

  // Wire: NowPlaying::on_track_change -> Automation::on_track_change; the update loop -> Automation::update.
  // TrackChange rules run here (deck filter = the deck that is now on air).
  void on_track_change(const TrackInfo& t);
  // Detects deck start/stop edges (first call only records the baseline) and runs the "hide after T seconds" timers.
  void update(const NowPlayingState& state, double now_seconds = -1);
  // Timers only; update() calls it.
  void tick(double now_seconds = -1);

 private:
  struct Hide {
    std::string scene;
    long long item = -1;
    double due = 0;
  };
  void run_rule(const AutomationRule& r, const TrackInfo& t);

  Sender send_;
  std::mutex m_;
  std::vector<AutomationRule> rules_;
  std::vector<char> was_playing_;
  bool have_baseline_ = false;
  std::vector<Hide> hides_;
  double clock_ = 0;
};

}  // namespace deckhand::broadcast
