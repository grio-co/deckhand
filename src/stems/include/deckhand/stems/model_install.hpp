// Model catalogue and user-driven installation. DeckHand never ships, mirrors or re-hosts model weights: a model is
// downloaded onto the user's own machine, by the user's explicit action, straight from the upstream author's host,
// after the upstream licence statement has been shown and accepted. The catalogue entries are metadata only.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "deckhand/stems/stems.hpp"

namespace deckhand::stems {

struct CatalogueFile {
  std::string name, url, sha256;
  std::uint64_t size = 0;
};
struct CatalogueEntry {
  std::string id, name, kind, summary, quality, speed, upstream_url;
  std::vector<std::string> stems;
  std::vector<CatalogueFile> files;
  std::vector<std::string> requires_ids;  // models that must be installed first (recipes combine two installed models; they download nothing)
  // licence statement shown to the user before installing (verbatim-short) and where it comes from
  std::string licence_summary, licence_source, licence_link, weights_licence, code_licence, training_data, restriction;
  bool install_supported = false;  // false until the conversion step for this model exists
  std::string blocked_by;
  std::uint64_t total_bytes() const { std::uint64_t t = 0; for (auto& f : files) t += f.size; return t; }
};

// Reads resources/stem-models/*.json (invalid files are skipped and described in `problems`).
std::vector<CatalogueEntry> list_catalogue(const std::filesystem::path& catalogue_dir, std::vector<std::string>* problems = nullptr);
bool parse_catalogue_entry(const std::string& json, CatalogueEntry& out, std::string* error);

struct InstalledModel {
  std::string id, name, licence, licence_accepted_at;
  std::filesystem::path dir;
  std::uint64_t bytes = 0;
  bool usable = false;  // a valid .dhstems.json manifest is present
};

// Downloader: fetches https URL to `dest`, reporting bytes received; returns false on error or cancel.
using DownloadFn = std::function<bool(const std::string& url, const std::filesystem::path& dest, std::uint64_t expected_size,
                                     const std::function<void(std::uint64_t)>& bytes_cb, const std::atomic<bool>* cancel, std::string* error)>;
// The default downloader runs the operating system's own `curl` (present on macOS, Windows 10+ and mainstream Linux)
// with TLS verification and https-only redirects. It adds no library dependency.
DownloadFn default_downloader();

// Only these hosts may be downloaded from (checked before any request, whatever the catalogue says).
bool url_host_allowed(const std::string& url);

struct InstallResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
};

// Installs a catalogue entry into <data_dir>/models/<id>/: refuses unless accept_licence is true, downloads every file
// to a temp name, verifies its SHA-256 and size, then renames it into place. Conversion into a runnable model
// (writing the weights file for the model's ONNX skeleton) is the last step; entries whose conversion does not exist
// yet (install_supported == false) are refused before anything is downloaded.
// Hooks for tests and for the conversion step. `convert` runs after all files are verified (it writes the runnable
// model + manifest next to the downloads); when empty the download is only kept.
struct InstallHooks {
  DownloadFn download;  // empty = default_downloader()
  std::function<bool(const std::filesystem::path& model_dir, const CatalogueEntry&, std::string* error)> convert;
};
InstallResult install_model(const std::filesystem::path& data_dir, const CatalogueEntry& entry, bool accept_licence,
                            const std::function<void(float)>& progress, const std::atomic<bool>* cancel, const InstallHooks& hooks = {});
// Conversion hook for catalogue entries of kind "demucs" (one or four checkpoints), "roformer" (Mel-Band RoFormer vocals) and "recipe" (no files;
// needs the models named in `requires_ids` installed, writes the combining manifest). Original description for "demucs" (single model): converts the verified checkpoint with the
// committed skeleton (resources/stem-models/skeletons/htdemucs.*) into <models>/<id>/htdemucs.{onnx,weights} and writes the
// manifest <models>/<id>.dhstems.json. The downloaded checkpoint is deleted afterwards. Pass as InstallHooks::convert.
std::function<bool(const std::filesystem::path&, const CatalogueEntry&, std::string*)> demucs_converter(
    const std::filesystem::path& data_dir, const std::filesystem::path& skeleton_dir);
std::vector<InstalledModel> installed_models(const std::filesystem::path& data_dir);
bool remove_model(const std::filesystem::path& data_dir, const std::string& id);

}  // namespace deckhand::stems
