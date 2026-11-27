// The second browser pane: slot geometry, snapshot encoding, persisted state.
#include <cstdio>
#include <filesystem>

#include "../src/app/browser_panes.hpp"
#include "check.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

int main() {
  const SlotRect r{10, 20, 206, 100};
  auto side = split_slots(r, SplitMode::Side, 6);
  CHECK(side[0].w == 100 && side[1].x == 116 && side[1].w == 100 && side[0].h == 100);
  auto stack = split_slots(r, SplitMode::Stacked, 6);
  CHECK(stack[0].h == 47 && stack[1].y == 20 + 47 + 6 && stack[1].w == 206);
  CHECK(split_slots(r, SplitMode::Off)[0].w == 206);
  CHECK(slot_at(side, SplitMode::Side, 50, 50) == 0 && slot_at(side, SplitMode::Side, 150, 50) == 1);
  CHECK(slot_at(side, SplitMode::Side, 111, 50) == -1);  // the gap
  CHECK(slot_at(side, SplitMode::Side, 500, 50) == -1);
  CHECK(slot_at(split_slots(r, SplitMode::Off), SplitMode::Off, 150, 50) == 0);
  CHECK(next_split_mode(SplitMode::Off) == SplitMode::Side && next_split_mode(SplitMode::Stacked) == SplitMode::Off);

  PaneSnapshot p;
  p.kind = 5;
  p.id = 42;
  p.path = "/music/a b";
  p.sort = SortKey::Bpm;
  p.descending = true;
  p.search = "a|b c";
  PaneSnapshot q;
  CHECK(decode_pane(encode_pane(p), q) && q == p);
  PaneSnapshot bad_path = p;
  bad_path.path = "x|y";
  CHECK(decode_pane(encode_pane(bad_path), q) && q.path.empty() && q.search == "a|b c");
  CHECK(!decode_pane("", q) && !decode_pane("x|1|none|0||", q) && !decode_pane("1|2|none", q));
  CHECK(decode_pane("0|0|nonsense|1||", q) && q.sort == SortKey::Default && !q.descending);

  const fs::path dir = fs::temp_directory_path() / "deckhand_test_browser_panes";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  {
    Library lib((dir / "lib.db").string());
    PaneSettings d = load_pane_settings(lib);
    CHECK(d.mode == SplitMode::Off && d.live_slot == 0 && d.pane[1] == PaneSnapshot{});
    d.mode = SplitMode::Stacked;
    d.live_slot = 1;
    d.pane[0] = p;
    save_pane_settings(lib, d);
  }
  {
    Library lib((dir / "lib.db").string());
    const PaneSettings d = load_pane_settings(lib);
    CHECK(d.mode == SplitMode::Stacked && d.live_slot == 1 && d.pane[0] == p && d.pane[1] == PaneSnapshot{});
  }
  fs::remove_all(dir, ec);
  std::puts("browser panes: all checks passed");
  return 0;
}
