// Export compatibility check: player-profile rule tables (data), the per-track report and the 24-bit WAV fix.
#include "deckhand/export/export_preflight.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "deckhand/export/export_check.hpp"

namespace deckhand::usbexport {
namespace fs = std::filesystem;

const char* verdict_name(Verdict v) {
  switch (v) {
    case Verdict::Ok: return "ok";
    case Verdict::Warning: return "warning";
    default: return "will_not_play";
  }
}
const char* rule_source_name(RuleSource s) { return s == RuleSource::Documented ? "documented" : "unverified"; }

// ---- the rule tables ----------------------------------------------------------------------------------------
// EDIT HERE. Order matters: the first matching rule of a profile wins. `Documented` = taken from the player
// manuals / the project's list of known-solid rules; `Unverified` = conservative guess. Check every rule against
// the manual of the player you care about; none of this was verified on hardware.

namespace {

const std::vector<Container> kPcmContainers = {Container::Wav, Container::Rf64, Container::Aiff, Container::Aifc};
const std::vector<int> kSafeRates = {44100, 48000, 88200, 96000};
const std::vector<int> kLossyRates = {44100, 48000};

FormatRule rule(const char* id, FormatMatch m, Verdict v, RuleSource s, const char* msg) {
  FormatRule r;
  r.id = id;
  r.match = std::move(m);
  r.verdict = v;
  r.source = s;
  r.message = msg;
  return r;
}

std::vector<FormatRule> club_rules(bool lossless_ok) {
  using V = Verdict;
  using S = RuleSource;
  std::vector<FormatRule> r;
  {
    FormatMatch m;
    m.containers = kPcmContainers;
    m.codecs = {Codec::Float};
    r.push_back(rule("pcm.float", m, V::WillNotPlay, S::Documented, "{kind}: club players cannot read this."));
  }
  {
    FormatMatch m;
    m.containers = kPcmContainers;
    m.codecs = {Codec::Pcm};
    m.bits_min = 32;
    r.push_back(rule("pcm.int32", m, V::WillNotPlay, S::Documented, "{kind}: club players cannot read this."));
  }
  {
    FormatMatch m;
    m.containers = kPcmContainers;
    m.codecs = {Codec::Other};
    r.push_back(rule("pcm.codec", m, V::WillNotPlay, S::Unverified,
                     "{kind}: compressed or unusual WAV/AIFF encodings are not on the list of formats club players read."));
  }
  {
    FormatMatch m;
    m.containers = {Container::Aifc};
    m.codecs = {Codec::Pcm};
    m.big_endian = 0;
    r.push_back(rule("pcm.little_endian_aiff", m, V::Warning, S::Unverified,
                     "{kind}: little-endian AIFF-C is unusual and some players may not read it. Test it on the player."));
  }
  {
    FormatMatch m;
    m.containers = kPcmContainers;
    m.codecs = {Codec::Pcm};
    m.bits_max = 15;
    r.push_back(rule("pcm.bits", m, V::Warning, S::Unverified,
                     "{kind}: only 16- and 24-bit WAV/AIFF are on the list of safe formats. Test it on the player."));
  }
  {
    FormatMatch m;
    m.containers = kPcmContainers;
    m.codecs = {Codec::Pcm};
    m.bits_min = 16;
    m.bits_max = 24;
    m.rates = kSafeRates;
    m.rates_exclude = true;
    r.push_back(rule("pcm.rate", m, V::Warning, S::Unverified,
                     "{kind}: only 44.1, 48, 88.2 and 96 kHz WAV/AIFF are on the list of safe sample rates. Test it on the player."));
  }
  {
    FormatMatch m;
    m.containers = kPcmContainers;
    m.codecs = {Codec::Pcm};
    m.bits_min = 16;
    m.bits_max = 24;
    r.push_back(rule("pcm.ok", m, V::Ok, S::Documented, "{kind}: fine."));
  }
  {
    FormatMatch m;
    m.codecs = {Codec::Mp3};
    m.layer_max = 2;
    r.push_back(rule("mp3.layer", m, V::Warning, S::Unverified,
                     "{kind}: MPEG layer I/II is rare and may not be read. Test it on the player."));
  }
  {
    FormatMatch m;
    m.codecs = {Codec::Mp3};
    m.rates = kLossyRates;
    m.rates_exclude = true;
    r.push_back(rule("mp3.rate", m, V::Warning, S::Unverified,
                     "{kind}: only 44.1 and 48 kHz MP3 are on the list of safe sample rates. Test it on the player."));
  }
  {
    FormatMatch m;
    m.codecs = {Codec::Mp3};
    r.push_back(rule("mp3.ok", m, V::Ok, S::Documented, "{kind}: fine."));
  }
  {
    FormatMatch m;
    m.codecs = {Codec::Aac};
    m.rates = kLossyRates;
    m.rates_exclude = true;
    r.push_back(rule("aac.rate", m, V::Warning, S::Unverified,
                     "{kind}: only 44.1 and 48 kHz AAC are on the list of safe sample rates. Test it on the player."));
  }
  {
    FormatMatch m;
    m.codecs = {Codec::Aac};
    r.push_back(rule("aac.ok", m, V::Ok, S::Documented, "{kind}: fine."));
  }
  for (const Codec c : {Codec::Flac, Codec::Alac}) {
    const bool flac = c == Codec::Flac;
    {
      FormatMatch m;
      m.codecs = {c};
      m.bits_min = 25;
      r.push_back(rule(flac ? "flac.depth" : "alac.depth", m, V::WillNotPlay, S::Unverified,
                       "{kind}: more than 24 bits is beyond what players read."));
    }
    {
      FormatMatch m;
      m.codecs = {c};
      m.rate_min = 96001;
      r.push_back(rule(flac ? "flac.rate" : "alac.rate", m, V::WillNotPlay, S::Unverified,
                       "{kind}: sample rates above 96 kHz are beyond what players read."));
    }
    FormatMatch m;
    m.codecs = {c};
    if (lossless_ok)
      r.push_back(rule(flac ? "flac.ok" : "alac.ok", m, V::Warning, S::Documented,
                       "{kind}: current-generation players read this, older players cannot."));
    else
      r.push_back(rule(flac ? "flac.ok" : "alac.ok", m, V::WillNotPlay, S::Documented,
                       "{kind}: older players cannot read this."));
  }
  {
    FormatMatch m;
    m.codecs = {Codec::Opus};
    r.push_back(rule("opus.unsupported", m, V::WillNotPlay, S::Documented, "{kind}: club players cannot read Opus."));
  }
  {
    FormatMatch m;
    m.codecs = {Codec::Vorbis};
    r.push_back(rule("vorbis.unsupported", m, V::WillNotPlay, S::Documented, "{kind}: club players cannot read Ogg Vorbis."));
  }
  {
    FormatMatch m;
    m.containers = {Container::Ogg};
    r.push_back(rule("ogg.unsupported", m, V::WillNotPlay, S::Documented, "{kind}: club players cannot read Ogg files."));
  }
  {
    FormatMatch m;
    m.codecs = {Codec::Wma};
    r.push_back(rule("wma.unsupported", m, V::WillNotPlay, S::Documented, "{kind}: club players cannot read WMA."));
  }
  r.push_back(rule("format.unsupported", FormatMatch{}, V::WillNotPlay, S::Unverified,
                   "{kind}: this format is not on the list of formats club players read."));
  return r;
}

std::vector<PlayerProfile> build_profiles() {
  std::vector<PlayerProfile> v;
  PlayerProfile cur;
  cur.id = "club";
  cur.name = "Club players (current generation)";
  cur.rules = club_rules(true);
  cur.pcm_rates = kSafeRates;
  v.push_back(cur);
  PlayerProfile old = cur;
  old.id = "club_legacy";
  old.name = "Club players (older generations)";
  old.rules = club_rules(false);
  v.push_back(old);
  return v;
}

bool contains(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

bool matches(const FormatMatch& m, const FormatInfo& f) {
  if (!m.containers.empty() && std::find(m.containers.begin(), m.containers.end(), f.container) == m.containers.end()) return false;
  if (!m.codecs.empty() && std::find(m.codecs.begin(), m.codecs.end(), f.codec) == m.codecs.end()) return false;
  if (m.is_float >= 0 && (m.is_float != 0) != f.is_float) return false;
  if (m.bits_min && f.bit_depth < m.bits_min) return false;
  if (m.bits_max && f.bit_depth > m.bits_max) return false;
  if (m.rate_min && f.sample_rate < m.rate_min) return false;
  if (m.rate_max && f.sample_rate > m.rate_max) return false;
  if (!m.rates.empty() && contains(m.rates, f.sample_rate) == m.rates_exclude) return false;
  if (m.layer_min && f.layer < m.layer_min) return false;
  if (m.layer_max && f.layer > m.layer_max) return false;
  if (m.big_endian >= 0 && (m.big_endian != 0) != f.big_endian) return false;
  return true;
}

std::string kind_of(const FormatInfo& f) {
  const std::string d = describe_format(f);
  const auto c = d.find(", ");
  return c == std::string::npos ? d : d.substr(0, c);
}

std::string replace_kind(std::string msg, const std::string& kind) {
  const auto p = msg.find("{kind}");
  if (p != std::string::npos) msg.replace(p, 6, kind);
  return msg;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string ext_of(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const auto slash = path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  return lower(path.substr(dot + 1));
}

std::string file_name_of(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void add(TrackPreflight& t, Verdict v, std::string id, std::string msg, RuleSource s, bool fixable = false) {
  PreflightIssue i;
  i.verdict = v;
  i.id = std::move(id);
  i.message = std::move(msg);
  i.source = s;
  i.fixable = fixable;
  if (static_cast<int>(v) > static_cast<int>(t.verdict)) t.verdict = v;
  t.issues.push_back(std::move(i));
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (const unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o + "\"";
}

Container expected_container(const std::string& ext) {
  if (ext == "wav") return Container::Wav;
  if (ext == "aif" || ext == "aiff") return Container::Aiff;
  if (ext == "flac") return Container::Flac;
  if (ext == "mp3") return Container::Mp3;
  if (ext == "m4a" || ext == "mp4" || ext == "m4p") return Container::Mp4;
  if (ext == "ogg" || ext == "opus" || ext == "oga") return Container::Ogg;
  if (ext == "wma") return Container::Asf;
  return Container::Unknown;
}

bool same_family(Container a, Container b) {
  auto fam = [](Container c) {
    if (c == Container::Rf64) return Container::Wav;
    if (c == Container::Aifc) return Container::Aiff;
    return c;
  };
  return fam(a) == fam(b);
}

}  // namespace

const std::vector<PlayerProfile>& player_profiles() {
  static const std::vector<PlayerProfile> p = build_profiles();
  return p;
}
const PlayerProfile* find_player_profile(const std::string& id) {
  for (const auto& p : player_profiles())
    if (p.id == id) return &p;
  return nullptr;
}
const PlayerProfile& default_player_profile() { return player_profiles().front(); }

// ---- conversion plan ----------------------------------------------------------------------------------------

bool plan_pcm24_conversion(const FormatInfo& f, const PlayerProfile& profile, ConversionPlan& plan, std::string* why) {
  auto no = [&](const std::string& s) {
    if (why) *why = s;
    return false;
  };
  if (!f.recognized || f.truncated) return no("the file header is damaged");
  if (std::find(kPcmContainers.begin(), kPcmContainers.end(), f.container) == kPcmContainers.end())
    return no("only WAV and AIFF files can be converted without a decoder");
  const bool ok_float = f.codec == Codec::Float && (f.bit_depth == 32 || f.bit_depth == 64);
  const bool ok_int32 = f.codec == Codec::Pcm && f.bit_depth == 32;
  if (!ok_float && !ok_int32) return no("only 32-bit integer and 32/64-bit float WAV/AIFF can be converted");
  if (f.channels < 1 || f.channels > 2) return no("only mono and stereo files can be converted");
  if (!contains(profile.pcm_rates, f.sample_rate))
    return no("the sample rate (" + std::to_string(f.sample_rate) + " Hz) would need resampling, which DeckHand does not do");
  const int bps = f.bit_depth / 8;
  if (f.block_align != f.channels * bps) return no("the file's frame size does not match its header");
  const std::uint64_t frames = f.data_bytes / static_cast<std::uint64_t>(f.block_align);
  if (frames == 0) return no("the file contains no audio data");
  const std::uint64_t out = 44 + frames * static_cast<std::uint64_t>(f.channels) * 3;
  if (out > 0xFFFFFFFFull) return no("the converted file would exceed 4 GB");
  plan.sample_rate = f.sample_rate;
  plan.channels = f.channels;
  plan.bytes_per_sample = bps;
  plan.is_float = f.codec == Codec::Float;
  plan.big_endian = f.big_endian;
  plan.frames = frames;
  plan.out_bytes = out;
  plan.bitrate_kbps = static_cast<int>((static_cast<std::uint64_t>(f.sample_rate) * 24 * f.channels + 500) / 1000);
  return true;
}

namespace {
void put_le16(std::vector<std::uint8_t>& b, unsigned v) {
  b.push_back(v & 0xFF);
  b.push_back((v >> 8) & 0xFF);
}
void put_le32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  put_le16(b, v & 0xFFFF);
  put_le16(b, v >> 16);
}

std::int32_t to_s24(const std::uint8_t* p, const ConversionPlan& pl) {
  constexpr std::int64_t kMax = 8388607, kMin = -8388608;
  std::uint64_t raw = 0;
  for (int i = 0; i < pl.bytes_per_sample; ++i) {
    const int idx = pl.big_endian ? i : pl.bytes_per_sample - 1 - i;  // most significant byte first
    raw = (raw << 8) | p[idx];
  }
  if (pl.is_float) {
    double x;
    if (pl.bytes_per_sample == 4) {
      const std::uint32_t u = static_cast<std::uint32_t>(raw);
      float fl;
      std::memcpy(&fl, &u, 4);
      x = fl;
    } else {
      std::memcpy(&x, &raw, 8);
    }
    if (!(x == x)) return 0;  // NaN
    const double v = std::floor(x * 8388608.0 + 0.5);
    return static_cast<std::int32_t>(std::clamp<double>(v, static_cast<double>(kMin), static_cast<double>(kMax)));
  }
  const std::int32_t s = static_cast<std::int32_t>(static_cast<std::uint32_t>(raw));
  const std::int64_t v = (static_cast<std::int64_t>(s) + 128) >> 8;  // round to nearest
  return static_cast<std::int32_t>(std::clamp<std::int64_t>(v, kMin, kMax));
}
}  // namespace

bool convert_to_pcm24_wav(const std::string& source, const FormatInfo& f, const ConversionPlan& plan,
                          const std::string& dest, std::string& error) {
  try {
    std::ifstream in(fs::path(source), std::ios::binary);
    if (!in) {
      error = "cannot open " + source;
      return false;
    }
    std::ofstream out(fs::path(dest), std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "cannot write " + dest;
      return false;
    }
    std::vector<std::uint8_t> h;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(plan.frames * plan.channels * 3);
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    put_le32(h, 36 + data_bytes);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_le32(h, 16);
    put_le16(h, 1);
    put_le16(h, static_cast<unsigned>(plan.channels));
    put_le32(h, static_cast<std::uint32_t>(plan.sample_rate));
    put_le32(h, static_cast<std::uint32_t>(plan.sample_rate * plan.channels * 3));
    put_le16(h, static_cast<unsigned>(plan.channels * 3));
    put_le16(h, 24);
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    put_le32(h, data_bytes);
    out.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));

    const std::size_t frame_in = static_cast<std::size_t>(f.block_align);
    constexpr std::size_t kChunkFrames = 4096;
    std::vector<std::uint8_t> ibuf(kChunkFrames * frame_in), obuf;
    in.seekg(static_cast<std::streamoff>(f.data_offset));
    std::uint64_t left = plan.frames;
    while (left > 0) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, kChunkFrames));
      in.read(reinterpret_cast<char*>(ibuf.data()), static_cast<std::streamsize>(n * frame_in));
      if (static_cast<std::size_t>(in.gcount()) != n * frame_in) {
        error = "source file ended early: " + source;
        return false;
      }
      obuf.clear();
      for (std::size_t i = 0; i < n * static_cast<std::size_t>(plan.channels); ++i) {
        const std::int32_t s = to_s24(&ibuf[i * static_cast<std::size_t>(plan.bytes_per_sample)], plan);
        obuf.push_back(static_cast<std::uint8_t>(s & 0xFF));
        obuf.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF));
        obuf.push_back(static_cast<std::uint8_t>((s >> 16) & 0xFF));
      }
      out.write(reinterpret_cast<const char*>(obuf.data()), static_cast<std::streamsize>(obuf.size()));
      left -= n;
    }
    out.flush();
    if (!out) {
      error = "cannot write " + dest + " (disk full or not writable?)";
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    error = std::string("conversion failed: ") + e.what();
    return false;
  }
}

