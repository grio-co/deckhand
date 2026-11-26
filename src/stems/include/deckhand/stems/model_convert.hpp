// Turns a downloaded PyTorch checkpoint into the external-data weights file that a committed ONNX skeleton expects.
// The layout file (tools/export_stems_skeleton.py) lists, per weight, its checkpoint key and its place in the file; the
// checkpoint is read with the hardened reader in checkpoint.hpp and nothing from it is ever executed.
#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace deckhand::stems {

// Writes <out_weights> (temp file + rename). Fails if a layout tensor is missing from the checkpoint or has another shape,
// or if the checkpoint holds tensors the layout does not know about (a different architecture or version).
bool convert_checkpoint(const std::filesystem::path& checkpoint, const std::filesystem::path& layout_json,
                        const std::filesystem::path& out_weights, const std::function<void(float)>& progress, std::string* error);

}  // namespace deckhand::stems
