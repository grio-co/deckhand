#pragma once
// Internal: on-disk log framing (see docs/SYNC.md, format appendix) and small file helpers.
#include <cstdint>
#include <string>
#include <vector>

#include "state.hpp"

namespace deckhand::sync::detail {

constexpr std::uint8_t kFormatVersion = 1;
constexpr std::uint8_t kFileLog = 1, kFileSnapshot = 2;
constexpr std::uint8_t kRecHeader = 0x01, kRecReg = 0x02, kRecEncrypted = 0x20, kRecTrailer = 0xFF;
constexpr std::uint8_t kFlagMayBeEncrypted = 1;

std::uint32_t crc32(const void* data, std::size_t n, std::uint32_t seed = 0);
// Non-cryptographic 128-bit content hash (two 64-bit lanes) as 32 hex digits, fed incrementally.
class Hash128 {
 public:
  void add(const void* data, std::size_t n);
  std::string hex() const;

 private:
  std::uint64_t a_ = 0xcbf29ce484222325ull, b_ = 0x9ae16a3b2f90404full;
};

struct RegRec {
  char kind = 'T';
  std::string eid, field;
  Stamp st;
  Val v;
};

struct FileHeader {
  std::string device_id, device_name;
  std::uint64_t seq = 0;
  std::int64_t created_ms = 0;
  std::uint8_t kind = kFileLog;
};

struct ParsedFile {
  bool header_ok = false;
  bool complete = false;      // trailer present and both checksums match
  bool unsupported = false;   // newer format version: skipped entirely
  FileHeader header;
  std::vector<RegRec> regs;   // valid records, even when the file is incomplete
  std::size_t unknown_records = 0, encrypted_records = 0;
  std::string problem;        // why it is incomplete, if it is
};

class FileWriter {
 public:
  FileWriter(const FileHeader& h);
  void add(const RegRec& r);
  std::size_t size() const { return buf_.size(); }
  std::size_t count() const { return count_; }
  std::string finish();  // appends the trailer

 private:
  void frame(std::uint8_t type, const std::string& payload);
  std::string buf_;
  std::size_t count_ = 0;
};

ParsedFile parse_file(const std::string& bytes);
// Header of a file without validating the rest (for listing devices).
bool parse_header(const std::string& prefix, FileHeader& out);

bool read_file(const std::string& path, std::string& out, std::size_t max_bytes = std::size_t(1) << 31);
// Write via temp name + fsync + rename (+ directory fsync). The only place with platform specifics.
bool write_file_atomic(const std::string& path, const std::string& bytes, std::string& error);

}  // namespace deckhand::sync::detail
