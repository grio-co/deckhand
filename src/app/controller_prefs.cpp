#include "deckhand/app/controller_prefs.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace deckhand {

std::string controller_slug(const std::string& name) {
  std::string out;
  bool gap = false;
  for (const unsigned char c : name) {
    if (std::isalnum(c)) {
      if (gap && !out.empty()) out += '_';
      gap = false;
      out += static_cast<char>(std::tolower(c));
    } else {
      gap = true;
    }
  }
  if (out.size() > 40) out.resize(40);
  return out.empty() ? "controller" : out;
}

namespace {
std::string key(const std::string& name, const char* field) { return "controller." + controller_slug(name) + "." + field; }
double num(const std::string& s, double def) {
  if (s.empty()) return def;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  return end != s.c_str() && std::isfinite(v) ? v : def;
}
std::string text(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}
}  // namespace

ControllerPrefs load_controller_prefs(Library& lib, const std::string& name) {
  ControllerPrefs p;
  p.jog_scale = std::clamp(num(lib.setting(key(name, "jog_scale")), 1.0), 0.1, 8.0);
  const double tpr = num(lib.setting(key(name, "ticks_per_rev")), 0);
  p.ticks_per_rev = tpr >= 16 && tpr <= 100000 ? tpr : 0;
  const std::string pk = lib.setting(key(name, "pickup"));
  p.pickup = pk == "1" ? 1 : pk == "0" ? 0 : -1;
  p.leds = lib.setting(key(name, "leds")) != "0";
  p.mapping = lib.setting(key(name, "mapping"));
  const DeviceOptions x = DeviceOptions::from_text(lib.setting(key(name, "options")));  // older data has no such setting
  p.deck_pair = x.deck_pair;
  p.jog_vinyl = x.jog_vinyl;
  p.jog_finish_scratch = x.jog_finish_scratch;
  p.crossfader_curve = x.crossfader_curve;
  p.eq_kill = x.eq_kill;
  p.filter_snap = x.filter_snap;
  p.invert_fader = x.invert_fader;
  p.invert_crossfader = x.invert_crossfader;
  p.invert_tempo = x.invert_tempo;
  return p;
}

void save_controller_prefs(Library& lib, const std::string& name, const ControllerPrefs& in) {
  ControllerPrefs p = in;
  p.jog_scale = std::clamp(std::isfinite(p.jog_scale) ? p.jog_scale : 1.0, 0.1, 8.0);
  p.ticks_per_rev = std::isfinite(p.ticks_per_rev) && p.ticks_per_rev >= 16 && p.ticks_per_rev <= 100000 ? p.ticks_per_rev : 0;
  lib.set_setting(key(name, "jog_scale"), p.jog_scale == 1.0 ? "" : text(p.jog_scale));
  lib.set_setting(key(name, "ticks_per_rev"), p.ticks_per_rev > 0 ? text(p.ticks_per_rev) : "");
  lib.set_setting(key(name, "pickup"), p.pickup < 0 ? "" : (p.pickup ? "1" : "0"));
  lib.set_setting(key(name, "leds"), p.leds ? "" : "0");
  lib.set_setting(key(name, "mapping"), p.mapping);
  DeviceOptions x;  // the newer options travel as one self-describing text; only what differs from the default
  x.deck_pair = p.deck_pair;
  x.jog_vinyl = p.jog_vinyl;
  x.jog_finish_scratch = p.jog_finish_scratch;
  x.crossfader_curve = p.crossfader_curve;
  x.eq_kill = p.eq_kill;
  x.filter_snap = p.filter_snap;
  x.invert_fader = p.invert_fader;
  x.invert_crossfader = p.invert_crossfader;
  x.invert_tempo = p.invert_tempo;
  lib.set_setting(key(name, "options"), x.to_text());
}

