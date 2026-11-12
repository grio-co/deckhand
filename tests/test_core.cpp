#include <cassert>
#include "deckhand/core/model.hpp"
#include "deckhand/log/log.hpp"

int main() {
  deckhand::Playlist p;
  assert(p.tracks.empty());
  assert(deckhand::TrackId{1} == deckhand::TrackId{1});
  deckhand::log::set_level(deckhand::log::Level::Debug);
  assert(deckhand::log::enabled(deckhand::log::Level::Info));
  assert(!deckhand::log::enabled(deckhand::log::Level::Trace));
  return 0;
}
