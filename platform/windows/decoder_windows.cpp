// Audio file decoding and tag reading for Windows.
//
//  * Plain PCM WAV (integer or float, also WAVE_FORMAT_EXTENSIBLE) and PCM AIFF / AIFC are read by the small
//    parsers below: Media Foundation has no AIFF support, and reading uncompressed audio directly is faster and
//    does not depend on which Media Foundation codecs the Windows edition ships.
//  * Everything else (MP3, AAC/M4A, FLAC, compressed WAV, ...) goes through the Media Foundation Source Reader
//    (Windows 10 and later), asked for float output at the file's own sample rate.
// Output matches the other platforms: interleaved stereo float, mono duplicated, the first two channels of a
// multichannel file when Media Foundation cannot downmix.
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>
#include <propsys.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "deckhand/audio/track.hpp"

namespace deckhand {
namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const wchar_t* w, std::size_t len) {
  if (!w || len == 0) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), s.data(), n, nullptr, nullptr);
  return s;
}

std::string hr_text(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

// ---- uncompressed PCM files ---------------------------------------------------------------------
enum class Probe { NotPcm, Ok, Bad };

struct PcmInfo {
  int channels = 0;
  std::uint32_t rate = 0;
  int bits = 0;
  bool is_float = false;
  bool big_endian = false;
  bool offset_binary = false;  // WAV 8-bit is unsigned
  std::uint64_t data_offset = 0, data_bytes = 0;
  std::string title, artist, album;
  std::uint64_t frames() const {
    const std::uint64_t fb = static_cast<std::uint64_t>(channels) * (bits / 8);
    return fb ? data_bytes / fb : 0;
  }
};

std::uint32_t rd_le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint16_t rd_le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd_be32(const std::uint8_t* p) { return (static_cast<std::uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
std::uint16_t rd_be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

std::uint64_t file_size(std::istream& in) {
  in.clear();
  in.seekg(0, std::ios::end);
  const auto n = in.tellg();
  in.seekg(0);
  return n < 0 ? 0 : static_cast<std::uint64_t>(n);
}

bool read_at(std::istream& in, std::uint64_t at, void* buf, std::size_t n) {
  in.clear();
  in.seekg(static_cast<std::streamoff>(at));
  in.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
  return static_cast<std::size_t>(in.gcount()) == n;
}

std::string trim_text(std::string s) {
  while (!s.empty() && (s.back() == 0 || s.back() == ' ')) s.pop_back();
  return s;
}

Probe probe_wav(std::istream& in, std::uint64_t size, PcmInfo& out) {
  std::uint8_t h[12];
  if (!read_at(in, 0, h, sizeof h) || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return Probe::NotPcm;
  bool have_fmt = false, have_data = false;
  for (std::uint64_t at = 12; at + 8 <= size;) {
    std::uint8_t ch[8];
    if (!read_at(in, at, ch, sizeof ch)) break;
    const std::uint64_t len = rd_le32(ch + 4), body = at + 8;
    if (std::memcmp(ch, "fmt ", 4) == 0 && len >= 16) {
      std::uint8_t f[40] = {};
      if (!read_at(in, body, f, static_cast<std::size_t>(std::min<std::uint64_t>(len, sizeof f)))) return Probe::Bad;
      std::uint16_t tag = rd_le16(f);
      if (tag == 0xFFFE && len >= 26) tag = rd_le16(f + 24);  // extensible: the sub-format's first word
      if (tag != 1 && tag != 3) return Probe::NotPcm;         // ADPCM, MP3-in-WAV, ...: Media Foundation's job
      out.channels = rd_le16(f + 2);
      out.rate = rd_le32(f + 4);
      out.bits = rd_le16(f + 14);
      out.is_float = tag == 3;
      out.offset_binary = out.bits == 8;
      have_fmt = true;
    } else if (std::memcmp(ch, "data", 4) == 0) {
      out.data_offset = body;
      out.data_bytes = std::min<std::uint64_t>(len, size > body ? size - body : 0);  // streamed files carry 0 / 0xFFFFFFFF
      have_data = true;
    } else if (std::memcmp(ch, "LIST", 4) == 0 && len >= 4) {
      std::vector<std::uint8_t> l(static_cast<std::size_t>(std::min<std::uint64_t>(len, 1u << 16)));
      if (read_at(in, body, l.data(), l.size()) && std::memcmp(l.data(), "INFO", 4) == 0) {
        for (std::size_t p = 4; p + 8 <= l.size();) {
          const std::size_t n = rd_le32(l.data() + p + 4);
          if (p + 8 + n > l.size()) break;
          const std::string v = trim_text(std::string(reinterpret_cast<const char*>(l.data()) + p + 8, n));
          if (std::memcmp(l.data() + p, "INAM", 4) == 0) out.title = v;
          else if (std::memcmp(l.data() + p, "IART", 4) == 0) out.artist = v;
          else if (std::memcmp(l.data() + p, "IPRD", 4) == 0) out.album = v;
          p += 8 + n + (n & 1);
        }
      }
    }
    if (have_fmt && have_data && !out.title.empty()) break;
    at = body + len + (len & 1);
  }
  if (!have_fmt || !have_data || out.channels < 1 || out.rate < 1) return Probe::Bad;
  const bool ok = out.is_float ? (out.bits == 32 || out.bits == 64) : (out.bits == 8 || out.bits == 16 || out.bits == 24 || out.bits == 32);
  return ok ? Probe::Ok : Probe::Bad;
}

Probe probe_aiff(std::istream& in, std::uint64_t size, PcmInfo& out) {
  std::uint8_t h[12];
  if (!read_at(in, 0, h, sizeof h) || std::memcmp(h, "FORM", 4) != 0) return Probe::NotPcm;
  const bool aifc = std::memcmp(h + 8, "AIFC", 4) == 0;
  if (!aifc && std::memcmp(h + 8, "AIFF", 4) != 0) return Probe::NotPcm;
  out.big_endian = true;
  bool have_comm = false, have_data = false;
  for (std::uint64_t at = 12; at + 8 <= size;) {
    std::uint8_t ch[8];
    if (!read_at(in, at, ch, sizeof ch)) break;
    const std::uint64_t len = rd_be32(ch + 4), body = at + 8;
    if (std::memcmp(ch, "COMM", 4) == 0 && len >= 18) {
      std::uint8_t c[30] = {};
      if (!read_at(in, body, c, static_cast<std::size_t>(std::min<std::uint64_t>(len, sizeof c)))) return Probe::Bad;
      out.channels = rd_be16(c);
      out.bits = rd_be16(c + 6);
      const int exp = rd_be16(c + 8) & 0x7FFF;
      const std::uint64_t mant = (static_cast<std::uint64_t>(rd_be32(c + 10)) << 32) | rd_be32(c + 14);
      out.rate = static_cast<std::uint32_t>(std::llround(std::ldexp(static_cast<double>(mant), exp - 16383 - 63)));
      if (aifc) {
        if (len < 22) return Probe::Bad;
        if (std::memcmp(c + 18, "NONE", 4) == 0 || std::memcmp(c + 18, "twos", 4) == 0) {
        } else if (std::memcmp(c + 18, "sowt", 4) == 0) {
          out.big_endian = false;
        } else if (std::memcmp(c + 18, "fl32", 4) == 0 || std::memcmp(c + 18, "FL32", 4) == 0) {
          out.is_float = true;
          out.bits = 32;
        } else if (std::memcmp(c + 18, "fl64", 4) == 0 || std::memcmp(c + 18, "FL64", 4) == 0) {
          out.is_float = true;
          out.bits = 64;
        } else {
          return Probe::Bad;  // ima4, ulaw, ...
        }
      }
      have_comm = true;
    } else if (std::memcmp(ch, "SSND", 4) == 0 && len >= 8) {
      std::uint8_t s[8];
      if (!read_at(in, body, s, sizeof s)) return Probe::Bad;
      const std::uint64_t skip = rd_be32(s);
      out.data_offset = body + 8 + skip;
      const std::uint64_t avail = size > out.data_offset ? size - out.data_offset : 0;
      out.data_bytes = std::min<std::uint64_t>(len > 8 + skip ? len - 8 - skip : 0, avail);
      have_data = true;
    } else if ((std::memcmp(ch, "NAME", 4) == 0 || std::memcmp(ch, "AUTH", 4) == 0) && len < 4096) {
      std::string v(static_cast<std::size_t>(len), '\0');
      if (read_at(in, body, v.data(), v.size())) (std::memcmp(ch, "NAME", 4) == 0 ? out.title : out.artist) = trim_text(v);
    }
    at = body + len + (len & 1);
  }
  if (!have_comm || !have_data || out.channels < 1 || out.rate < 1) return Probe::Bad;
  const bool ok = out.is_float ? (out.bits == 32 || out.bits == 64) : (out.bits == 8 || out.bits == 16 || out.bits == 24 || out.bits == 32);
  return ok ? Probe::Ok : Probe::Bad;
}

Probe probe_pcm(const std::string& path, PcmInfo& out, std::ifstream& in) {
  in.open(std::filesystem::path(widen(path)), std::ios::binary);
  if (!in) return Probe::NotPcm;
  const std::uint64_t size = file_size(in);
  Probe p = probe_wav(in, size, out);
  if (p == Probe::NotPcm) p = probe_aiff(in, size, out);
  return p;
}

float sample_at(const std::uint8_t* p, const PcmInfo& f) {
  if (f.is_float) {
    if (f.bits == 32) {
      std::uint32_t u = f.big_endian ? rd_be32(p) : rd_le32(p);
      float v;
      std::memcpy(&v, &u, 4);
      return v;
    }
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = f.big_endian ? (u << 8) | p[i] : u | (static_cast<std::uint64_t>(p[i]) << (8 * i));
    double v;
    std::memcpy(&v, &u, 8);
    return static_cast<float>(v);
  }
  switch (f.bits) {
    case 8: return f.offset_binary ? (static_cast<int>(p[0]) - 128) / 128.0f : static_cast<std::int8_t>(p[0]) / 128.0f;
    case 16: return static_cast<std::int16_t>(f.big_endian ? rd_be16(p) : rd_le16(p)) / 32768.0f;
    case 24: {
      const std::int32_t v = f.big_endian ? (p[0] << 24) | (p[1] << 16) | (p[2] << 8) : (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
      return static_cast<float>(v / 2147483648.0);
    }
    default: return static_cast<float>(static_cast<std::int32_t>(f.big_endian ? rd_be32(p) : rd_le32(p)) / 2147483648.0);
  }
}

std::shared_ptr<TrackAudio> decode_pcm(std::ifstream& in, const PcmInfo& f, const std::string& path, std::string& error) {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = f.rate;
  const std::uint64_t frames = f.frames();
  if (frames == 0) {
    error = "file contains no audio: " + path;
    return nullptr;
  }
  if (frames > (std::uint64_t{1} << 31)) {
    error = "audio file too long: " + path;
    return nullptr;
  }
  audio->pcm.resize(static_cast<std::size_t>(frames) * 2);
  const std::size_t bps = static_cast<std::size_t>(f.bits / 8), fb = bps * static_cast<std::size_t>(f.channels);
  constexpr std::size_t kChunk = 16384;
  std::vector<std::uint8_t> buf(kChunk * fb);
  in.clear();
  in.seekg(static_cast<std::streamoff>(f.data_offset));
  std::uint64_t done = 0;
  while (done < frames) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, frames - done));
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want * fb));
    const std::size_t got = static_cast<std::size_t>(in.gcount()) / fb;
    for (std::size_t i = 0; i < got; ++i) {
      const std::uint8_t* fr = buf.data() + i * fb;
      const float l = sample_at(fr, f);
      const float r = f.channels > 1 ? sample_at(fr + bps, f) : l;
      float* out = audio->pcm.data() + (done + i) * 2;
      out[0] = l;
      out[1] = r;
    }
    done += got;
    if (got < want) break;  // truncated file: keep what is there
  }
  audio->frames = done;
  audio->pcm.resize(static_cast<std::size_t>(done) * 2);
  audio->pcm.shrink_to_fit();
  if (done == 0) {
    error = "file contains no audio: " + path;
    return nullptr;
  }
  build_waveform(*audio);
  return audio;
}

// ---- Media Foundation ---------------------------------------------------------------------------
template <class T>
class Com {
 public:
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { reset(); }
  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }
  T** put() {
    reset();
    return &p_;
  }
  T* operator->() const { return p_; }
  T* get() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

// COM and Media Foundation for the calling thread, for the life of one call.
class MfSession {
 public:
  MfSession() {
    const HRESULT c = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_ = SUCCEEDED(c);  // RPC_E_CHANGED_MODE: the thread is already in an apartment; carry on without owning it
    mf_ = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  }
  ~MfSession() {
    if (SUCCEEDED(mf_)) MFShutdown();
    if (com_) CoUninitialize();
  }
  MfSession(const MfSession&) = delete;
  MfSession& operator=(const MfSession&) = delete;
  HRESULT status() const { return mf_; }

 private:
  bool com_ = false;
  HRESULT mf_ = E_FAIL;
};

HRESULT open_reader(const std::string& path, Com<IMFSourceReader>& reader) {
  Com<IMFByteStream> stream;
  const std::wstring wpath = widen(path);
  HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, wpath.c_str(), stream.put());
  if (FAILED(hr)) return hr;
  Com<IMFAttributes> attrs;
  if (SUCCEEDED(stream->QueryInterface(IID_IMFAttributes, reinterpret_cast<void**>(attrs.put()))))
    attrs->SetString(MF_BYTESTREAM_ORIGIN_NAME, wpath.c_str());  // lets Media Foundation choose by extension too
  return MFCreateSourceReaderFromByteStream(stream.get(), nullptr, reader.put());
}

bool media_type_info(IMFSourceReader* r, UINT32& channels, UINT32& rate) {
  Com<IMFMediaType> t;
  if (FAILED(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, t.put()))) return false;
  channels = rate = 0;
  t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
  t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
  return channels > 0 && rate > 0;
}

// Float PCM at the file's own rate; two channels when Media Foundation can mix down to them.
bool configure_reader(IMFSourceReader* r, UINT32& channels, UINT32& rate, std::string& error) {
  r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
  Com<IMFMediaType> native;
  UINT32 nch = 0, nrate = 0;
  if (SUCCEEDED(r->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, native.put()))) {
    native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &nch);
    native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &nrate);
  }
  auto set_float = [&](UINT32 want_channels) {
    Com<IMFMediaType> t;
    if (FAILED(MFCreateMediaType(t.put()))) return false;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    if (nrate) t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, nrate);
    if (want_channels) t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, want_channels);
    return SUCCEEDED(r->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, t.get()));
  };
  bool ok = false;
  if (nch > 2) ok = set_float(2);
  if (!ok) ok = set_float(nch == 1 ? 1 : 0);
  if (!ok) ok = set_float(0);
  if (!ok || !media_type_info(r, channels, rate)) {
    error = "no decoder for this audio format";
    return false;
  }
  return true;
}

std::shared_ptr<TrackAudio> decode_mf(const std::string& path, std::string& error) {
  MfSession session;
  if (FAILED(session.status())) {
    error = "Media Foundation is not available (" + hr_text(session.status()) + "): " + path;
    return nullptr;
  }
  Com<IMFSourceReader> reader;
  HRESULT hr = open_reader(path, reader);
  if (FAILED(hr)) {
    error = "cannot open audio file (" + hr_text(hr) + "): " + path;
    return nullptr;
  }
  UINT32 channels = 0, rate = 0;
  if (!configure_reader(reader.get(), channels, rate, error)) {
    error += ": " + path;
    return nullptr;
  }
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = rate;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &pv)) &&
      pv.vt == VT_UI8) {
    const double est = static_cast<double>(pv.uhVal.QuadPart) / 1e7 * rate;
    if (est > 0 && est < 1e9) audio->pcm.reserve(static_cast<std::size_t>(est) * 2 + 8192);
  }
  PropVariantClear(&pv);

  bool failed = false;
  for (;;) {
    DWORD flags = 0;
    Com<IMFSample> sample;
    hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, sample.put());
    if (FAILED(hr)) {
      failed = audio->pcm.empty();
      break;  // an unreadable tail keeps what was decoded
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      UINT32 c = 0, r = 0;
      if (media_type_info(reader.get(), c, r)) channels = c;
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
      failed = failed || ((flags & MF_SOURCE_READERF_ERROR) && audio->pcm.empty());
      if (!sample) break;
    }
    if (!sample) continue;
    Com<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) continue;
    BYTE* data = nullptr;
    DWORD cur = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &cur))) continue;
    const std::size_t frames = cur / (sizeof(float) * channels);
    const std::size_t at = audio->pcm.size();
    audio->pcm.resize(at + frames * 2);
    float* out = audio->pcm.data() + at;
    const float* in = reinterpret_cast<const float*>(data);
    for (std::size_t i = 0; i < frames; ++i) {
      out[2 * i] = in[i * channels];
      out[2 * i + 1] = channels > 1 ? in[i * channels + 1] : in[i * channels];
    }
    buffer->Unlock();
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  audio->frames = audio->pcm.size() / 2;
  audio->pcm.shrink_to_fit();
  if (audio->frames == 0) {
    error = (failed ? "decode failed: " : "file contains no audio: ") + path;
    return nullptr;
  }
  build_waveform(*audio);
  return audio;
}

