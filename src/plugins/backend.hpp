// Private: what a format backend provides. plugin.cpp dispatches on PluginInfo::format.
#pragma once
#if defined(__APPLE__)
#include <TargetConditionals.h>
#else
#define TARGET_OS_OSX 0
#endif
#include "deckhand/plugins/plugin.hpp"

#if defined(__APPLE__) && TARGET_OS_OSX && !defined(DECKHAND_PLUGINS_DISABLED)
#define DH_PLUGINS_AU 1
#else
#define DH_PLUGINS_AU 0
#endif

namespace deckhand::plugins {
std::vector<PluginInfo> au_scan();
std::string au_fingerprint();
std::unique_ptr<PluginInstance> au_open(const PluginInfo& info, double sr, int max_block, std::string* err);
std::string fourcc_text(std::uint32_t code);
}  // namespace deckhand::plugins
