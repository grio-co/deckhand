#pragma once
// "Now playing" for stream overlays: decides which track is on air and publishes it as text files,
// a tracklist and a loopback HTTP overlay (an OBS Browser Source). No dependency on the app or audio modules:
// the application feeds NowPlayingState from its own decks.
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace deckhand::broadcast {

class OverlayServer;

struct DeckState {
  bool loaded = false;
  bool playing = false;
  double audible = 0;  // 0..1: channel fader combined with the crossfader weight for this deck
  std::string title, artist, album, key;
  double bpm = 0;
  double position_seconds = 0;
  double length_seconds = 0;
  std::string artwork;  // optional path to a png/jpg/gif/webp
};

struct NowPlayingState {
  std::vector<DeckState> decks;  // index 0 = deck 1
};

struct TrackInfo {
  int deck = 0;  // 1-based
  std::string title, artist, album, key;
  double bpm = 0;
  double length_seconds = 0;
  std::string artwork;
};

struct TracklistEntry {
  double offset_seconds = 0;  // since the first track went on air
  TrackInfo track;
  std::string label;  // tracklist template applied
};

struct NowPlayingConfig {
  double hold_seconds = 8.0;     // a deck must be the loudest playing one this long before its track is "on air"
  double min_audible = 0.05;     // below this a deck does not count as audible
  double switch_margin = 0.1;    // a challenger must beat the current deck by this much while that deck still sounds
  std::string text_template = "{artist} - {title}";       // {title} {artist} {album} {key} {bpm} {deck}
  std::string tracklist_template = "{artist} - {title}";
  struct Files {
    bool enabled = false;
    std::string folder;  // created if missing
  } files;               // title.txt artist.txt now-playing.txt now-playing.json tracklist.txt
  struct Http {
    bool enabled = false;
    std::uint16_t port = 8765;  // 0 = pick an ephemeral port (see NowPlaying::http_port)
  } http;                       // always bound to 127.0.0.1
};

class NowPlaying {
 public:
  explicit NowPlaying(NowPlayingConfig cfg = {});
  ~NowPlaying();
  NowPlaying(const NowPlaying&) = delete;
  NowPlaying& operator=(const NowPlaying&) = delete;

  // Replaces the configuration (restarts the HTTP server if needed). Thread-safe.
  void configure(const NowPlayingConfig& cfg);

  // Feed the live deck state, e.g. 10-30 times a second from the UI/control thread. Cheap when nothing changes.
  // Outputs (file writes, HTTP push) and the track-change callback run on the calling thread, only on changes.
  // now_seconds: monotonic time; negative = use the steady clock (tests pass explicit times).
  void update(const NowPlayingState& state, double now_seconds = -1);

  // Called on the thread that called update() when a new track goes on air.
  void on_track_change(std::function<void(const TrackInfo&)> cb);

  std::optional<TrackInfo> current() const;
  std::vector<TracklistEntry> tracklist() const;
  // "mm:ss label" lines (h:mm:ss after an hour), first line 00:00; paste-ready for stream descriptions.
  std::string tracklist_text() const;
  // Forget the current track and the tracklist (start of a new set); rewrites the outputs.
  void reset();

  // Port of the overlay server, 0 when it is not running.
  std::uint16_t http_port() const;

  // Substitutes {title} {artist} {album} {key} {bpm} {deck}; unknown placeholders are left untouched.
  static std::string render(const std::string& tmpl, const TrackInfo& t);
  // The now-playing.json payload. for_http=true replaces the artwork path by a URL served by the overlay.
  static std::string to_json(const std::optional<TrackInfo>& t, const std::string& text, bool for_http);

 private:
  struct Candidate {
    int deck = 0;
    std::string identity;
    double since = 0;
  };
  void write_outputs_locked(bool tracklist_changed);  // lock held
  void publish_http_locked();

  mutable std::mutex m_;
  NowPlayingConfig cfg_;
  std::unique_ptr<OverlayServer> server_;
  std::function<void(const TrackInfo&)> on_change_;
  std::optional<TrackInfo> current_;
  std::string current_identity_;
  std::optional<Candidate> cand_;
  std::vector<TracklistEntry> tracklist_;
  double first_time_ = 0;
  double last_refresh_ = -1e9;
};

}  // namespace deckhand::broadcast
