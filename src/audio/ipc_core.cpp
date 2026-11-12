// Platform-neutral parts of the engine IPC: shared-segment layout checks, track blocks (build and validate),
// command validation and socket framing. Everything that crosses the process boundary is validated here.
#include <algorithm>
#include <new>

#include "deckhand/audio/ipc.hpp"

namespace deckhand::ipc {

namespace {
constexpr std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

bool fail(std::string& error, const char* msg) {
  error = msg;
  return false;
}

bool finite_in(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

void fingerprint(std::uint32_t (&fp)[10]) {
  fp[0] = sizeof(DeckView);
  fp[1] = sizeof(MixerView);
  fp[2] = sizeof(SamplerView);
  fp[3] = sizeof(RecInfo);
  fp[4] = sizeof(DeviceInfo);
  fp[5] = sizeof(HubInfo);
  fp[6] = sizeof(DeckMeta);
  fp[7] = sizeof(WireCommand) * 100 + sizeof(WireEvent);
  fp[8] = static_cast<std::uint32_t>(sizeof(Shared));
  fp[9] = static_cast<std::uint32_t>(Op::Count) * 1000 + kMaxDecks * 100 + kSamplerSlots;
}
}  // namespace

// ---- main segment ---------------------------------------------------------------------------------
void init_shared(Shared& s) {
  Shared* p = new (&s) Shared();
  p->hdr.magic = kMainMagic;
  p->hdr.version = kProtocolVersion;
  p->hdr.header_size = sizeof(Header);
  p->hdr.total_size = sizeof(Shared);
  fingerprint(p->hdr.fingerprint);
  p->cmd.reset();
}

bool check_shared(const void* base, std::size_t size, std::string& error) {
  if (!base || size < sizeof(Shared)) return fail(error, "engine segment too small");
  const auto* s = static_cast<const Shared*>(base);
  if (s->hdr.magic != kMainMagic) return fail(error, "not an engine segment");
  if (s->hdr.version != kProtocolVersion) return fail(error, "engine protocol version differs");
  if (s->hdr.header_size != sizeof(Header) || s->hdr.total_size != sizeof(Shared)) return fail(error, "engine layout differs");
  std::uint32_t fp[10];
  fingerprint(fp);
  for (int i = 0; i < 10; ++i)
    if (s->hdr.fingerprint[i] != fp[i]) return fail(error, "engine was built from different sources");
  return true;
}

// ---- track blocks ---------------------------------------------------------------------------------
bool parse_block(const void* base, std::size_t size, BlockKind kind, ParsedBlock& out, std::string& error, bool scan_pcm) {
  if (!base || size < sizeof(BlockHeader)) return fail(error, "block too small");
  BlockHeader h;  // one copy; the checks below use only this copy
  std::memcpy(&h, base, sizeof h);
  if (h.magic != kBlockMagic || h.version != kBlockVersion) return fail(error, "bad block header");
  if (h.sealed != kBlockSealed) return fail(error, "block not sealed");
  if (h.kind != static_cast<std::uint32_t>(kind)) return fail(error, "wrong block kind");
  if (h.total_size < sizeof(BlockHeader) || h.total_size > size || h.total_size > kMaxBlockBytes)
    return fail(error, "bad block size");
  const std::uint64_t total = h.total_size;
  if (h.sample_rate < 8000 || h.sample_rate > 384000) return fail(error, "bad sample rate");
  if (h.frames == 0 || h.frames > kMaxBlockFrames) return fail(error, "bad frame count");
  const std::uint64_t pcm_bytes = h.frames * 2 * sizeof(float);
  if (h.pcm_offset % 8 != 0 || h.pcm_offset < sizeof(BlockHeader) || h.pcm_offset > total || pcm_bytes > total - h.pcm_offset)
    return fail(error, "bad audio range");
  if (h.grid_count > kMaxGridSections || h.memcue_count > kMaxMemoryCues) return fail(error, "too many grid or cue entries");
  const std::uint64_t grid_bytes = static_cast<std::uint64_t>(h.grid_count) * sizeof(WireGrid);
  const std::uint64_t cue_bytes = static_cast<std::uint64_t>(h.memcue_count) * sizeof(double);
  if (h.grid_count && (h.grid_offset % 8 != 0 || h.grid_offset < sizeof(BlockHeader) || h.grid_offset > total || grid_bytes > total - h.grid_offset))
    return fail(error, "bad grid range");
  if (h.memcue_count && (h.memcue_offset % 8 != 0 || h.memcue_offset < sizeof(BlockHeader) || h.memcue_offset > total || cue_bytes > total - h.memcue_offset))
    return fail(error, "bad cue range");
  if (h.path_len > kMaxPath) return fail(error, "bad path length");
  if (!finite_in(h.bpm, 0, 10000) || !finite_in(h.cue_point, -1e12, 1e12)) return fail(error, "bad tempo or cue");
  if (!std::isfinite(h.gain) || h.gain < 0 || h.gain > 16) return fail(error, "bad gain");
  for (int i = 0; i < kHotCues; ++i)
    if (!finite_in(h.hot_cue[i], -1e12, 1e12) || !finite_in(h.hot_loop_end[i], -1e12, 1e12)) return fail(error, "bad hot cue");

  const auto* bytes = static_cast<const std::uint8_t*>(base);
  std::vector<GridSection> grid;
  grid.reserve(h.grid_count);
  for (std::uint32_t i = 0; i < h.grid_count; ++i) {
    WireGrid g;
    std::memcpy(&g, bytes + h.grid_offset + static_cast<std::uint64_t>(i) * sizeof g, sizeof g);
    if (!finite_in(g.first_beat_frame, -1e12, 1e12) || !(std::isfinite(g.bpm) && g.bpm > 0 && g.bpm <= 10000) ||
        g.bar_beat < 1 || g.bar_beat > 32)
      return fail(error, "bad grid section");
    grid.push_back({g.first_beat_frame, g.bpm, g.bar_beat});
  }
  std::stable_sort(grid.begin(), grid.end(),
                   [](const GridSection& a, const GridSection& b) { return a.first_beat_frame < b.first_beat_frame; });
  std::vector<double> cues(h.memcue_count);
  if (h.memcue_count) std::memcpy(cues.data(), bytes + h.memcue_offset, static_cast<std::size_t>(cue_bytes));
  for (const double c : cues)
    if (!finite_in(c, -1e12, 1e12)) return fail(error, "bad memory cue");
  std::sort(cues.begin(), cues.end());

  const auto* pcm = reinterpret_cast<const float*>(bytes + h.pcm_offset);
  if (scan_pcm) {
    const auto* words = reinterpret_cast<const std::uint32_t*>(pcm);
    const std::uint64_t n = h.frames * 2;
    for (std::uint64_t i = 0; i < n; ++i)
      if ((words[i] & 0x7F800000u) == 0x7F800000u) return fail(error, "audio contains NaN or infinity");
  }
  out.h = h;
  out.pcm = pcm;
  out.grid = std::move(grid);
  out.memory_cues = std::move(cues);
  out.path.assign(h.path, h.path_len);
  return true;
}

namespace {
struct Layout {
  std::size_t pcm = 0, grid = 0, cues = 0, total = 0;
};
Layout layout_for(std::uint64_t frames, std::size_t grid, std::size_t cues) {
  Layout l;
  std::size_t at = align_up(sizeof(BlockHeader), 64);
  l.pcm = at;
  at = align_up(at + static_cast<std::size_t>(frames) * 2 * sizeof(float), 64);
  l.grid = at;
  at = align_up(at + grid * sizeof(WireGrid), 64);
  l.cues = at;
  at = align_up(at + cues * sizeof(double), 64);
  l.total = at;
  return l;
}

bool build_block(const std::string& name, BlockKind kind, int slot, std::uint64_t generation, const TrackAudio& audio,
                 const std::vector<GridSection>& grid, const std::vector<double>& cues, const BlockHeader& proto,
                 const std::string& path, std::string& error) {
  if (!audio.data() || audio.frames == 0 || audio.frames > kMaxBlockFrames) return fail(error, "nothing to share");
  if (!audio.ext_pcm && audio.pcm.size() < audio.frames * 2) return fail(error, "audio shorter than its frame count");
  const Layout l = layout_for(audio.frames, grid.size(), cues.size());
  Shm shm;
  if (!Shm::create(name, l.total, shm, error, /*replace=*/false)) return false;
  auto* base = static_cast<std::uint8_t*>(shm.data());
  BlockHeader h = proto;
  h.magic = kBlockMagic;
  h.version = kBlockVersion;
  h.kind = static_cast<std::uint32_t>(kind);
  h.total_size = l.total;
  h.generation = generation;
  h.slot = static_cast<std::uint32_t>(slot);
  h.sample_rate = audio.sample_rate;
  h.frames = audio.frames;
  h.pcm_offset = l.pcm;
  h.grid_offset = l.grid;
  h.memcue_offset = l.cues;
  h.grid_count = static_cast<std::uint32_t>(grid.size());
  h.memcue_count = static_cast<std::uint32_t>(cues.size());
  h.path_len = static_cast<std::uint32_t>(std::min<std::size_t>(path.size(), kMaxPath));
  std::memset(h.path, 0, sizeof h.path);
  std::memcpy(h.path, path.data(), h.path_len);
  h.sealed = 0;
  std::memcpy(base, &h, sizeof h);
  std::memcpy(base + l.pcm, audio.data(), static_cast<std::size_t>(audio.frames) * 2 * sizeof(float));
  for (std::size_t i = 0; i < grid.size(); ++i) {
    WireGrid g;
    g.first_beat_frame = grid[i].first_beat_frame;
    g.bpm = grid[i].bpm;
    g.bar_beat = grid[i].bar_beat;
    std::memcpy(base + l.grid + i * sizeof g, &g, sizeof g);
  }
  if (!cues.empty()) std::memcpy(base + l.cues, cues.data(), cues.size() * sizeof(double));
  __atomic_store_n(&reinterpret_cast<BlockHeader*>(base)->sealed, kBlockSealed, __ATOMIC_RELEASE);
  return true;  // ~Shm unmaps; the named object stays for the engine
}
}  // namespace

bool create_track_block(const std::string& name, int slot, std::uint64_t generation, const LoadedTrack& t, std::string& error) {
  if (!t.audio) return fail(error, "track has no audio");
  BlockHeader p;
  p.track_id = t.track_id;
  p.bpm = t.bpm;
  p.cue_point = t.cue_point;
  for (int i = 0; i < kHotCues; ++i) {
    p.hot_cue[i] = t.hot_cue[i];
    p.hot_loop_end[i] = t.hot_loop_end[i];
  }
  return build_block(name, BlockKind::Track, slot, generation, *t.audio, t.grid, t.memory_cues, p, t.path, error);
}

bool create_sample_block(const std::string& name, int slot, std::uint64_t generation, const LoadedSample& s, std::string& error) {
  if (!s.audio) return fail(error, "sample has no audio");
  BlockHeader p;
  p.bpm = s.bpm;
  p.gain = s.gain;
  p.flags = (s.loop ? 1u : 0u) | (s.sync ? 2u : 0u);
  return build_block(name, BlockKind::Sample, slot, generation, *s.audio, {}, {}, p, "", error);
}

namespace {
bool map_block(const std::string& name, BlockKind kind, int slot, std::uint64_t generation, std::shared_ptr<Shm>& keep,
               ParsedBlock& pb, std::string& error) {
  Shm shm;
  if (!Shm::open(name, /*writable=*/false, shm, error)) return false;
  if (!parse_block(shm.data(), shm.size(), kind, pb, error)) return false;
  if (pb.h.slot != static_cast<std::uint32_t>(slot) || pb.h.generation != generation) return fail(error, "stale block");
  keep = std::make_shared<Shm>(std::move(shm));
  return true;
}

std::shared_ptr<TrackAudio> audio_of(const ParsedBlock& pb, const std::shared_ptr<Shm>& keep) {
  auto a = std::make_shared<TrackAudio>();
  a->ext_pcm = pb.pcm;
  a->ext_hold = keep;
  a->sample_rate = pb.h.sample_rate;
  a->frames = pb.h.frames;
  return a;
}
}  // namespace

std::unique_ptr<LoadedTrack> map_track_block(const std::string& name, int slot, std::uint64_t generation, std::string& error) {
  std::shared_ptr<Shm> keep;
  ParsedBlock pb;
  if (!map_block(name, BlockKind::Track, slot, generation, keep, pb, error)) return nullptr;
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio_of(pb, keep);
  t->track_id = pb.h.track_id;
  t->path = pb.path;
  t->bpm = pb.h.bpm;
  t->grid = std::move(pb.grid);
  t->memory_cues = std::move(pb.memory_cues);
  t->cue_point = std::max(0.0, pb.h.cue_point);
  for (int i = 0; i < kHotCues; ++i) {
    t->hot_cue[i] = pb.h.hot_cue[i] >= 0 ? pb.h.hot_cue[i] : -1;
    t->hot_loop_end[i] = pb.h.hot_loop_end[i] >= 0 ? pb.h.hot_loop_end[i] : -1;
  }
  return t;
}

std::unique_ptr<LoadedSample> map_sample_block(const std::string& name, int slot, std::uint64_t generation, std::string& error) {
  std::shared_ptr<Shm> keep;
  ParsedBlock pb;
  if (!map_block(name, BlockKind::Sample, slot, generation, keep, pb, error)) return nullptr;
  auto s = std::make_unique<LoadedSample>();
  s->audio = audio_of(pb, keep);
  s->bpm = pb.h.bpm;
  s->loop = (pb.h.flags & 1u) != 0;
  s->sync = (pb.h.flags & 2u) != 0;
  s->gain = pb.h.gain;
  return s;
}

// ---- commands -------------------------------------------------------------------------------------
namespace {
enum class Scope { Reject, Global, Deck, Fx, Slot };
Scope scope_of(Op op) {
  switch (op) {
    case Op::Crossfader: case Op::HeadMix: case Op::HeadLevel: case Op::MasterLevel: case Op::MasterBpm:
    case Op::SamplerLevel: case Op::RecStart: case Op::RecStop: case Op::StreamEnable: case Op::SamplerStop:
      return Scope::Global;
    case Op::FxSelect: case Op::FxEnable: case Op::FxWet: case Op::FxTime: case Op::FxParam:
      return Scope::Fx;
    case Op::SamplerTrigger: case Op::SamplerSync: case Op::SamplerLoop: case Op::SamplerGain:
      return Scope::Slot;
    case Op::Load: case Op::SamplerLoad: case Op::SchedInsert: case Op::SchedCancel: case Op::SchedCancelAll: case Op::Count:
      return Scope::Reject;
    default:
      return Scope::Deck;
  }
}
}  // namespace

bool validate_command(const WireCommand& w, Command& out) {
  if (w.op >= static_cast<std::uint8_t>(Op::Count)) return false;
  const Op op = static_cast<Op>(w.op);
  const Scope sc = scope_of(op);
  if (sc == Scope::Reject) return false;
  if (!finite_in(w.f, -1e15, 1e15) || !finite_in(w.d, -1e15, 1e15)) return false;
  int deck = w.deck;
  switch (sc) {
    case Scope::Global: deck = 0; break;
    case Scope::Deck: if (deck >= kMaxDecks) return false; break;
    case Scope::Fx: if (deck > kFxMaster) return false; break;
    case Scope::Slot: if (deck >= kSamplerSlots) return false; break;
    case Scope::Reject: return false;
  }
  switch (op) {
    case Op::HotCueDown: case Op::HotCueUp: case Op::HotCueClear:
      if (w.i < 0 || w.i >= kHotCues) return false;
      break;
    case Op::FxSelect: case Op::FxEnable: case Op::FxWet: case Op::FxTime: case Op::FxParam:
      if (w.i < 0 || w.i >= kFxSlots) return false;
      if (op == Op::FxSelect && !(w.d >= 0 && w.d < static_cast<double>(EffectKind::Count))) return false;
      break;
    case Op::ColorFx:
      if (w.i < 0 || w.i >= static_cast<int>(ColorFx::Count)) return false;
      break;
    case Op::SamplerStop:
      if (w.i < -1 || w.i >= kSamplerSlots) return false;
      break;
    default: break;
  }
  out = Command{};
  out.op = op;
  out.deck = static_cast<std::uint8_t>(deck);
  out.i = w.i;
  out.f = w.f;
  out.d = w.d;
  return true;
}

WireCommand to_wire(const Command& c) {
  WireCommand w;
  w.op = static_cast<std::uint8_t>(c.op);
  w.deck = c.deck;
  w.i = c.i;
  w.f = c.f;
  w.d = c.d;
  return w;
}

// ---- framing --------------------------------------------------------------------------------------
bool send_frame(Conn& c, std::uint8_t type, const std::vector<std::uint8_t>& payload) {
  if (payload.size() + 1 > kMaxFrame) return false;
  std::vector<std::uint8_t> buf(5 + payload.size());
  const std::uint32_t len = static_cast<std::uint32_t>(payload.size() + 1);
  std::memcpy(buf.data(), &len, 4);
  buf[4] = type;
  if (!payload.empty()) std::memcpy(buf.data() + 5, payload.data(), payload.size());
  return c.send_all(buf.data(), buf.size());
}

bool recv_frame(Conn& c, std::uint8_t& type, std::vector<std::uint8_t>& payload, int timeout_ms, bool* timed_out) {
  std::uint32_t len = 0;
  if (!c.recv_all(&len, 4, timeout_ms, timed_out)) return false;
  if (len < 1 || len > kMaxFrame) return false;
  std::vector<std::uint8_t> body(len);
  if (!c.recv_all(body.data(), len, std::max(timeout_ms, 2000), timed_out)) return false;
  type = body[0];
  payload.assign(body.begin() + 1, body.end());
  return true;
}

}  // namespace deckhand::ipc
