// The mixer laws as settings: the table, defaults, clamping on load and store, and that a garbled stored value falls back.
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include "check.hpp"
#include "deckhand/library/library.hpp"
#include "mixer_prefs.hpp"

using namespace deckhand;

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "deckhand_test_mixer_prefs";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);
  {
    Library lib((dir / "library.db").string());
    // one entry per engine field, unique keys and names, ranges inside what the engine accepts
    std::set<std::string> keys, names;
    CHECK(mixer_prefs().size() == static_cast<std::size_t>(mixer::MixerField::Count));
    for (const MixerPref& p : mixer_prefs()) {
      CHECK(keys.insert(p.key).second && names.insert(p.name).second);
      CHECK(std::string(p.key) == std::string("mixer.") + p.name);
      CHECK(mixer::clamp_setting(p.field, p.lo) == p.lo && mixer::clamp_setting(p.field, p.hi) == p.hi);
      CHECK(mixer_pref_by_name(p.name) == &p);
    }
    CHECK(!mixer_pref_by_name("nonsense"));
    // nothing stored: the defaults, which are the engine's original behaviour
    const mixer::Settings d;
    const mixer::Settings s0 = mixer_settings_load(lib);
    for (const MixerPref& p : mixer_prefs()) CHECK(mixer_value(s0, p.field) == mixer_value(d, p.field));
    // store and load; values are clamped
    const MixerPref* kill = mixer_pref_by_name("eq_kill_db");
    CHECK(mixer_setting_store(lib, *kill, 500.0f) == 120.0f);
    CHECK(mixer_setting_store(lib, *mixer_pref_by_name("crossfader_law"), 1.0f) == 1.0f);
    const mixer::Settings s1 = mixer_settings_load(lib);
    CHECK(s1.eq_kill_db == 120.0f && s1.crossfader_law == 1 && s1.eq_boost_db == d.eq_boost_db);
    // garbage and empty values fall back
    lib.set_setting("mixer.eq_boost_db", "loud");
    lib.set_setting("mixer.eq_kill_db", "");
    const mixer::Settings s2 = mixer_settings_load(lib);
    CHECK(s2.eq_boost_db == d.eq_boost_db && s2.eq_kill_db == d.eq_kill_db);
    // out-of-range stored text is clamped on load
    lib.set_setting("mixer.eq_low_hz", "5");
    CHECK(mixer_settings_load(lib).eq_low_hz == 60.0f);
  }
  std::filesystem::remove_all(dir, ec);
  std::puts("mixer prefs ok");
  return 0;
}
