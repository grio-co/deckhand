// Browser layout presets: built-ins, saving the user's own, persistence, damaged settings, stepping.
#include <cstdio>
#include <filesystem>

#include "../src/app/browser_layouts.hpp"
#include "check.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

int main() {
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_browser_layouts";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  {
    Library lib((dir / "lib.db").string());
    BrowserLayouts ls(lib);
    CHECK(ls.all().size() == 3 && ls.all()[0].name == "prepare" && ls.all()[1].name == "perform" && ls.all()[2].name == "tags");
    for (const auto& l : ls.all()) {
      CHECK(l.builtin && !l.columns.empty());
      for (const auto& c : l.columns) CHECK(std::find(browser_column_ids().begin(), browser_column_ids().end(), c) != browser_column_ids().end());
    }
    CHECK(ls.find("perform")->sort == SortKey::Bpm);
    CHECK(ls.active().empty());

    BrowserLayout mine;
    mine.name = "  My; set|up ";
    mine.columns = {"artist", "title", "nonsense", "title", "bpm"};
    mine.widths["artist"] = 1.5f;
    mine.sort = SortKey::Key;
    mine.descending = true;
    CHECK(ls.save(mine));
    CHECK(ls.find("My  set up") != nullptr);
    CHECK(!ls.save(BrowserLayout{"perform", false, {"title"}, {}, SortKey::Default, false}));  // a built-in name
    CHECK(!ls.save(BrowserLayout{"", false, {"title"}, {}, SortKey::Default, false}));
    CHECK(!ls.save(BrowserLayout{"empty", false, {}, {}, SortKey::Default, false}));
    ls.set_active("My  set up");
  }
  {
    Library lib((dir / "lib.db").string());  // survives a restart
    BrowserLayouts ls(lib);
    CHECK(ls.all().size() == 4);
    const BrowserLayout* l = ls.find("My  set up");
    CHECK(l && !l->builtin && (l->columns == std::vector<std::string>{"artist", "title", "bpm"}));
    CHECK(l->widths.at("artist") == 1.5f && l->sort == SortKey::Key && l->descending);
    CHECK(ls.active() == "My  set up");
    CHECK(ls.step("tags", 1).name == "My  set up");
    CHECK(ls.step("My  set up", 1).name == "prepare");
    CHECK(ls.step("prepare", -1).name == "My  set up");
    CHECK(ls.step("unknown", 1).name == "prepare");
    // overwrite, then remove
    BrowserLayout again = *l;
    again.columns = {"title"};
    CHECK(ls.save(again) && ls.all().size() == 4 && ls.find("My  set up")->columns.size() == 1);
    CHECK(!ls.remove("tags"));
    CHECK(ls.remove("My  set up") && ls.all().size() == 3 && ls.active().empty());
    // damaged settings are skipped
    lib.set_setting("browser.layouts", "junk;ok|title,bpm||bpm|1;bad|nope||none|0;|title||none|0;prepare|title||none|0");
    ls.reload();
    CHECK(ls.all().size() == 4 && ls.find("ok") && ls.find("ok")->descending);
  }
  fs::remove_all(dir, ec);
  std::puts("browser layouts: all checks passed");
  return 0;
}
