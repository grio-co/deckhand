#include "deckhand/app/stream_bridge.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "deckhand/broadcast/automation.hpp"
#include "deckhand/broadcast/nowplaying.hpp"
#include "deckhand/broadcast/obs.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {

struct StreamBridge::Impl {
  Engine& engine;
  std::function<bool(int, TrackRow&)> track_of;
  std::unique_ptr<broadcast::NowPlaying> now_playing;
  std::unique_ptr<broadcast::ObsClient> obs;
  std::unique_ptr<broadcast::Automation> automation;
  std::atomic<bool> quit{false};
  std::thread thread;
  std::string description;

  Impl(Engine& e, std::function<bool(int, TrackRow&)> t) : engine(e), track_of(std::move(t)) {}

  void run() {
    while (!quit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      const MixerView mixer = engine.mixer();
      broadcast::NowPlayingState state;
      for (int i = 0; i < kMaxDecks; ++i) {
        const DeckView v = engine.deck(i);
        broadcast::DeckState d;
        d.loaded = v.loaded;
        d.playing = v.playing;
        // Decks 1 and 3 sit on the left of the crossfader, 2 and 4 on the right.
        const double side = i % 2 == 0 ? std::min(1.0, 2.0 * (1.0 - mixer.crossfader)) : std::min(1.0, 2.0 * mixer.crossfader);
        d.audible = static_cast<double>(v.fader) * side;
        d.bpm = v.bpm;
        if (v.sample_rate > 0) {
          d.position_seconds = v.position / v.sample_rate;
          d.length_seconds = v.length / v.sample_rate;
        }
        TrackRow row;
        if (v.loaded && track_of(i, row)) {
          d.title = row.title;
          d.artist = row.artist;
          d.album = row.album;
          d.key = row.key;
        }
        state.decks.push_back(std::move(d));
      }
      now_playing->update(state);
      if (automation) automation->update(state);
    }
  }
};

StreamBridge::StreamBridge(Engine& engine, const std::string& data_dir, std::function<bool(int, TrackRow&)> track_of)
    : impl_(std::make_unique<Impl>(engine, std::move(track_of))) {
  try {
    Library lib(data_dir + "/library.db");
    broadcast::NowPlayingConfig cfg;
    cfg.files.enabled = lib.setting("stream.files", "0") == "1";
    cfg.files.folder = lib.setting("stream.folder", data_dir + "/NowPlaying");
    cfg.http.enabled = lib.setting("stream.overlay", "0") == "1";
    cfg.http.port = static_cast<std::uint16_t>(std::clamp(std::atoi(lib.setting("stream.overlay_port", "8765").c_str()), 1, 65535));
    const std::string text = lib.setting("stream.template");
    if (!text.empty()) cfg.text_template = text;
    impl_->now_playing = std::make_unique<broadcast::NowPlaying>(cfg);
    if (cfg.files.enabled) impl_->description += "files";
    if (cfg.http.enabled) impl_->description += std::string(impl_->description.empty() ? "" : " + ") + "overlay :" + std::to_string(impl_->now_playing->http_port());

    if (lib.setting("obs.enabled", "0") == "1") {
      broadcast::ObsConfig oc;
      oc.host = lib.setting("obs.host", "127.0.0.1");
      oc.port = static_cast<std::uint16_t>(std::clamp(std::atoi(lib.setting("obs.port", "4455").c_str()), 1, 65535));
      oc.password = lib.setting("obs.password");
      impl_->obs = std::make_unique<broadcast::ObsClient>();
      impl_->obs->configure(oc);
      impl_->automation = std::make_unique<broadcast::Automation>(*impl_->obs);
      std::vector<broadcast::AutomationRule> rules;
      const std::string source = lib.setting("obs.text_source");
      if (!source.empty()) {
        broadcast::AutomationRule r;
        r.source = source;
        const std::string t = lib.setting("obs.text_template");
        if (!t.empty()) r.text = t;
        rules.push_back(std::move(r));
      }
      impl_->automation->set_rules(std::move(rules));
      impl_->now_playing->on_track_change([this](const broadcast::TrackInfo& t) { impl_->automation->on_track_change(t); });
      impl_->obs->start();
      impl_->description += std::string(impl_->description.empty() ? "" : " + ") + "OBS";
    }
    impl_->thread = std::thread([this] { impl_->run(); });
    if (!impl_->description.empty()) DH_LOG(Info, "streaming outputs: %s", impl_->description.c_str());
  } catch (const std::exception& e) {
    DH_LOG(Error, "streaming outputs did not start: %s", e.what());
  }
}

StreamBridge::~StreamBridge() {
  impl_->quit = true;
  if (impl_->thread.joinable()) impl_->thread.join();
  if (impl_->obs) impl_->obs->stop();
}

std::string StreamBridge::describe() const { return impl_->description; }

}  // namespace deckhand