DeviceOptions to_device_options(const ControllerPrefs& p) {
  DeviceOptions o;
  o.jog_scale = p.jog_scale;
  o.ticks_per_rev = p.ticks_per_rev;
  o.pickup = p.pickup;
  o.leds = p.leds;
  o.deck_pair = p.deck_pair;
  o.jog_vinyl = p.jog_vinyl;
  o.jog_finish_scratch = p.jog_finish_scratch;
  o.crossfader_curve = p.crossfader_curve;
  o.eq_kill = p.eq_kill;
  o.filter_snap = p.filter_snap;
  o.invert_fader = p.invert_fader;
  o.invert_crossfader = p.invert_crossfader;
  o.invert_tempo = p.invert_tempo;
  return o;
}

std::vector<std::string> seen_controllers(Library& lib) {
  std::vector<std::string> out;
  std::stringstream ss(lib.setting("controller.seen"));
  std::string item;
  while (std::getline(ss, item, '\n'))
    if (!item.empty()) out.push_back(item);
  return out;
}

void note_seen_controller(Library& lib, const std::string& name) {
  if (name.empty() || name.find('\n') != std::string::npos) return;
  std::vector<std::string> seen = seen_controllers(lib);
  if (std::find(seen.begin(), seen.end(), name) != seen.end()) return;
  seen.push_back(name);
  if (seen.size() > 24) seen.erase(seen.begin());
  std::string joined;
  for (const std::string& s : seen) joined += s + "\n";
  lib.set_setting("controller.seen", joined);
}

std::vector<std::string> preferred_mappings(Library& lib, const std::vector<std::string>& names) {
  std::vector<std::string> out;
  for (const std::string& n : names) {
    const std::string m = lib.setting(key(n, "mapping"));
    if (!m.empty() && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  }
  return out;
}

std::vector<MappingFileInfo> scan_mapping_files(const std::vector<std::string>& folders) {
  namespace fs = std::filesystem;
  std::vector<MappingFileInfo> out;
  for (const std::string& folder : folders) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) continue;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(folder, ec))
      if (e.is_regular_file(ec) && e.path().extension() == ".dhmap") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
      std::ifstream in(f, std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      Mapping m;
      std::string err;
      if (!in || !parse_mapping(buf.str(), m, err)) continue;
      if (m.name.empty()) m.name = f.stem().string();
      MappingFileInfo info;
      info.name = m.name;
      info.match = m.match;
      info.audio_match = m.audio_match;
      info.path = f.string();
      info.ticks_per_rev = m.ticks_per_rev;
      info.pickup_default = m.pickup_default;
      info.master_channel = m.master_channel;
      info.phones_channel = m.phones_channel;
      info.hid = m.hid;
      std::set<int> decks;
      std::set<std::string> modes;
      std::vector<std::string> alts;
      for (const InRule& r : m.in) {
        if (r.has_deck || r.deck > 0) decks.insert(r.deck + 1);
        else if (r.action != Action::None) decks.insert(1);
        if (r.action == Action::PadMode && r.mode >= 0) {
          modes.insert(pad_mode_name(r.mode));
          if (r.alt >= 0) {
            const std::string pair = std::string(pad_mode_name(r.mode)) + " / " + pad_mode_name(r.alt);
            if (std::find(alts.begin(), alts.end(), pair) == alts.end()) alts.push_back(pair);
          }
        }
      }
      info.decks.assign(decks.begin(), decks.end());
      info.pad_modes.assign(modes.begin(), modes.end());
      info.alt_modes = alts;
      const auto same = std::find_if(out.begin(), out.end(), [&](const MappingFileInfo& o) { return o.name == info.name; });
      if (same != out.end()) *same = std::move(info);  // a later folder (the user's) overrides the stock mapping
      else out.push_back(std::move(info));
    }
  }
  return out;
}

std::vector<const MappingFileInfo*> competing_mappings(const std::vector<MappingFileInfo>& files, const std::string& match) {
  std::vector<const MappingFileInfo*> out;
  if (match.empty()) return out;
  for (const MappingFileInfo& f : files) {
    if (f.match.empty()) continue;
    if (f.match.find(match) != std::string::npos || match.find(f.match) != std::string::npos) out.push_back(&f);
  }
  return out;
}

}  // namespace deckhand