std::string prop_text(const PROPVARIANT& v) {
  if (v.vt == VT_LPWSTR && v.pwszVal) return narrow(v.pwszVal, wcslen(v.pwszVal));
  if (v.vt == VT_BSTR && v.bstrVal) return narrow(v.bstrVal, SysStringLen(v.bstrVal));
  if (v.vt == (VT_VECTOR | VT_LPWSTR) && v.calpwstr.cElems > 0 && v.calpwstr.pElems[0])
    return narrow(v.calpwstr.pElems[0], wcslen(v.calpwstr.pElems[0]));
  return {};
}

bool tags_mf(const std::string& path, FileTags& out) {
  MfSession session;
  if (FAILED(session.status())) return false;
  Com<IMFSourceReader> reader;
  if (FAILED(open_reader(path, reader))) return false;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &pv)) &&
      pv.vt == VT_UI8)
    out.duration_ms = static_cast<std::int64_t>(pv.uhVal.QuadPart / 10000);
  PropVariantClear(&pv);
  Com<IPropertyStore> store;
  if (SUCCEEDED(reader->GetServiceForStream(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PROPERTY_HANDLER_SERVICE,
                                            IID_IPropertyStore, reinterpret_cast<void**>(store.put())))) {
    // System.Title, System.Music.Artist, System.Author (fallback) and System.Music.AlbumTitle
    const PROPERTYKEY title = {{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
    const PROPERTYKEY author = {{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 4};
    const PROPERTYKEY artist = {{0x56A3372E, 0xCE9C, 0x11D2, {0x9F, 0x0E, 0x00, 0x60, 0x97, 0xC6, 0x86, 0xF6}}, 2};
    const PROPERTYKEY album = {{0x56A3372E, 0xCE9C, 0x11D2, {0x9F, 0x0E, 0x00, 0x60, 0x97, 0xC6, 0x86, 0xF6}}, 4};
    auto get = [&](const PROPERTYKEY& k) {
      PROPVARIANT v;
      PropVariantInit(&v);
      std::string s;
      if (SUCCEEDED(store->GetValue(k, &v))) s = prop_text(v);
      PropVariantClear(&v);
      return s;
    };
    out.title = get(title);
    out.artist = get(artist);
    if (out.artist.empty()) out.artist = get(author);
    out.album = get(album);
  }
  return true;
}

}  // namespace

std::shared_ptr<TrackAudio> decode_file(const std::string& path, std::string& error) {
  PcmInfo info;
  std::ifstream in;
  const Probe p = probe_pcm(path, info, in);
  if (p == Probe::Ok) return decode_pcm(in, info, path, error);
  in.close();
  if (p == Probe::Bad) {
    // A WAV or AIFF header the parsers do not accept (an unusual codec): let Media Foundation try.
  }
  return decode_mf(path, error);
}

bool read_file_tags(const std::string& path, FileTags& out) {
  PcmInfo info;
  std::ifstream in;
  const Probe p = probe_pcm(path, info, in);
  if (p == Probe::Ok) {
    out.title = info.title;
    out.artist = info.artist;
    out.album = info.album;
    out.duration_ms = static_cast<std::int64_t>(info.frames() * 1000 / info.rate);
    return true;
  }
  in.close();
  return tags_mf(path, out);
}

}  // namespace deckhand
