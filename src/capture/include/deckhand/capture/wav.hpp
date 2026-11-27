#pragma once
// WAV / RF64 writing and reading for recordings. Crash-safe: the header is refreshed at every flush, and a file whose
// header is behind (or whose tail was cut mid-frame) is repaired from its length. Beyond 4 GB the header becomes RF64.
#include <cstdint>
#include <cstdio>
#include <string>

namespace deckhand::capture {

enum class SampleFormat { Pcm16, Pcm24, Float32 };
constexpr std::uint32_t bytes_per_sample(SampleFormat f) { return f == SampleFormat::Pcm16 ? 2u : f == SampleFormat::Pcm24 ? 3u : 4u; }

struct WavFormat {
  std::uint32_t sample_rate = 44100;
  std::uint16_t channels = 2;
  SampleFormat format = SampleFormat::Pcm24;
  std::uint32_t block_align() const { return channels * bytes_per_sample(format); }
};

struct TrackTags {
  std::string artist, title, album, genre, year, comment;
  int track_number = 0, track_total = 0;
  bool empty() const {
    return artist.empty() && title.empty() && album.empty() && genre.empty() && year.empty() && comment.empty() && track_number == 0;
  }
};

constexpr std::uint64_t kRf64Threshold = 4000000000ull;
constexpr std::uint32_t kWavHeaderBytes = 80;  // RIFF/RF64 header + 28-byte JUNK/ds64 + fmt + data header

// Converts float samples to the file's sample bytes (clamped, rounded). `out` needs n * bytes_per_sample bytes.
void pack_samples(const float* in, std::size_t n, SampleFormat f, unsigned char* out);
void unpack_samples(const unsigned char* in, std::size_t n, SampleFormat f, float* out);

// The ID3v2.3 tag (title, artist, album, genre, year, comment, track "n/total") used by the WAV "id3 " and AIFF "ID3 " chunks.
std::string build_id3_tag(const TrackTags& t);

struct WavInfo {
  WavFormat fmt;
  std::uint64_t data_offset = 0, data_bytes = 0, frames = 0, file_bytes = 0, trailer_bytes = 0;
  bool rf64 = false;
  bool damaged = false;    // header sizes disagreed with the file (unclean stop); data_bytes comes from the file length
  bool our_layout = false; // the 80-byte layout WavWriter produces (can be resumed)
};

bool read_wav_info(const std::string& path, WavInfo& info, std::string& error);

class WavWriter {
 public:
  WavWriter() = default;
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;
  ~WavWriter() { close(); }

  bool create(const std::string& path, const WavFormat& fmt, std::string& error, std::uint64_t rf64_threshold = kRf64Threshold);
  // Reopens an existing file written by WavWriter (clean or not): repairs the header, drops a partial frame and any tag
  // trailer, then appends. The format comes from the file.
  bool open_append(const std::string& path, std::string& error, std::uint64_t rf64_threshold = kRf64Threshold);
  bool write(const float* interleaved, std::uint64_t frames);
  bool write_raw(const unsigned char* bytes, std::uint64_t frames);  // already packed in the file's format
  bool flush();                                                      // refreshes the header, flushes stdio
  bool close(const TrackTags* tags = nullptr);                       // final header; tags go after the data
  bool is_open() const { return file_ != nullptr; }
  std::uint64_t frames() const { return frames_; }
  const WavFormat& format() const { return fmt_; }
  bool rf64() const { return frames_ * fmt_.block_align() > threshold_; }

 private:
  bool write_header(std::uint64_t trailer_bytes);
  std::FILE* file_ = nullptr;
  WavFormat fmt_;
  std::uint64_t frames_ = 0, threshold_ = kRf64Threshold;
  bool failed_ = false;
};

// Rewrites the header of an unclean recording from its file length. Returns false if the layout is not ours.
struct RepairResult {
  bool ok = false;
  bool was_damaged = false;
  std::uint64_t frames = 0, dropped_bytes = 0;
  std::string error;
};
RepairResult repair_wav(const std::string& path);

class WavReader {
 public:
  WavReader() = default;
  WavReader(const WavReader&) = delete;
  WavReader& operator=(const WavReader&) = delete;
  ~WavReader() { close(); }
  bool open(const std::string& path, std::string& error);
  void close();
  const WavInfo& info() const { return info_; }
  // Reads up to `count` frames from `start` as float; returns frames read.
  std::uint64_t read(std::uint64_t start, std::uint64_t count, float* out);

 private:
  std::FILE* file_ = nullptr;
  WavInfo info_;
  std::string buf_;
};

int seek64(std::FILE* f, std::uint64_t pos);
std::uint64_t file_size(const std::string& path);

}  // namespace deckhand::capture
