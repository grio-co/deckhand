// Stems on the stick: .dhstems reader, WAV rendering, stem track + playlist expansion. See export_stems.hpp.
#include "deckhand/export/export_stems.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

#include "deckhand/core/waveform_bands.hpp"
#include "deckhand/export/export_preflight.hpp"
#include "stem_wav.hpp"

#if defined(_WIN32)
#define DH_FSEEK _fseeki64
#else
#define DH_FSEEK fseeko
#endif

namespace deckhand::usbexport {
namespace fs = std::filesystem;
using detail::Bytes;

// ---- mix names -----------------------------------------------------------------------------------------------

const char* stem_mix_label(unsigned m) {
  switch (m) {
    case kMixAcapella: return "Acapella";
    case kMixInstrumental: return "Instrumental";
    case kMixDrums: return "Drums";
    case kMixBass: return "Bass";
    case kMixOther: return "Other";
  }
  return "";
}

namespace {
constexpr unsigned kAllMixes[5] = {kMixAcapella, kMixInstrumental, kMixDrums, kMixBass, kMixOther};

std::string lower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

bool parse_stem_mixes(const std::string& text, unsigned& mixes, std::string& error) {
  mixes = 0;
  std::string tok;
  std::vector<std::string> toks;
  for (char c : lower_ascii(text)) {
    if (c == '+' || c == ',' || c == ' ' || c == ';') {
      if (!tok.empty()) toks.push_back(tok);
      tok.clear();
    } else {
      tok.push_back(c);
    }
  }
  if (!tok.empty()) toks.push_back(tok);
  for (const std::string& t : toks) {
    if (t == "off" || t == "none" || t == "0") continue;
    if (t == "acapella" || t == "vocals" || t == "acappella") mixes |= kMixAcapella;
    else if (t == "instrumental") mixes |= kMixInstrumental;
    else if (t == "drums") mixes |= kMixDrums;
    else if (t == "bass") mixes |= kMixBass;
    else if (t == "other") mixes |= kMixOther;
    else if (t == "all" || t == "four" || t == "4") mixes |= kMixesFourStems;
    else if (t == "everything") mixes |= kMixAcapella | kMixInstrumental | kMixDrums | kMixBass | kMixOther;
    else {
      error = "unknown stem mix '" + t + "' (use off, acapella, instrumental, drums, bass, other, all, everything)";
      mixes = 0;
      return false;
    }
  }
  return true;
}

std::string stem_mixes_text(unsigned mixes) {
  std::string s;
  for (unsigned m : kAllMixes)
    if (mixes & m) s += (s.empty() ? "" : "+") + lower_ascii(stem_mix_label(m));
  return s.empty() ? "off" : s;
}

std::uint64_t stem_wav_bytes(std::uint64_t frames, int bits) { return detail::wav_bytes(frames, bits); }

// ---- .dhstems reader -----------------------------------------------------------------------------------------

namespace detail {
namespace {
constexpr std::size_t kHeader = 128, kTrailer = 16;
constexpr int kStems = 4;
const char kMagic[8] = {'D', 'H', 'S', 'T', 'E', 'M', '0', '1'};
const char kEnd[8] = {'D', 'H', 'E', 'N', 'D', 0, 0, 1};
std::uint32_t g32(const std::uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
}  // namespace

StemFile::~StemFile() {
  if (f_) std::fclose(f_);
}

bool StemFile::open(const std::string& path, std::string& error) {
  if (f_) {
    std::fclose(f_);
    f_ = nullptr;
  }
  f_ = std::fopen(fs::path(path).string().c_str(), "rb");
  if (!f_) {
    error = "cannot open " + path;
    return false;
  }
  std::uint8_t h[kHeader];
  if (std::fread(h, 1, kHeader, f_) != kHeader || std::memcmp(h, kMagic, 8) != 0 || g32(h + 8) != 1 ||
      g32(h + 20) != kStems || g32(h + 24) != 2) {
    error = "not a stem cache file: " + path;
    return false;
  }
  bits_ = static_cast<int>(g32(h + 12));
  rate_ = static_cast<int>(g32(h + 16));
  const std::uint32_t sc = g32(h + 28);
  std::memcpy(&scale_, &sc, 4);
  frames_ = static_cast<std::uint64_t>(g32(h + 32)) | (static_cast<std::uint64_t>(g32(h + 36)) << 32);
  if ((bits_ != 16 && bits_ != 24) || rate_ <= 0 || !std::isfinite(scale_) || scale_ <= 0 || frames_ == 0 ||
      frames_ > (std::uint64_t{1} << 40)) {
    error = "stem cache file has an invalid header: " + path;
    return false;
  }
  const std::uint64_t want = kHeader + std::uint64_t{kStems} * 2 * frames_ * static_cast<std::uint64_t>(bits_ / 8) + kTrailer;
  std::uint8_t t[kTrailer];
  if (DH_FSEEK(f_, 0, SEEK_END) != 0) {
    error = "cannot read " + path;
    return false;
  }
  std::error_code ec;
  const std::uint64_t size = fs::file_size(fs::path(path), ec);
  if (ec || size != want || DH_FSEEK(f_, static_cast<long long>(want - kTrailer), SEEK_SET) != 0 ||
      std::fread(t, 1, kTrailer, f_) != kTrailer || std::memcmp(t, kEnd, 8) != 0) {
    error = "stem cache file is truncated or damaged: " + path;
    return false;
  }
  return true;
}

bool StemFile::read(int stem, int channel, std::uint64_t start, std::size_t n, float* out) {
  if (!f_ || stem < 0 || stem >= kStems || channel < 0 || channel > 1) return false;
  std::fill(out, out + n, 0.f);
  if (start >= frames_ || n == 0) return true;
  const std::size_t bps = static_cast<std::size_t>(bits_ / 8);
  const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(n, frames_ - start));
  std::vector<std::uint8_t> raw(take * bps);
  const std::uint64_t off = kHeader + (static_cast<std::uint64_t>(stem) * 2 + static_cast<std::uint64_t>(channel)) * frames_ * bps + start * bps;
  if (DH_FSEEK(f_, static_cast<long long>(off), SEEK_SET) != 0) return false;
  if (std::fread(raw.data(), 1, raw.size(), f_) != raw.size()) return false;
  for (std::size_t i = 0; i < take; ++i) {
    const std::uint8_t* p = &raw[i * bps];
    float v;
    if (bps == 2)
      v = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8))) / 32768.f;
    else
      v = (static_cast<std::int32_t>((static_cast<std::uint32_t>(p[0]) << 8) | (static_cast<std::uint32_t>(p[1]) << 16) |
                                     (static_cast<std::uint32_t>(p[2]) << 24)) >> 8) / 8388608.f;
    out[i] = v * scale_;
  }
  return true;
}

