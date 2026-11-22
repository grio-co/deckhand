#pragma once
// Feeds deck state to the streaming outputs: now-playing files, the browser overlay, and OBS.
#include <functional>
#include <memory>
#include <string>

#include "deckhand/audio/engine.hpp"
#include "deckhand/library/library.hpp"

namespace deckhand {

class StreamBridge {
 public:
  // track_of(deck, row) fills the row of the track loaded on a deck; false if none. Called from
  // the bridge's own thread. Settings are read once from the library ("stream.*", "obs.*").
  StreamBridge(IEngine& engine, const std::string& data_dir, std::function<bool(int, TrackRow&)> track_of);
  ~StreamBridge();
  std::string describe() const;  // short status for the UI, empty when everything is off

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand
