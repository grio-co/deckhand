// Model manifests: <name>.dhstems.json files in <data>/models/ describe an installed separation model, so a model can
// be added later (by the app or by the user) without code changes. See docs/STEMS.md for the format.
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/stems/stems.hpp"

namespace deckhand::stems {

struct ModelManifest {
  std::string id;          // model name + version, [A-Za-z0-9._-]; part of every cache key
  std::string name;        // display name (optional)
  std::string file;        // model file, relative to the manifest folder (at most one sub-folder, no "..")
  std::string licence;     // licence string of the weights (required, shown in the UI)
  std::string source_url;  // where the weights came from (required)
  std::string sha256;      // SHA-256 of the model file, lower-case hex (required, verified before loading)
  int sample_rate = 44100;
  std::size_t segment_frames = 0;
  int channels = 2;
  float overlap = 0.25f;
  std::string input_name, input_layout = "BCT";        // layout letters: B batch, C channel, T time (B optional)
  std::string output_name;                            // one tensor holding all stems (layout has S) ...
  std::vector<std::string> output_names;              // ... or one tensor per stem, in `stems` order (layout has no S)
  std::string output_layout = "BSCT";                 // letters B, S stem, C channel, T time
  std::array<std::string, kStemCount> stems{"vocals", "drums", "bass", "other"};  // model output order
  std::string pipeline;                               // "" = plain tensors as described above; "htdemucs-core" = Demucs core + C++ STFT
  std::string execution_provider = "cpu";             // "cpu" or "coreml" (CoreML where available, else falls back)
  std::filesystem::path dir;                          // folder the manifest was loaded from (not in the JSON)
};

bool parse_manifest(const std::string& json, ModelManifest& out, std::string* error);
bool load_manifest(const std::filesystem::path& file, ModelManifest& out, std::string* error);
// Valid manifests in a folder, sorted by id. Invalid ones are skipped and described in `problems` when given.
std::vector<ModelManifest> list_models(const std::filesystem::path& models_dir, std::vector<std::string>* problems = nullptr);
bool verify_model_file(const ModelManifest& m, std::string* error);  // file exists and matches the SHA-256

// Verifies the file, then opens it with the compiled-in backend. Fails with a clear message when none is built in.
std::unique_ptr<StemModel> open_model(const ModelManifest& m, int threads, std::string* error);

inline std::filesystem::path models_dir(const std::filesystem::path& data_dir) { return data_dir / "models"; }
inline std::filesystem::path cache_dir(const std::filesystem::path& data_dir) { return data_dir / "stems"; }

}  // namespace deckhand::stems
