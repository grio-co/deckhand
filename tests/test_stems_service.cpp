// The stem service's model install, end to end, with local files in place of downloads (nothing is fetched):
// catalogue -> gates -> download hook -> SHA-256 check -> Demucs conversion -> manifest -> installed / usable -> remove.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "check.hpp"
#include "deckhand/app/stems_service.hpp"

namespace fs = std::filesystem;
using namespace deckhand;

namespace {
const StemModelRow* find_row(const std::vector<StemModelRow>& rows, const std::string& id) {
  for (const StemModelRow& r : rows)
    if (r.entry.id == id) return &r;
  return nullptr;
}

// Waits until the install worker leaves "running" (event-driven, no polling).
std::string wait_install(StemService& svc, std::mutex& m, std::condition_variable& cv, bool& finished) {
  std::unique_lock<std::mutex> lock(m);
  cv.wait_for(lock, std::chrono::minutes(10), [&] { return finished; });
  lock.unlock();
  return svc.install_state().state;
}
}  // namespace

int main() {
  const fs::path data = fs::temp_directory_path() / "deckhand_test_stems_service";
  fs::remove_all(data);
  fs::create_directories(data);
  StemService svc(data.string(), DH_RESOURCE_DIR, [] { return false; });

  // Both Demucs catalogue entries are installable through the same converter.
  const auto rows = svc.models();
  const StemModelRow* single = find_row(rows, "demucs-htdemucs");
  const StemModelRow* bag = find_row(rows, "demucs-htdemucs-ft");
  CHECK(single && bag);
  CHECK(single->entry.kind == "demucs" && single->entry.install_supported && single->entry.files.size() == 1);
  CHECK(bag->entry.kind == "demucs" && bag->entry.install_supported && bag->entry.files.size() == 4);

  std::string error;
  CHECK(svc.install("no-such-model", true, error) == StemService::Result::NotFound);
  if (!svc.available()) {
    std::printf("stems disabled in this build: install checks skipped\n");
    fs::remove_all(data);
    return 0;
  }
  CHECK(svc.install("demucs-htdemucs", false, error) == StemService::Result::NeedLicence);

  // A recipe combines installed models: with its parts missing the install is refused (409), naming what is missing.
  const auto recipe_rows = svc.models();
  const StemModelRow* recipe = find_row(recipe_rows, "recipe-roformer-demucs");
  CHECK(recipe && recipe->entry.kind == "recipe" && recipe->entry.files.empty() && recipe->entry.requires_ids.size() == 2);
  error.clear();
  CHECK(svc.install("recipe-roformer-demucs", true, error) == StemService::Result::Refused);
  CHECK(error.find("melband-roformer-vocals") != std::string::npos && error.find("demucs-htdemucs") != std::string::npos);
  CHECK(!find_row(svc.models(), "recipe-roformer-demucs")->installed);

  std::mutex m;
  std::condition_variable cv;
  bool finished = false;
  svc.set_event_sink([&](const std::string& type, const std::string&, const std::string& state, float, const std::string&) {
    if (type != "stems.install" || state == "running") return;
    std::lock_guard<std::mutex> lock(m);
    finished = true;
    cv.notify_all();
  });
  const auto reset = [&] {
    std::lock_guard<std::mutex> lock(m);
    finished = false;
  };

  // A download that delivers the wrong bytes is refused by the hash check, and nothing is left installed.
  int calls = 0;
  svc.set_downloader([&](const std::string&, const fs::path& dest, std::uint64_t expected, const std::function<void(std::uint64_t)>& cb,
                         const std::atomic<bool>*, std::string*) {
    ++calls;
    std::ofstream(dest, std::ios::binary) << std::string(static_cast<std::size_t>(expected < 4096 ? expected : 4096), 'x');
    if (cb) cb(expected);
    return true;
  });
  reset();
  CHECK(svc.install("demucs-htdemucs", true, error) == StemService::Result::Ok);
  CHECK(wait_install(svc, m, cv, finished) == "failed");
  CHECK(calls >= 1 && !svc.install_state().error.empty());
  CHECK(!find_row(svc.models(), "demucs-htdemucs")->usable);

  // The real checkpoints, copied from third_party/models in place of the download (skipped when they are not there).
  const fs::path ckdir = fs::path(DH_STEMS_MODELS_DIR) / "demucs";
  const auto local = [&](const std::string& url, const fs::path& dest, std::uint64_t, const std::function<void(std::uint64_t)>& cb,
                         const std::atomic<bool>*, std::string*) {
    const fs::path src = ckdir / fs::path(url).filename();
    if (!fs::exists(src)) return false;
    fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
    if (cb) cb(fs::file_size(dest));
    return true;
  };
  const auto have_all = [&](const StemModelRow& r) {
    for (const auto& f : r.entry.files)
      if (!fs::exists(ckdir / fs::path(f.url).filename())) return false;
    return true;
  };
  const auto run_install = [&](const std::string& id) {
    svc.set_downloader(local);
    reset();
    CHECK(svc.install(id, true, error) == StemService::Result::Ok);
    const std::string state = wait_install(svc, m, cv, finished);
    if (state != "done") std::fprintf(stderr, "install %s: %s\n", id.c_str(), svc.install_state().error.c_str());
    CHECK(state == "done");
    const StemModelRow* row = find_row(svc.models(), id);
    CHECK(row && row->installed && row->usable);
    CHECK(svc.remove(id));
    CHECK(!find_row(svc.models(), id)->installed);
  };
  if (have_all(*single)) run_install("demucs-htdemucs");
  else std::printf("  (htdemucs checkpoint not downloaded: full install skipped)\n");
  // The four-checkpoint bag converts four 160 MB models: only with DH_STEMS_FULL_INSTALL=1.
  if (std::getenv("DH_STEMS_FULL_INSTALL") && have_all(*bag)) run_install("demucs-htdemucs-ft");

  fs::remove_all(data);
  std::printf("test_stems_service passed\n");
  return 0;
}
