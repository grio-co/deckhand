#pragma once
// Named effect chain presets: one JSON file per name in <data dir>/fx-presets (the text fx_chain_save_full writes: the
// chain plus the state of every plug-in unit). Process-wide folder, set once by the application.
#include <string>
#include <vector>

namespace deckhand {

void chain_presets_set_dir(const std::string& dir);
std::string chain_presets_dir();
// A name as it is stored: trimmed, no path separators or control characters, at most 60 bytes. "" when nothing is left.
std::string chain_preset_clean_name(const std::string& name);
std::vector<std::string> chain_presets_list();  // sorted
bool chain_preset_save(const std::string& name, const std::string& json, std::string& error);
bool chain_preset_read(const std::string& name, std::string& json, std::string& error);
bool chain_preset_delete(const std::string& name, std::string& error);

}  // namespace deckhand
