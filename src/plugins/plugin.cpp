#include <cstdio>

#include "backend.hpp"

namespace deckhand::plugins {

std::string fourcc_text(std::uint32_t c) {
  std::string s(4, '?');
  for (int i = 0; i < 4; ++i) {
    const unsigned char ch = (unsigned char)((c >> (24 - 8 * i)) & 0xff);
    s[(std::size_t)i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
  }
  return s;
}

bool hosting_supported() { return DH_PLUGINS_AU != 0; }

std::vector<PluginInfo> scan_installed() {
  std::vector<PluginInfo> all;
#if DH_PLUGINS_AU
  all = au_scan();
#endif
  return all;  // a VST3 scan would append here
}

std::string scan_fingerprint() {
#if DH_PLUGINS_AU
  return au_fingerprint();
#else
  return "none";
#endif
}

std::unique_ptr<PluginInstance> open_plugin(const PluginInfo& info, double sr, int max_block, std::string* err) {
  if (sr < 8000 || sr > 384000 || max_block < 16 || max_block > 16384) {
    if (err) *err = "bad sample rate or block size";
    return nullptr;
  }
#if DH_PLUGINS_AU
  if (info.format == Format::AudioUnit) return au_open(info, sr, max_block, err);
#endif
  if (err) *err = "plug-in format not supported on this platform";
  (void)info;
  return nullptr;
}

}  // namespace deckhand::plugins
