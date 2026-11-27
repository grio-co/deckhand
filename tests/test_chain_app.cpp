// Application layer of the effect chains: named presets on disk, the plug-in service, and the chain controls for plug-ins
// and presets. Apple's built-in units only (never a third-party plug-in), offline engine, nothing opens a device.
// Without plug-in support the plug-in part is skipped.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/app/chain_presets.hpp"
#include "deckhand/app/controls.hpp"
#include "deckhand/app/plugin_service.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/plugin_host.hpp"
#include "deckhand/plugins/catalog.hpp"
#include "deckhand/plugins/plugin.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

static void test_presets(const std::string& dir) {
  chain_presets_set_dir(dir + "/fx-presets");
  CHECK(chain_presets_list().empty());
  CHECK(chain_preset_clean_name("  a/b\\c:d  ") == "abcd");
  CHECK(chain_preset_clean_name("..").empty());
  CHECK(chain_preset_clean_name(std::string(200, 'x')).size() == 60);
  std::string err;
  CHECK(!chain_preset_save("", "{}", err) && !err.empty());
  CHECK(chain_preset_save("Big Echo", "{\"v\":1}", err));
  CHECK(chain_preset_save("alpha", "{\"v\":2}", err));
  const auto names = chain_presets_list();
  CHECK(names.size() == 2 && names[0] == "Big Echo" && names[1] == "alpha");
  std::string body;
  CHECK(chain_preset_read("alpha", body, err) && body == "{\"v\":2}");
  CHECK(!chain_preset_read("nope", body, err));
  CHECK(chain_preset_delete("alpha", err) && !chain_preset_delete("alpha", err));
  CHECK(chain_presets_list().size() == 1);
}

static void test_builtin_controls() {
  Engine e;
  e.configure_offline(48000, 2);
  std::string err, out;
  // the three newest effects and a plug-in number through the numeric insert
  for (const char* slug : {"tape_echo", "beat_reverb", "tape_stop"}) {
    const EffectKind k = effect_from_slug(slug);
    CHECK(k != EffectKind::None);
    CHECK(apply_chain_control(e, "deck1.chain.insert", true, static_cast<double>(k), "", out, err));
  }
  CHECK(e.fx_chain(0).count == 3);
  CHECK(!apply_chain_control(e, "deck1.chain.insert", true, static_cast<double>(EffectKind::Plugin), "", out, err));  // has its own control
  CHECK(!apply_chain_control(e, "deck1.chain.insert_plugin", false, 0, "", out, err));                               // needs an id
  CHECK(!apply_chain_control(e, "deck1.chain.insert_plugin", false, 0, "au:aufx:nope:nope", out, err) && !err.empty());
  CHECK(effect_count() == static_cast<int>(EffectKind::Count) - 2);
}

#if defined(__APPLE__) && !defined(DECKHAND_PLUGINS_DISABLED)
static std::string find_id(const char* sub) {
  for (const auto& p : plugins::scan_installed())
    if (p.type == "aufx" && p.subtype == sub && p.manufacturer == "appl") return p.id;
  return {};
}

static void test_plugins(const std::string& dir) {
  const std::string pdir = dir + "/plugins";
  fs::create_directories(pdir);
  plugins::PluginCatalog cat(pdir + "/plugins.cache");
  cat.scan();  // metadata only: nothing is instantiated
  plugin_service().set_dir(pdir);
  plugin_host_set_probe_exe(DH_PLUGIN_PROBE_EXE);
  CHECK(plugin_service().status().supported);
  const std::string delay = find_id("dely"), lp = find_id("lpas");
  CHECK(!delay.empty() && !lp.empty());
  {
    bool seen = false;
    for (const PluginRow& r : plugin_service().list()) {
      CHECK(!r.vetted);  // nothing has been probed yet
      seen = seen || r.id == delay;
    }
    CHECK(seen);
  }
  std::string err;
  CHECK(plugin_service().vet(delay, err));
  for (const PluginRow& r : plugin_service().list()) CHECK(r.vetted == (r.id == delay));

  Engine e;
  e.configure_offline(48000, 2);
  std::string out;
  CHECK(apply_chain_control(e, "deck2.chain.insert_plugin", false, 0, delay, out, err));
  const int id = std::atoi(out.c_str());
  CHECK(id > 0 && e.fx_chain(1).count == 1 && e.fx_chain(1).unit[0].kind == EffectKind::Plugin);
  CHECK(apply_chain_control(e, ("deck2.chain.unit.pp0." + std::to_string(id)).c_str(), true, 0.25, "", out, err));
  CHECK(!apply_chain_control(e, ("deck2.chain.unit.pp900." + std::to_string(id)).c_str(), true, 0.25, "", out, err));
  CHECK(!apply_chain_control(e, ("deck2.chain.unit.pp0." + std::to_string(id)).c_str(), true, 2.0, "", out, err));  // 0..1 only
  CHECK(apply_chain_control(e, ("deck2.chain.unit.on." + std::to_string(id)).c_str(), true, 1, "", out, err));
  PluginOp op;
  op.target = 1;
  op.id = id;
  PluginOpResult pr;
  CHECK(e.plugin_op(op, pr));
  PluginInfoView view;
  CHECK(parse_plugin_info(pr.data, view) && view.loaded && !view.params.empty() && view.id == delay);
  CHECK(view.params[0].norm > 0.2 && view.params[0].norm < 0.3);

  // named presets: save, wipe, load
  CHECK(apply_chain_control(e, "deck2.chain.preset_save", false, 0, "my delay", out, err));
  CHECK(apply_chain_control(e, "deck2.chain.clear", false, 0, "", out, err) && e.fx_chain(1).count == 0);
  CHECK(apply_chain_control(e, "deck3.chain.preset_load", false, 0, "my delay", out, err));  // another bus takes it too
  CHECK(e.fx_chain(2).count == 1 && e.fx_chain(2).unit[0].kind == EffectKind::Plugin);
  PluginOp op3;
  op3.target = 2;
  op3.id = e.fx_chain(2).unit[0].id;
  CHECK(e.plugin_op(op3, pr) && parse_plugin_info(pr.data, view) && view.params[0].norm > 0.2 && view.params[0].norm < 0.3);
  CHECK(!apply_chain_control(e, "deck3.chain.preset_load", false, 0, "no such preset", out, err));
  CHECK(apply_chain_control(e, "deck3.chain.preset_delete", false, 0, "my delay", out, err));
  // a preset naming a plug-in that was blocked loads as a pass-through and says so
  plugins::PluginCatalog cat2(pdir + "/plugins.cache");
  cat2.block(delay, "test");
  CHECK(!apply_chain_control(e, "deck2.chain.insert_plugin", false, 0, delay, out, err) && err.find("blocked") != std::string::npos);
}
#endif

int main() {
  const std::string dir = (fs::temp_directory_path() / ("dh-chain-app-" + std::to_string(static_cast<long>(std::hash<std::string>{}(std::to_string(reinterpret_cast<std::uintptr_t>(&main))))))).string();
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  test_presets(dir);
  test_builtin_controls();
#if defined(__APPLE__) && !defined(DECKHAND_PLUGINS_DISABLED)
  test_plugins(dir);
#else
  std::printf("plug-ins not supported here: skipping the plug-in part\n");
#endif
  plugin_service().wait();
  fs::remove_all(dir, ec);
  std::printf("chain app ok\n");
  return 0;
}