// ---- the report ---------------------------------------------------------------------------------------------

PreflightReport preflight_export(const std::vector<ExportTrack>& tracks, const PlayerProfile& profile,
                                 const PreflightOptions& options) {
  PreflightReport rep;
  rep.profile_id = profile.id;
  for (const ExportTrack& et : tracks) {
    TrackPreflight t;
    t.track_id = et.id;
    t.path = et.source_path;
    try {
      if (!et.source_path.empty()) {
        std::error_code ec;
        const fs::path p = fs::path(et.source_path);
        const bool exists = fs::is_regular_file(p, ec);
        const std::uint64_t size = exists ? static_cast<std::uint64_t>(fs::file_size(p, ec)) : 0;
        if (!exists || ec) {
          add(t, Verdict::WillNotPlay, "source.missing", "The source file is missing or cannot be read.", RuleSource::Documented);
        } else if (size == 0) {
          add(t, Verdict::WillNotPlay, "source.empty", "The source file is empty (zero bytes).", RuleSource::Documented);
        } else {
          const std::string ext = ext_of(et.source_path);
          const std::string name = file_name_of(et.source_path);
          if (ext == "m4p")
            add(t, Verdict::WillNotPlay, "drm.protected", "Copy-protected audio (.m4p): club players cannot play it.", RuleSource::Documented);
          if (options.stick_is_fat32 && profile.max_file_bytes && size > profile.max_file_bytes)
            add(t, Verdict::WillNotPlay, "file.too_large",
                "The file is larger than 4 GB, which a FAT32 stick cannot hold. Format the stick as exFAT or shorten the file.",
                RuleSource::Documented);
          if (sanitize_fs_name(name, 255, "track") != name)
            add(t, Verdict::Warning, "name.changed",
                "The file name contains characters a FAT32/exFAT stick cannot store; it will be renamed on the stick.",
                RuleSource::Documented);
          const std::string rel = "Contents/" + sanitize_fs_name(et.artist, 60, "Unknown Artist") + "/" +
                                  sanitize_fs_name(et.album, 60, "Unknown Album") + "/" + sanitize_fs_name(name, 255, "track");
          if (profile.max_path_bytes && rel.size() + 1 > profile.max_path_bytes)
            add(t, Verdict::WillNotPlay, "path.too_long",
                "The path on the stick would be too long for the player's database (" + std::to_string(rel.size() + 1) +
                    " bytes). Shorten the artist, album or file name.",
                RuleSource::Documented);

          t.format = probe_format(et.source_path);
          t.format_text = describe_format(t.format);
          const FormatInfo& f = t.format;
          if (f.drm && ext != "m4p")
            add(t, Verdict::WillNotPlay, "drm.protected", "Copy-protected audio: club players cannot play it.", RuleSource::Documented);
          if (!f.recognized) {
            add(t, Verdict::WillNotPlay, f.truncated ? "file.damaged" : "file.unrecognized",
                f.truncated ? "The audio file's header is damaged or cut off (" + f.error + "); players are unlikely to load it."
                            : "This is not a recognizable audio file (" + f.error + ").",
                RuleSource::Unverified);
          } else {
            if (f.truncated)
              add(t, Verdict::Warning, "file.damaged", "The audio file looks cut off or damaged (incomplete header).", RuleSource::Unverified);
            const Container want = expected_container(ext);
            if (want != Container::Unknown && !same_family(want, f.container))
              add(t, Verdict::Warning, "ext.mismatch",
                  "The file is named \"." + ext + "\" but contains " + container_name(f.container) +
                      "; players choose the decoder from the file name, so it may not load.",
                  RuleSource::Unverified);
            const FormatRule* hit = nullptr;
            for (const FormatRule& r : profile.rules)
              if (matches(r.match, f)) {
                hit = &r;
                break;
              }
            if (hit && hit->verdict != Verdict::Ok) {
              std::string msg = replace_kind(hit->message, kind_of(f));
              bool fixable = false;
              if (hit->verdict == Verdict::WillNotPlay && (hit->id == "pcm.float" || hit->id == "pcm.int32")) {
                ConversionPlan plan;
                std::string why;
                if (plan_pcm24_conversion(f, profile, plan, &why)) {
                  fixable = true;
                  msg += " DeckHand can write a 24-bit WAV copy to the stick (your original file is not changed).";
                } else {
                  msg += " It cannot be fixed automatically: " + why + ". Convert it in an audio editor.";
                }
              }
              add(t, hit->verdict, hit->id, msg, hit->source, fixable);
              t.fixable = t.fixable || fixable;
            }
          }
        }
      }
    } catch (...) {
      add(t, Verdict::Warning, "check.failed", "The file could not be checked.", RuleSource::Unverified);
    }
    switch (t.verdict) {
      case Verdict::Ok: ++rep.ok; break;
      case Verdict::Warning: ++rep.warnings; break;
      default: ++rep.will_not_play; break;
    }
    if (t.verdict == Verdict::WillNotPlay && t.fixable) ++rep.fixable;
    rep.tracks.push_back(std::move(t));
  }
  return rep;
}

