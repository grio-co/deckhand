#pragma once
// Exposes the application over the REST + WebSocket API (see docs/API.md).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/audio/engine.hpp"

namespace deckhand {

class StemService;
class AutomixService;

// Arguments of a track-preparation action (grid editing, cues, loops). Which fields an action reads is in docs/API.md.
struct PrepArgs {
  bool has_value = false;
  double value = 0, value2 = 0;
  int slot = 0;       // 1-based hot cue slot or list index, 0 = not given
  int color = -1;     // 0xRRGGBB, -1 = none (used when has_color)
  bool has_color = false;
  bool has_text = false;
  std::string text;
};

// What the bridge needs from the application. Methods are called from API threads.
class ApiHost {
 public:
  virtual ~ApiHost() = default;
  virtual IEngine& api_engine() = 0;
  virtual bool api_load(int deck, std::int64_t track_id, std::string& error) = 0;
  virtual void api_add_file(const std::string& path, int deck) = 0;  // deck -1 = just add
  // convert_incompatible / player_profile: the export's compatibility fix and rule table (profile empty = default).
  virtual bool api_export(const std::string& destination, std::int64_t playlist_id, std::string& error,
                          bool convert_incompatible, const std::string& player_profile) = 0;
  // The compatibility report of the last export as JSON ({"report", "converted", "remaining_will_not_play"}), or empty.
  virtual std::string api_export_report() { return {}; }
  virtual void api_library_changed() = 0;
  // Cues of these tracks were written to the library by the API: decks holding them follow (default: nothing to do).
  virtual void api_cues_changed(const std::vector<std::int64_t>& track_ids) { (void)track_ids; }
  // start = true begins recording the master output; path receives the file in both cases.
  virtual bool api_record(bool start, std::string& path, std::string& error) = 0;
  virtual void api_load_sample(int slot, const std::string& path) = 0;
  // Grid and cue editing of the track loaded on `deck` (0-based). action: "grid.shift", "cue.memory_set" ... Same code as
  // the buttons. Returns false and an error text when it cannot be done.
  virtual bool api_prep(int deck, const std::string& action, const PrepArgs& args, std::string& error) = 0;
  // The deck's grid, grid lock, memory cues and hot cue names/colours as JSON text ("" when nothing is loaded).
  virtual std::string api_deck_prep_json(int deck) = 0;
  // Pre-listen: decode `track_id` and hand it to the headphone-only preview player (play = start at once). False with an
  // error when there is no headphone output and "preview on master" is off.
  virtual bool api_preview_load(std::int64_t track_id, bool play, std::string& error) = 0;
  virtual std::string api_preview_title() = 0;
  // The effective keyboard shortcuts as JSON, and replacing the user's overrides with a .dhkeys text ("" resets them).
  // Defaults: "not available".
  virtual std::string api_keymap_json() { return {}; }
  virtual bool api_keymap_set(const std::string& dhkeys_text, std::vector<std::string>& warnings, std::string& error) {
    (void)dhkeys_text; (void)warnings;
    error = "keyboard shortcuts are not available";
    return false;
  }
  // Stem separation (models, queue, cache); null when the host has none.
  virtual StemService* api_stems() { return nullptr; }
  // Automix and the play queue (service owns the queue); null when the host has none.
  virtual AutomixService* api_automix() { return nullptr; }
  // An automix.* control (action = the part after "automix."): start, stop, pause, skip, next_type ...
  virtual bool api_automix_control(const std::string& action, bool has_value, double value, std::string& error) {
    (void)action; (void)has_value; (void)value;
    error = "automix is not available";
    return false;
  }
  virtual std::string api_status() = 0;
  virtual std::string api_audio_device() = 0;
  // Debug (admin scope): the windows the UI has (JSON array) and what a click at (x, y) of a window would do (JSON object).
  // x and y are window points. Empty = not available. Runs on the UI thread, so the call waits for the next frame.
  virtual std::string api_debug_windows() { return {}; }
  virtual std::string api_debug_hit(const std::string& window, float x, float y) { (void)window; (void)x; (void)y; return {}; }
};

class ApiBridge {
 public:
  // Settings are read from the library ("api.enabled", "api.port", "api.bind", "api.tls").
  ApiBridge(ApiHost& host, const std::string& data_dir);
  ~ApiBridge();
  bool running() const;
  std::string describe() const;
  // Forward engine events here (scheduled commands that fired or were dropped). Any thread.
  void engine_event(const EngineEvent& e);  // e.g. "API http://127.0.0.1:7373"

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand
