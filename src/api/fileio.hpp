#pragma once
// Internal: write a file atomically (temp name, flush to disk, rename over the target).
#include <string>

namespace deckhand::api {
// `owner_only`: readable and writable by the current user alone (mode 0600; on Windows a protected ACL that grants
// the owner and SYSTEM only). Otherwise the file gets the platform's default permissions (0644).
bool write_file_atomic(const std::string& path, const std::string& data, bool owner_only);
}  // namespace deckhand::api
