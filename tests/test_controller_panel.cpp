// The Controller panel's parts that need no window: per-controller preferences (settings round trip, one set per
// controller), mapping file inventory, and that the skin's click target resolves. Never opens a MIDI device.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "check.hpp"
#include "deckhand/app/controller_prefs.hpp"
#include "deckhand/app/keymap.hpp"
#include "skin_view.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

int main() {
  CHECK(controller_slug("Pioneer DDJ-FLX4") == "pioneer_ddj_flx4");
  CHECK(controller_slug("  --DDJ  FLX4-- ") == "ddj_flx4");
  CHECK(controller_slug("") == "controller" && controller_slug("!!!") == "controller");
  CHECK(controller_slug(std::string(100, 'a')).size() == 40);

  const fs::path dir = fs::temp_directory_path() / "deckhand_test_controller_panel";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    Library lib((dir / "library.db").string());
    // defaults: nothing stored
    ControllerPrefs d = load_controller_prefs(lib, "DDJ-FLX4");
    CHECK(d == ControllerPrefs{} && d.jog_scale == 1.0 && d.leds && d.pickup == -1 && d.ticks_per_rev == 0);
    // a round trip, and a second controller keeps its own
    ControllerPrefs a;
    a.jog_scale = 1.5;
    a.ticks_per_rev = 1440;
    a.pickup = 1;
    a.leds = false;
    a.mapping = "My FLX4";
    save_controller_prefs(lib, "DDJ-FLX4", a);
    ControllerPrefs b;
    b.jog_scale = 0.5;
    b.pickup = 0;
    save_controller_prefs(lib, "Other Box", b);
    CHECK(load_controller_prefs(lib, "DDJ-FLX4") == a);
    CHECK(load_controller_prefs(lib, "Other Box") == b);
    CHECK(load_controller_prefs(lib, "Never Seen") == ControllerPrefs{});
    CHECK(lib.setting("controller.ddj_flx4.jog_scale") == "1.5" && lib.setting("controller.other_box.jog_scale") == "0.5");
    // out of range values are clamped on the way in and the way out
    ControllerPrefs wild;
    wild.jog_scale = 99;
    wild.ticks_per_rev = 3;
    save_controller_prefs(lib, "Wild", wild);
    CHECK(load_controller_prefs(lib, "Wild").jog_scale == 8.0 && load_controller_prefs(lib, "Wild").ticks_per_rev == 0);
    lib.set_setting("controller.garbled.jog_scale", "banana");
    CHECK(load_controller_prefs(lib, "Garbled").jog_scale == 1.0);
    // back to defaults removes what was stored
    save_controller_prefs(lib, "DDJ-FLX4", ControllerPrefs{});
    CHECK(load_controller_prefs(lib, "DDJ-FLX4") == ControllerPrefs{} && lib.setting("controller.ddj_flx4.jog_scale").empty());
    // the hub sees the same numbers
    const DeviceOptions o = to_device_options(a);
    CHECK(o.jog_scale == 1.5 && o.ticks_per_rev == 1440 && o.pickup == 1 && !o.leds);
    // controllers seen before, and the mapping choices to hand to the hub
    note_seen_controller(lib, "DDJ-FLX4");
    note_seen_controller(lib, "Other Box");
    note_seen_controller(lib, "DDJ-FLX4");
    CHECK(seen_controllers(lib) == std::vector<std::string>({"DDJ-FLX4", "Other Box"}));
    save_controller_prefs(lib, "DDJ-FLX4", a);
    CHECK(preferred_mappings(lib, {"DDJ-FLX4", "Other Box"}) == std::vector<std::string>({"My FLX4"}));
  }

  // the mapping files: the shipped one, and a user copy that competes for the same device
  {
    const fs::path user = dir / "user";
    fs::create_directories(user);
    std::ifstream in("controllers/ddj-flx4.dhmap");
    CHECK(in.good());
    std::stringstream text;
    text << in.rdbuf();
    std::string copy = text.str();
    const auto at = copy.find("name  \"DDJ-FLX4\"");
    CHECK(at != std::string::npos);
    copy.replace(at, 16, "name \"My FLX4\"");
    std::ofstream(user / "my-flx4.dhmap", std::ios::binary) << copy;
    const std::vector<MappingFileInfo> files = scan_mapping_files({"controllers", user.string()});
    const MappingFileInfo* stock = nullptr;
    for (const MappingFileInfo& f : files)
      if (f.name == "DDJ-FLX4") stock = &f;
    CHECK(stock && stock->path.find("ddj-flx4.dhmap") != std::string::npos);
    CHECK(stock->decks == std::vector<int>({1, 2}) && stock->ticks_per_rev == 720 && !stock->audio_match.empty());
    bool has_sampler = false, has_alt = false;
    for (const std::string& m : stock->pad_modes) has_sampler = has_sampler || m == "sampler";
    for (const std::string& m : stock->alt_modes) has_alt = has_alt || m == "sampler / stems";  // the button with a second mode
    CHECK(has_sampler && has_alt);
    CHECK(competing_mappings(files, stock->match).size() == 2);
    CHECK(competing_mappings(files, "something else").empty());
    // a later folder overrides an earlier file of the same name
    std::ofstream(user / "stock-again.dhmap", std::ios::binary) << text.str();
    std::size_t n = 0;
    for (const MappingFileInfo& f : scan_mapping_files({"controllers", user.string()}))
      if (f.name == "DDJ-FLX4") {
        ++n;
        CHECK(f.path.find("stock-again") != std::string::npos);
      }
    CHECK(n == 1);
  }
  fs::remove_all(dir);

  // the click target: the status row's controller name opens the panel, in every skin that shows it
  {
    const ui::Target t = ui::map_binding("app.controller.open");
    CHECK(t.type == ui::Target::Type::Action && t.name == "controller_open");
    CHECK(ui::map_binding("app.controller").type == ui::Target::Type::Info);  // the plain label still works (other skins)
    std::ifstream sk("resources/skins/default.dhskin");
    std::stringstream st;
    st << sk.rdbuf();
    CHECK(st.str().find("app.controller.open") != std::string::npos && st.str().find("app.controller_hint") != std::string::npos);
  }
  {  // the shortcut exists and is bound by default
    bool known = false;
    for (const KeyControlInfo& c : list_key_controls()) known = known || c.name == "ui.controller";
    CHECK(known);
  }
  std::puts("test_controller_panel: ok");
  return 0;
}