bool StemFile::read_mix(unsigned mask, std::uint64_t start, std::size_t n, std::vector<double>& l, std::vector<double>& r) {
  l.assign(n, 0.0);
  r.assign(n, 0.0);
  std::vector<float> tmp(n);
  for (int s = 0; s < kStems; ++s) {
    if (!(mask & (1u << s))) continue;
    if (!read(s, 0, start, n, tmp.data())) return false;
    for (std::size_t i = 0; i < n; ++i) l[i] += static_cast<double>(tmp[i]);
    if (!read(s, 1, start, n, tmp.data())) return false;
    for (std::size_t i = 0; i < n; ++i) r[i] += static_cast<double>(tmp[i]);
  }
  return true;
}

unsigned stem_mask_of(unsigned m) {
  switch (m) {
    case kMixAcapella: return 1u;                    // vocals
    case kMixInstrumental: return 2u | 4u | 8u;      // drums + bass + other
    case kMixDrums: return 2u;
    case kMixBass: return 4u;
    case kMixOther: return 8u;
  }
  return 0;
}

// ---- WAV -----------------------------------------------------------------------------------------------------

std::uint64_t wav_bytes(std::uint64_t frames, int bits) { return 44 + frames * 2 * static_cast<std::uint64_t>(bits / 8); }

