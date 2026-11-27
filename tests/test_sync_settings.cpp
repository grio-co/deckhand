// The sync settings keys and their conversion to SyncConfig (header only: deckhand/app/sync_settings.hpp).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "check.hpp"
#include "deckhand/app/sync_settings.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

int main() {
  CHECK(parse_id_list("3, 7,x,7,0,-2,12").empty() == false);
  const auto ids = parse_id_list("3,7,x,7,0,-2,12");
  CHECK(ids.size() == 3 && ids[0] == 3 && ids[1] == 7 && ids[2] == 12);
  CHECK(format_id_list(ids) == "3,7,12");
  CHECK(parse_id_list("").empty());
  CHECK(mib_setting("") == 0 && mib_setting("-5") == 0 && mib_setting("abc") == 0);
  CHECK(mib_setting("2") == (2ull << 20));

  const fs::path dir = fs::temp_directory_path() / ("dh-sync-settings-" + std::to_string(reinterpret_cast<std::uintptr_t>(&ids)));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  {
    Library lib((dir / "library.db").string());
    deckhand::sync::SyncConfig cfg;
    load_sync_options(lib, cfg);  // defaults
    CHECK(cfg.sync_mix_links && !cfg.sync_analysis && cfg.audio_playlists.empty() && cfg.music_folder.empty());
    CHECK(cfg.audio_budget_bytes == 0 && cfg.audio_import_budget_bytes == 0 && !cfg.allow_unencrypted_audio);
    CHECK(!sync_audio_wanted(cfg));
    lib.set_setting("sync.mix_links", "0");
    lib.set_setting("sync.analysis", "1");
    lib.set_setting("sync.audio_playlists", "4,9");
    lib.set_setting("sync.music_folder", "/tmp/music");
    lib.set_setting("sync.audio_budget_mb", "512");
    lib.set_setting("sync.audio_import_budget_mb", "64");
    lib.set_setting("sync.allow_unencrypted_audio", "1");
    load_sync_options(lib, cfg);
    CHECK(!cfg.sync_mix_links && cfg.sync_analysis && cfg.audio_playlists.size() == 2 && cfg.music_folder == "/tmp/music");
    CHECK(cfg.audio_budget_bytes == (512ull << 20) && cfg.audio_import_budget_bytes == (64ull << 20) && cfg.allow_unencrypted_audio);
    CHECK(sync_audio_wanted(cfg));
    lib.set_setting("sync.audio_playlists", "");
    load_sync_options(lib, cfg);
    CHECK(sync_audio_wanted(cfg));  // a music folder alone means "import"
    lib.set_setting("sync.music_folder", "");
    load_sync_options(lib, cfg);
    CHECK(!sync_audio_wanted(cfg));
  }
  {  // stored audio nothing uses any more: removed after audio_cleanup_days (default 30, 0 = at once, negative = never)
    CHECK(audio_cleanup_days("") == 30 && audio_cleanup_days("abc") == 30 && audio_cleanup_days("7") == 7 && audio_cleanup_days("0") == 0);
    CHECK(audio_cleanup_days("-1") == -1 && audio_cleanup_days("-40") == -1 && audio_cleanup_days("999999") == 36500);
    CHECK(audio_cleanup_grace_ms("") == 30ll * 24 * 3600 * 1000 && audio_cleanup_grace_ms("0") == 0 && audio_cleanup_grace_ms("2") == 2ll * 24 * 3600 * 1000);
    CHECK(audio_cleanup_grace_ms("-1") < 0);
    Library lib((dir / "cleanup.db").string());
    deckhand::sync::SyncConfig cfg;
    load_sync_options(lib, cfg);
    CHECK(cfg.audio_cleanup_grace_ms == 30ll * 24 * 3600 * 1000);
    lib.set_setting("sync.audio_cleanup_days", "0");
    load_sync_options(lib, cfg);
    CHECK(cfg.audio_cleanup_grace_ms == 0);
    lib.set_setting("sync.audio_cleanup_days", "-3");
    load_sync_options(lib, cfg);
    CHECK(cfg.audio_cleanup_grace_ms < 0);
  }
  fs::remove_all(dir, ec);
  std::printf("sync settings ok\n");
  return 0;
}