std::string PreflightReport::text() const {
  std::string s;
  for (const auto& t : tracks) {
    for (const auto& i : t.issues) {
      s += "track " + std::to_string(t.track_id) + " (" + t.path + "): ";
      s += i.verdict == Verdict::WillNotPlay ? "WILL NOT PLAY" : "warning";
      s += " [" + i.id + ", " + rule_source_name(i.source) + "] " + i.message + "\n";
    }
  }
  s += std::to_string(ok) + " ok, " + std::to_string(warnings) + " warnings, " + std::to_string(will_not_play) +
       " will not play (" + std::to_string(fixable) + " fixable by conversion), profile " + profile_id + "\n";
  return s;
}

std::string PreflightReport::to_json() const {
  std::string j = "{\"profile\":" + json_str(profile_id) + ",\"summary\":{\"ok\":" + std::to_string(ok) +
                  ",\"warnings\":" + std::to_string(warnings) + ",\"will_not_play\":" + std::to_string(will_not_play) +
                  ",\"fixable\":" + std::to_string(fixable) + ",\"all_ok\":" + (all_ok() ? "true" : "false") + "},\"tracks\":[";
  bool first = true;
  for (const auto& t : tracks) {
    if (!first) j += ",";
    first = false;
    j += "{\"track_id\":" + std::to_string(t.track_id) + ",\"path\":" + json_str(t.path) +
         ",\"verdict\":" + json_str(verdict_name(t.verdict)) + ",\"format\":" + json_str(t.format_text) +
         ",\"container\":" + json_str(container_name(t.format.container)) +
         ",\"codec\":" + json_str(t.format.codec_name) + ",\"sample_rate\":" + std::to_string(t.format.sample_rate) +
         ",\"bit_depth\":" + std::to_string(t.format.bit_depth) + ",\"channels\":" + std::to_string(t.format.channels) +
         ",\"is_float\":" + (t.format.is_float ? "true" : "false") + ",\"fixable\":" + (t.fixable ? "true" : "false") +
         ",\"converted\":" + (t.converted ? "true" : "false") + ",\"issues\":[";
    bool f2 = true;
    for (const auto& i : t.issues) {
      if (!f2) j += ",";
      f2 = false;
      j += "{\"id\":" + json_str(i.id) + ",\"verdict\":" + json_str(verdict_name(i.verdict)) +
           ",\"message\":" + json_str(i.message) + ",\"rule\":" + json_str(rule_source_name(i.source)) +
           ",\"fixable\":" + (i.fixable ? "true" : "false") + "}";
    }
    j += "]}";
  }
  return j + "]}";
}

std::size_t ExportResult::remaining_will_not_play() const {
  std::size_t n = 0;
  for (const auto& t : report.tracks)
    if (t.verdict == Verdict::WillNotPlay && !t.converted) ++n;
  return n;
}

}  // namespace deckhand::usbexport
