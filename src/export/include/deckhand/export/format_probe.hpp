#pragma once
// Dependency-free audio header sniffer used by the export compatibility check (docs/EXPORT.md, "Compatibility
// check"). Reads only a bounded number of header bytes, never the whole file, and never throws or crashes on
// truncated or hostile input: anything it cannot make sense of comes back with recognized=false or truncated=true.
#include <cstdint>
#include <string>

namespace deckhand::usbexport {

enum class Container { Unknown, Wav, Rf64, Aiff, Aifc, Flac, Mp3, Mp4, Ogg, Asf };
enum class Codec {
  Unknown,
  Pcm,     // integer PCM
  Float,   // IEEE float PCM
  Mp3,     // MPEG audio, any layer (see FormatInfo::layer)
  Aac,
  Alac,
  Flac,
  Vorbis,
  Opus,
  Wma,
  Drm,     // copy-protected stream whose codec is not readable
  Other    // recognised container, codec not classified (ADPCM, A-law, ...); see codec_name
};

struct FormatInfo {
  bool recognized = false;  // container and codec identified
  bool truncated = false;   // the header is cut off or inconsistent (damaged file)
  Container container = Container::Unknown;
  Codec codec = Codec::Unknown;
  std::string codec_name;   // human-readable codec, e.g. "PCM", "IEEE float", "AAC", "fl32"
  int sample_rate = 0;
  int bit_depth = 0;        // bits per sample as stored (container size for PCM/float); 0 = not applicable/unknown
  int channels = 0;
  bool is_float = false;
  bool big_endian = false;  // PCM byte order (AIFF is big-endian, AIFC "sowt" and WAV are little-endian)
  int block_align = 0;      // PCM/float: bytes per frame
  int bitrate_kbps = 0;     // MP3 (first frame, or the average from a Xing/Info header); 0 = unknown
  bool vbr = false;
  int layer = 0;            // MP3: 1..3
  int mpeg_version = 0;     // MP3: 1, 2 (MPEG-2) or 25 (MPEG-2.5)
  bool drm = false;         // copy-protected (FairPlay-style) audio
  std::uint64_t file_size = 0;
  std::uint64_t data_offset = 0;  // PCM/float: first sample byte
  std::uint64_t data_bytes = 0;   // PCM/float: sample bytes actually present in the file
  std::string error;              // why recognized is false ("empty file", "unknown format", ...)
};

// Probes the file at `path`. Never throws.
FormatInfo probe_format(const std::string& path);

// One-line description, e.g. "32-bit float WAV, 44100 Hz, 2 ch".
std::string describe_format(const FormatInfo& f);

const char* container_name(Container c);

}  // namespace deckhand::usbexport