Bytes wav_header(int rate, int bits, std::uint64_t frames) {
  const std::uint32_t data = static_cast<std::uint32_t>(frames * 2 * static_cast<std::uint64_t>(bits / 8));
  Bytes h;
  h.insert(h.end(), {'R', 'I', 'F', 'F'});
  put_le32(h, 36 + data);
  h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put_le32(h, 16);
  put_le16(h, 1);
  put_le16(h, 2);
  put_le32(h, static_cast<std::uint32_t>(rate));
  put_le32(h, static_cast<std::uint32_t>(rate * 2 * (bits / 8)));
  put_le16(h, static_cast<std::uint32_t>(2 * (bits / 8)));
  put_le16(h, static_cast<std::uint32_t>(bits));
  h.insert(h.end(), {'d', 'a', 't', 'a'});
  put_le32(h, data);
  return h;
}

namespace {
constexpr std::size_t kChunk = 32768;  // frames per pass

void quantise(const std::vector<double>& l, const std::vector<double>& r, int bits, Bytes& out) {
  out.clear();
  const double scale = bits == 24 ? 8388608.0 : 32768.0;
  const long long hi = bits == 24 ? 8388607 : 32767, lo = -hi - 1;
  for (std::size_t i = 0; i < l.size(); ++i) {
    for (const double v : {l[i], r[i]}) {
      const long long q = std::clamp(std::llround(v * scale), lo, hi);
      const std::uint32_t u = static_cast<std::uint32_t>(q);
      out.push_back(static_cast<std::uint8_t>(u));
      out.push_back(static_cast<std::uint8_t>(u >> 8));
      if (bits == 24) out.push_back(static_cast<std::uint8_t>(u >> 16));
    }
  }
}

bool open_for(const StemRender& r, StemFile& f, std::string& error) {
  if (!f.open(r.stems_file, error)) return false;
  if (f.frames() != r.frames || f.sample_rate() != r.sample_rate) {
    error = "stem cache file changed during the export: " + r.stems_file;
    return false;
  }
  return true;
}
}  // namespace

bool render_stem_wav(const StemRender& r, const std::string& dest, std::string& error) {
  StemFile f;
  if (!open_for(r, f, error)) return false;
  std::ofstream out(fs::path(dest), std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write " + dest;
    return false;
  }
  const Bytes h = wav_header(r.sample_rate, r.bits, r.frames);
  out.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
  std::vector<double> l, rr;
  Bytes pcm;
  for (std::uint64_t pos = 0; pos < r.frames; pos += kChunk) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, r.frames - pos));
    if (!f.read_mix(r.stem_mask, pos, n, l, rr)) {
      error = "cannot read stems from " + r.stems_file;
      return false;
    }
    quantise(l, rr, r.bits, pcm);
    out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
  }
  out.flush();
  if (!out) {
    error = "cannot write " + dest + " (disk full or not writable?)";
    return false;
  }
  return true;
}

bool stem_wav_matches(const StemRender& r, const std::string& existing) {
  std::error_code ec;
  if (fs::file_size(fs::path(existing), ec) != wav_bytes(r.frames, r.bits) || ec) return false;
  StemFile f;
  std::string e;
  if (!open_for(r, f, e)) return false;
  std::ifstream in(fs::path(existing), std::ios::binary);
  if (!in) return false;
  const Bytes h = wav_header(r.sample_rate, r.bits, r.frames);
  Bytes got(h.size());
  in.read(reinterpret_cast<char*>(got.data()), static_cast<std::streamsize>(got.size()));
  if (!in || got != h) return false;
  std::vector<double> l, rr;
  Bytes pcm, disk;
  for (std::uint64_t pos = 0; pos < r.frames; pos += kChunk) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, r.frames - pos));
    if (!f.read_mix(r.stem_mask, pos, n, l, rr)) return false;
    quantise(l, rr, r.bits, pcm);
    disk.resize(pcm.size());
    in.read(reinterpret_cast<char*>(disk.data()), static_cast<std::streamsize>(disk.size()));
    if (static_cast<std::size_t>(in.gcount()) != disk.size() || disk != pcm) return false;
  }
  return true;
}

