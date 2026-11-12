#pragma once
// Command/event vocabulary shared by engine, UI and the public API.
#include <cstdint>
#include <variant>
#include "deckhand/core/model.hpp"

namespace deckhand {

enum class DeckId : std::uint8_t { A, B };

namespace cmd {
struct Load { DeckId deck; TrackId track; };
struct Play { DeckId deck; };
struct Pause { DeckId deck; };
struct SetCrossfader { float value; };
}  // namespace cmd
using Command = std::variant<cmd::Load, cmd::Play, cmd::Pause, cmd::SetCrossfader>;

namespace evt {
struct Position { DeckId deck; std::uint64_t frames; };
struct Loaded { DeckId deck; TrackId track; };
}  // namespace evt
using Event = std::variant<evt::Position, evt::Loaded>;

}  // namespace deckhand
