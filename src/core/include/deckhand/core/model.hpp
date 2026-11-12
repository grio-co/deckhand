#pragma once
// Domain model shared by every module. No I/O, no platform code.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace deckhand {

struct TrackId { std::uint64_t value{}; friend bool operator==(TrackId, TrackId) = default; };

struct Track {
  TrackId id;
  std::string title;
  std::string artist;
  std::string path;            // relative to a library root (portable mode)
  std::optional<float> bpm;
};

struct Playlist {
  std::string name;
  std::vector<TrackId> tracks;
};

struct NowPlaying {            // for stream overlays
  std::string title;
  std::string artist;
  std::optional<std::string> artwork;
};

}  // namespace deckhand