bool looks_like_stem_wav(const std::string& path, int rate, int bits) {
  std::error_code ec;
  const std::uint64_t size = fs::file_size(fs::path(path), ec);
  if (ec || size < 44) return false;
  std::ifstream in(fs::path(path), std::ios::binary);
  Bytes h(44);
  in.read(reinterpret_cast<char*>(h.data()), 44);
  if (!in) return false;
  const Bytes want = wav_header(rate, bits, (size - 44) / (2 * static_cast<std::uint64_t>(bits / 8)));
  return size == wav_bytes((size - 44) / (2 * static_cast<std::uint64_t>(bits / 8)), bits) && h == want;
}

bool stem_waveform(const StemRender& r, std::vector<WaveSample>& out, std::string& error) {
  StemFile f;
  if (!open_for(r, f, error)) return false;
  out.clear();
  // The very code the application's build_waveform runs (core/waveform_bands.hpp), fed with the stem mix.
  out.resize(waveform::band_points(r.frames, r.sample_rate, 150.0));
  std::vector<double> l, rr;
  std::uint64_t buf_start = 0;
  std::size_t buf_n = 0;
  bool failed = false;
  waveform::band_waveform(
      r.frames, r.sample_rate, 150.0,
      [&](std::size_t frame) -> float {
        if (failed) return 0.0f;
        if (frame >= buf_start + buf_n) {
          buf_start = frame;
          buf_n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, r.frames - frame));
          if (!f.read_mix(r.stem_mask, buf_start, buf_n, l, rr)) {
            failed = true;
            return 0.0f;
          }
        }
        const std::size_t i = frame - static_cast<std::size_t>(buf_start);
        return 0.5f * (static_cast<float>(l[i]) + static_cast<float>(rr[i]));
      },
      [&](std::size_t p, std::uint8_t lo, std::uint8_t mi, std::uint8_t hi) { out[p] = {lo, mi, hi}; });
  if (failed) {
    error = "cannot read stems from " + r.stems_file;
    return false;
  }
  return true;
}

}  // namespace detail

// ---- expansion -----------------------------------------------------------------------------------------------

namespace {

constexpr std::int64_t kStemTrackBase = 1000000000;     // ids of stem tracks: base + parent * 8 + mix index
constexpr std::int64_t kStemPlaylistBase = 1500000000;  // ids of mirrored playlists: base + original id

struct Ids {
  std::set<int> used;
  int take(std::int64_t wanted, std::int64_t floor_) {
    std::int64_t c = wanted;
    if (c <= 0 || c > std::numeric_limits<int>::max() || used.count(static_cast<int>(c))) c = floor_;
    while (used.count(static_cast<int>(c))) {
      ++c;
      if (c > std::numeric_limits<int>::max()) return 0;
    }
    used.insert(static_cast<int>(c));
    return static_cast<int>(c);
  }
};

std::string parent_title(const ExportTrack& t) {
  if (!t.title.empty()) return t.title;
  const std::string stem = fs::path(t.source_path).stem().string();
  return stem.empty() ? "Track " + std::to_string(t.id) : stem;
}

}  // namespace

