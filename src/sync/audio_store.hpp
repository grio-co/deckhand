#pragma once
// Internal: the content-addressed audio store inside the sync folder (docs/SYNC.md, "Audio files").
//   <folder>/DeckHandSync/audio/<first two hex digits>/<hash>.dhau
// Objects are written under a temporary name and renamed, so a reader sees a whole object or none. A file is verified by
// its content hash when it is copied out. With a key, the object is AES-256-GCM encrypted in 1 MiB chunks.
#include <cstdint>
#include <filesystem>
#include <string>

#include "format.hpp"

namespace deckhand::sync::detail {

constexpr std::size_t kAudioChunk = 1u << 20;

// 128-bit content hash (32 hex digits) and size of a whole file; "" when unreadable.
std::string hash_whole_file(const std::filesystem::path& path, std::uint64_t& size);

std::filesystem::path audio_object_path(const std::filesystem::path& store, const std::string& hash);

enum class PutResult { Ok, SourceChanged, Failed };
// Copies `src` into the store as the object for `hash` (the caller computed it). Re-hashes while copying and drops the
// result when the source changed meanwhile. `sealer` empty: plain object.
PutResult audio_put(const std::filesystem::path& store, const std::filesystem::path& src, const std::string& hash,
                    std::uint64_t size, const Sealer* sealer, const std::string& tag, std::string& error);

enum class GetResult { Ok, Missing, Corrupt, Locked, PlainRefused };
// Verifies object `obj` against hash/size and writes its content to `dest` (a temporary file the caller then renames).
// `require_encrypted`: a plain object is refused. `key_for` supplies keys by salt.
GetResult audio_get(const std::filesystem::path& obj, const std::string& hash, std::uint64_t size, const KeyFor& key_for,
                    bool require_encrypted, const std::filesystem::path& dest, std::string& error);

// Size of a whole object file on disk (0 when absent).
std::uint64_t audio_object_size(const std::filesystem::path& obj);

}  // namespace deckhand::sync::detail