bool expand_stems(const std::vector<ExportTrack>& tracks, const std::vector<ExportPlaylist>& playlists,
                  const ExportOptions& options, StemExpansion& out, std::string& error) {
  out = StemExpansion{};
  const StemOptions& so = options.stems;
  out.playlists = playlists;
  if (!so.enabled()) return true;
  if (so.bits != 16 && so.bits != 24) {
    error = "export: stem bit depth must be 16 or 24";
    return false;
  }
  const PlayerProfile* profile = find_player_profile(options.player_profile);
  if (!profile) profile = &default_player_profile();

  std::map<int, const ExportPlaylist*> pl_by_id;
  for (const auto& p : playlists) pl_by_id[p.id] = &p;
  auto effective = [&](const ExportPlaylist& p) {
    int guard = 0;
    for (const ExportPlaylist* cur = &p; cur && guard < 70; ++guard) {
      const auto it = so.mixes_by_playlist.find(cur->id);
      if (it != so.mixes_by_playlist.end()) return it->second;
      const auto par = pl_by_id.find(cur->parent_id);
      cur = par == pl_by_id.end() ? nullptr : par->second;
    }
    return so.mixes;
  };
  std::map<int, unsigned> eff;  // playlist id -> mixes
  for (const auto& p : playlists)
    if (!p.is_folder) eff[p.id] = effective(p);

  // Which mixes each track asks for.
  std::map<int, unsigned> want;
  std::set<int> in_playlist;
  for (const auto& p : playlists) {
    if (p.is_folder) continue;
    for (int id : p.track_ids) {
      want[id] |= eff[p.id];
      in_playlist.insert(id);
    }
  }
  for (const auto& t : tracks)
    if (!in_playlist.count(t.id)) want[t.id] |= so.mixes;

  Ids ids;
  for (const auto& t : tracks) ids.used.insert(t.id);
  for (const auto& p : playlists) ids.used.insert(p.id);

  // stem track ids per parent and mix
  std::map<int, std::map<unsigned, int>> made;
  std::set<int> noted;
  for (const ExportTrack& t : tracks) {
    if (t.stem) continue;
    const unsigned mask = want[t.id];
    if (mask == 0) continue;
    auto note = [&](const std::string& why) {
      if (noted.insert(t.id).second) out.report.no_stems.push_back({t.id, parent_title(t), why});
    };
    const std::string path = so.lookup(t);
    if (path.empty()) {
      note("no stems cached");
      continue;
    }
    detail::StemFile sf;
    std::string why;
    if (!sf.open(path, why)) {
      note("stems file unreadable: " + why);
      continue;
    }
    const std::int64_t stem_ms = static_cast<std::int64_t>(static_cast<double>(sf.frames()) / sf.sample_rate() * 1000.0);
    if (t.duration_ms > 0 && std::llabs(stem_ms - t.duration_ms) > 100) {
      note("stems do not match the track length (" + std::to_string(stem_ms) + " ms vs " + std::to_string(t.duration_ms) + " ms)");
      continue;
    }
    if (std::find(profile->pcm_rates.begin(), profile->pcm_rates.end(), sf.sample_rate()) == profile->pcm_rates.end()) {
      note("stems sample rate " + std::to_string(sf.sample_rate()) + " Hz is not one the player profile accepts for WAV");
      continue;
    }
    if (profile->max_file_bytes && detail::wav_bytes(sf.frames(), so.bits) > profile->max_file_bytes) {
      note("the stem WAV would be larger than 4 GB");
      continue;
    }
    for (int k = 0; k < 5; ++k) {
      const unsigned mix = kAllMixes[k];
      if (!(mask & mix)) continue;
      ExportTrack s;
      s.id = ids.take(kStemTrackBase + static_cast<std::int64_t>(t.id) * 8 + k, kStemTrackBase);
      if (s.id == 0) {
        error = "export: no free track id for stems";
        return false;
      }
      auto render = std::make_shared<StemRender>();
      render->stems_file = path;
      render->stem_mask = detail::stem_mask_of(mix);
      render->bits = so.bits;
      render->frames = sf.frames();
      render->sample_rate = sf.sample_rate();
      render->parent_id = t.id;
      render->mix = mix;
      s.stem = render;
      s.title = parent_title(t) + " (" + stem_mix_label(mix) + ")";
      s.artist = t.artist;
      s.album = t.album;
      s.genre = t.genre;
      s.key = t.key;
      s.comment = t.comment;
      s.bpm = t.bpm;
      s.duration_ms = t.duration_ms;
      s.sample_rate = sf.sample_rate();
      s.bitrate = sf.sample_rate() * so.bits * 2 / 1000;
      s.year = t.year;
      s.rating = t.rating;
      s.track_number = t.track_number;
      s.color_id = t.color_id;
      s.beats = t.beats;  // sample-aligned with the parent by construction: same grid, cues and loops
      s.cues = t.cues;
      s.tags = t.tags;
      if (!detail::stem_waveform(*render, s.waveform, why)) {
        error = "export: " + why;
        return false;
      }
      made[t.id][mix] = s.id;
      out.report.bytes += detail::wav_bytes(sf.frames(), so.bits);
      out.tracks.push_back(std::move(s));
    }
  }
  out.report.tracks = out.tracks.size();
  if (out.tracks.empty()) return true;

  auto stems_for = [&](int parent, unsigned mixes, std::vector<int>& dst) {
    const auto it = made.find(parent);
    if (it == made.end()) return;
    for (unsigned mix : kAllMixes) {
      const auto m = it->second.find(mix);
      if (m != it->second.end() && (mixes & mix)) dst.push_back(m->second);
    }
  };

  if (so.placement == StemPlacement::Inline) {
    for (auto& p : out.playlists) {
      if (p.is_folder) continue;
      std::vector<int> ids_out;
      for (int id : p.track_ids) {
        ids_out.push_back(id);
        stems_for(id, eff[p.id], ids_out);
      }
      p.track_ids = std::move(ids_out);
    }
    return true;
  }

  // Mirror folder: a top-level folder holding a copy of the tree, each playlist listing only the stem tracks.
  std::map<int, std::vector<int>> mirror_tracks;  // original playlist id -> stem track ids
  std::set<int> keep;                             // original ids (playlists and folders) that appear in the mirror
  for (const auto& p : playlists) {
    if (p.is_folder) continue;
    std::vector<int> v;
    for (int id : p.track_ids) stems_for(id, eff[p.id], v);
    if (v.empty()) continue;
    mirror_tracks[p.id] = std::move(v);
    int guard = 0;
    for (int cur = p.id; cur != 0 && guard < 70; ++guard) {
      keep.insert(cur);
      const auto it = pl_by_id.find(cur);
      cur = it == pl_by_id.end() ? 0 : it->second->parent_id;
    }
  }
  std::vector<int> unfiled;
  if (!playlists.empty()) {
    for (const auto& kv : made)
      if (!in_playlist.count(kv.first)) stems_for(kv.first, so.mixes, unfiled);
  }
  if (keep.empty() && unfiled.empty()) return true;

  ExportPlaylist root;
  root.id = ids.take(kStemPlaylistBase, kStemPlaylistBase);
  root.name = so.folder_name.empty() ? "Stems" : so.folder_name;
  root.is_folder = true;
  if (root.id == 0) {
    error = "export: no free playlist id for the stems folder";
    return false;
  }
  out.playlists.push_back(root);
  std::map<int, int> new_id;
  for (const auto& p : playlists) {
    if (!keep.count(p.id)) continue;
    new_id[p.id] = ids.take(kStemPlaylistBase + p.id, kStemPlaylistBase);
    if (new_id[p.id] == 0) {
      error = "export: no free playlist id for the stems folder";
      return false;
    }
  }
  for (const auto& p : playlists) {
    if (!keep.count(p.id)) continue;
    ExportPlaylist m;
    m.id = new_id[p.id];
    m.name = p.name;
    m.is_folder = p.is_folder;
    m.parent_id = p.parent_id == 0 || !new_id.count(p.parent_id) ? root.id : new_id[p.parent_id];
    if (!p.is_folder) m.track_ids = mirror_tracks[p.id];
    out.playlists.push_back(std::move(m));
  }
  if (!unfiled.empty()) {
    ExportPlaylist m;
    m.id = ids.take(kStemPlaylistBase + 1, kStemPlaylistBase);
    m.name = "Unfiled";
    m.parent_id = root.id;
    m.track_ids = std::move(unfiled);
    out.playlists.push_back(std::move(m));
  }
  return true;
}

}  // namespace deckhand::usbexport
