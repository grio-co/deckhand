#pragma once
// Engine IPC: everything the UI process and the engine process share.
//
//  * OS access (shared memory, instance lock, local sockets, spawning) sits behind the small classes in the first
//    section; ipc_posix.cpp implements them for macOS and Linux and a Windows file can add the same interface.
//  * One shared-memory segment ("main") holds, with no pointers in it (offsets and indices only):
//      - a multi-producer command ring UI -> engine,
//      - seqlock snapshots engine -> UI (decks, mixer, sampler, recorder, device, controllers, loaded-track info),
//      - an event ring and a stream-tap ring engine -> UI.
//  * Decoded audio travels in one segment per track ("block", see BlockHeader) created by the UI and mapped
//    read-only by the engine. The engine treats everything in a block and in a command as untrusted input.
//  * Non-real-time requests go over a stream socket (see Wr / Rd and the Req enum).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "deckhand/audio/engine.hpp"

namespace deckhand::ipc {

constexpr std::uint32_t kProtocolVersion = 1;
constexpr int kBlockSlots = 64;  // track / sample blocks in flight at once

// =====================================================================================================
// OS access
// =====================================================================================================
std::uint64_t now_ns();  // monotonic; comparable between processes on one machine
std::uint32_t current_pid();
bool process_alive(std::uint32_t pid);
bool reap_child(std::uint32_t pid);  // true when `pid` was our child and has exited (collects the zombie)
std::string executable_dir();        // folder holding the running executable ("" when unknown)

// A named, fixed-size block of shared memory.
class Shm {
 public:
  Shm() = default;
  ~Shm() { close(); }
  Shm(Shm&& o) noexcept { *this = std::move(o); }
  Shm& operator=(Shm&& o) noexcept;
  Shm(const Shm&) = delete;
  Shm& operator=(const Shm&) = delete;
  // Creates a zero-filled segment of `size` bytes, mapped read-write. `replace`: remove a stale object of that name
  // first; otherwise an existing name is an error.
  static bool create(const std::string& name, std::size_t size, Shm& out, std::string& error, bool replace = true);
  // Maps an existing segment. `size` of the result is the segment's real size.
  static bool open(const std::string& name, bool writable, Shm& out, std::string& error);
  static void remove(const std::string& name);  // no error if it does not exist
  void* data() const { return base_; }
  std::size_t size() const { return size_; }
  bool valid() const { return base_ != nullptr; }
  void close();

 private:
  void* base_ = nullptr;
  std::size_t size_ = 0;
};

// Single-instance lock; released when the process exits, however it exits.
class InstanceLock {
 public:
  InstanceLock() = default;
  ~InstanceLock() { release(); }
  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;
  bool acquire(const std::string& path, std::string& error);  // false when another process holds it
  void release();
  void release_and_remove();  // also deletes the lock file (safe: acquire() rechecks that the file it locked is still the one on disk)
  bool held() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
  std::string path_;
};

// A connected stream socket.
class Conn {
 public:
  Conn() = default;
  ~Conn() { close(); }
  Conn(Conn&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  Conn& operator=(Conn&& o) noexcept {
    if (this != &o) {
      close();
      fd_ = o.fd_;
      o.fd_ = -1;
    }
    return *this;
  }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
  static bool connect(const std::string& path, Conn& out);
  bool valid() const { return fd_ >= 0; }
  void close();
  bool send_all(const void* data, std::size_t n);
  // Reads exactly n bytes; false on timeout, error or end of stream. `timed_out` tells the first from the rest.
  bool recv_all(void* data, std::size_t n, int timeout_ms, bool* timed_out = nullptr);

 private:
  friend class Listener;
  int fd_ = -1;
};

class Listener {
 public:
  Listener() = default;
  ~Listener() { close(); }
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  bool listen(const std::string& path, std::string& error);  // replaces a stale socket file
  bool accept(Conn& out, int timeout_ms);                   // false on timeout
  void close();

 private:
  int fd_ = -1;
  std::string path_;
};

// Where one engine lives for one data folder. Everything is derived from the folder's canonical path and the
// user id, so two portable copies do not collide. Names respect macOS limits (31-character shm names, 104-byte
// socket paths).
struct SessionNames {
  std::string dir;       // private per-user folder (0700) for the lock and the socket
  std::string lock;      // instance lock file
  std::string socket;    // control socket
  std::string shm_main;  // main segment
  std::string shm_block_prefix;
  std::string block(int slot) const;
};
bool session_names(const std::string& data_dir, SessionNames& out, std::string& error);

// Starts `exe args...` as a session of its own (it survives this process), stdout/stderr appended to log_file.
bool spawn_detached(const std::string& exe, const std::vector<std::string>& args, const std::string& log_file,
                    std::uint32_t& pid, std::string& error);

// =====================================================================================================
// Lock-free structures that live in shared memory
// =====================================================================================================
// Seqlock: one writer, any number of readers, copying in 64-bit words so readers never block the writer.
template <class T>
struct SeqSlot {
  static_assert(std::is_trivially_copyable_v<T>, "snapshots are plain data");
  static_assert(sizeof(T) % 8 == 0 && alignof(T) <= 8, "pad snapshots to a multiple of 8 bytes");
  static constexpr std::size_t kWords = sizeof(T) / 8;
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

  std::atomic<std::uint32_t> seq{0};
  std::atomic<std::uint64_t> words[kWords];

  void write(const T& v) {
    std::uint64_t w[kWords];
    std::memcpy(w, &v, sizeof(T));
    const std::uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < kWords; ++i) words[i].store(w[i], std::memory_order_relaxed);
    seq.store(s + 2, std::memory_order_release);
  }
  bool try_read(T& out) const {
    const std::uint32_t a = seq.load(std::memory_order_acquire);
    if (a & 1u) return false;
    std::uint64_t w[kWords];
    for (std::size_t i = 0; i < kWords; ++i) w[i] = words[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) != a) return false;
    std::memcpy(&out, w, sizeof(T));
    return true;
  }
  // Retries until a consistent copy is read, for at most `limit`. A writer that died mid-write leaves the sequence
  // odd for good; a torn copy is never returned: on timeout `out` is left alone and false comes back.
  bool read_for(T& out, std::chrono::microseconds limit) const {
    std::chrono::steady_clock::time_point start{};
    for (unsigned i = 0;; ++i) {
      if (try_read(out)) return true;
      if ((i & 63u) != 63u) continue;
      const auto now = std::chrono::steady_clock::now();
      if (i == 63u) start = now;
      else if (now - start > limit) return false;
    }
  }
  // Consistent copy, or a value-initialised T when the writer is stuck (see read_for).
  T read() const {
    T out{};
    if (!read_for(out, std::chrono::milliseconds(3))) return T{};
    return out;
  }
};

// Wire form of an engine Command: no pointers. Op::Load carries i = block slot, aux = block generation.
struct WireCommand {
  std::uint8_t op = 0;
  std::uint8_t deck = 0;
  std::uint16_t reserved = 0;
  std::int32_t i = 0;
  float f = 0;
  std::uint32_t pad = 0;
  double d = 0;
  std::uint64_t aux = 0;
};
static_assert(sizeof(WireCommand) == 32);

// Multi-producer, single-consumer ring (Vyukov bounded queue). Producers are threads of the one attached UI
// process; if that process dies mid-push the consumer can stall at the hole, so a new UI's Attach resets the
// ring (reset() is only legal while no producer runs).
template <std::size_t N>
struct CmdRing {
  static_assert((N & (N - 1)) == 0);
  struct Slot {
    std::atomic<std::uint64_t> seq;
    WireCommand cmd;
  };
  std::atomic<std::uint64_t> enq{0};
  std::atomic<std::uint64_t> deq{0};
  Slot slots[N];

  void reset() {
    for (std::size_t i = 0; i < N; ++i) slots[i].seq.store(i, std::memory_order_relaxed);
    enq.store(0, std::memory_order_relaxed);
    deq.store(0, std::memory_order_release);
  }
  bool push(const WireCommand& c) {
    std::uint64_t pos = enq.load(std::memory_order_relaxed);
    for (;;) {
      Slot& s = slots[pos & (N - 1)];
      const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
      const std::int64_t diff = static_cast<std::int64_t>(seq - pos);
      if (diff == 0) {
        if (enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          s.cmd = c;
          s.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // full
      } else {
        pos = enq.load(std::memory_order_relaxed);
      }
    }
  }
  // Single consumer.
  bool pop(WireCommand& out) {
    const std::uint64_t pos = deq.load(std::memory_order_relaxed);
    Slot& s = slots[pos & (N - 1)];
    if (s.seq.load(std::memory_order_acquire) != pos + 1) return false;
    out = s.cmd;
    s.seq.store(pos + N, std::memory_order_release);
    deq.store(pos + 1, std::memory_order_release);
    return true;
  }
};

// Event kinds on the wire: 0..5 are EventType; the rest are for the UI process itself.
enum WireEventKind : std::uint32_t {
  kEvBrowseSteps = 100,   // slot = steps (controller browse knob)
  kEvBrowsePress = 101,
  kEvLoadSelected = 102,  // deck = target deck
  kEvLoadResult = 110,    // deck, slot = block slot, sched_id = block generation, clock_frame = 0 ok / error code
};
struct WireEvent {
  std::uint32_t kind = 0;
  std::int32_t slot = 0;
  std::uint32_t deck = 0;
  std::uint32_t pad = 0;
  double frame = 0;
  std::int64_t track_id = 0;
  std::uint64_t sched_id = 0;
  std::uint64_t clock_frame = 0;
};
static_assert(sizeof(WireEvent) == 48);

// Single-producer, single-consumer ring of fixed-size entries. When it is full the newest entry is dropped.
template <class T, std::size_t N>
struct SpscShared {
  static_assert((N & (N - 1)) == 0);
  std::atomic<std::uint64_t> head{0};
  std::atomic<std::uint64_t> tail{0};
  T buf[N];

  bool push(const T& v) {
    const std::uint64_t h = head.load(std::memory_order_relaxed);
    const std::uint64_t t = tail.load(std::memory_order_acquire);
    if (h - t >= N) return false;
    buf[h & (N - 1)] = v;
    head.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& out) {
    const std::uint64_t t = tail.load(std::memory_order_relaxed);
    const std::uint64_t h = head.load(std::memory_order_acquire);
    if (t == h) return false;
    out = buf[t & (N - 1)];
    tail.store(t + 1, std::memory_order_release);
    return true;
  }
};

// Interleaved stereo float frames engine -> UI (the stream tap).
template <std::size_t Frames>
struct StreamShared {
  static_assert((Frames & (Frames - 1)) == 0);
  std::atomic<std::uint64_t> head{0};  // frames written
  std::atomic<std::uint64_t> tail{0};  // frames read
  float buf[Frames * 2];

  std::size_t free_frames() const {
    return Frames - static_cast<std::size_t>(head.load(std::memory_order_relaxed) - tail.load(std::memory_order_acquire));
  }
  std::size_t push(const float* frames, std::size_t n) {
    const std::uint64_t h = head.load(std::memory_order_relaxed);
    n = std::min(n, free_frames());
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t at = static_cast<std::size_t>((h + i) & (Frames - 1)) * 2;
      buf[at] = frames[2 * i];
      buf[at + 1] = frames[2 * i + 1];
    }
    head.store(h + n, std::memory_order_release);
    return n;
  }
  std::size_t pop(float* out, std::size_t max_frames) {
    const std::uint64_t t = tail.load(std::memory_order_relaxed);
    const std::uint64_t h = head.load(std::memory_order_acquire);
    std::size_t n = static_cast<std::size_t>(h - t);
    if (n > Frames) n = Frames;  // a corrupted head cannot make us read outside the ring
    n = std::min(n, max_frames);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t at = static_cast<std::size_t>((t + i) & (Frames - 1)) * 2;
      out[2 * i] = buf[at];
      out[2 * i + 1] = buf[at + 1];
    }
    tail.store(t + n, std::memory_order_release);
    return n;
  }
  void discard() { tail.store(head.load(std::memory_order_acquire), std::memory_order_release); }
};

// ---- snapshot payloads ---------------------------------------------------------------------------
struct DeckMeta {  // what the UI needs to rebuild a deck after re-attaching
  std::int64_t track_id = 0;
  std::uint64_t generation = 0;
  std::uint32_t loaded = 0;
  std::uint32_t path_len = 0;
  char path[512] = {};
};
struct DeviceInfo {
  std::uint32_t running = 0;
  std::int32_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t name_len = 0;
  char name[128] = {};
};
struct RecInfo {
  RecorderStatus st;
  std::uint32_t path_len = 0;
  std::uint32_t pad = 0;
  char path[480] = {};
};
constexpr int kHubNames = 8;
struct HubInfo {
  std::uint32_t n_connected = 0;
  std::uint32_t learn_state = 0;  // LearnState
  std::int32_t audio_master = 0, audio_phones = 0;
  std::uint32_t audio_valid = 0, pad = 0;
  char names[kHubNames][64] = {};
  char audio_match[64] = {};
  char learn_device[64] = {};
  char learn_error[128] = {};
  char learn_line[192] = {};
};

constexpr std::uint64_t kMainMagic = 0x31303050414D4844ull;  // "DHMAP001"
enum EngineState : std::uint32_t { kStarting = 0, kRunning = 1, kStopping = 2 };

struct Header {
  std::uint64_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t header_size = 0;
  std::uint64_t total_size = 0;
  std::uint32_t fingerprint[10] = {};  // sizes of the shared types: both sides must be built from the same source
  std::atomic<std::uint64_t> session_id{0};
  std::atomic<std::uint32_t> engine_pid{0};
  std::atomic<std::uint32_t> engine_state{0};
  std::atomic<std::uint32_t> ui_pid{0};
  std::atomic<std::uint32_t> ui_epoch{0};
  std::atomic<std::uint64_t> engine_heartbeat_ns{0};
  std::atomic<std::uint64_t> ui_heartbeat_ns{0};
  std::atomic<std::uint64_t> engine_start_ns{0};
  std::atomic<std::uint64_t> clock_frames{0};
  std::atomic<std::uint64_t> stream_dropped{0};
  std::atomic<std::uint32_t> scheduled_count{0};
  std::atomic<std::uint32_t> rejected_commands{0};
};

constexpr std::size_t kCmdRingSize = 4096, kEventRingSize = 2048, kStreamFrames = 32768;
struct Shared {
  Header hdr;
  SeqSlot<DeckView> deck[kMaxDecks];
  SeqSlot<DeckMeta> meta[kMaxDecks];
  SeqSlot<MixerView> mixer;
  SeqSlot<SamplerView> sampler[kSamplerSlots];
  SeqSlot<RecInfo> rec;
  SeqSlot<DeviceInfo> device;
  SeqSlot<HubInfo> hub;
  CmdRing<kCmdRingSize> cmd;
  SpscShared<WireEvent, kEventRingSize> events;
  StreamShared<kStreamFrames> stream;
};

// Fills in a freshly created (zeroed) segment: header fingerprint, ring sequence numbers.
void init_shared(Shared& s);
// What a client checks before trusting a mapped main segment.
bool check_shared(const void* base, std::size_t size, std::string& error);

// =====================================================================================================
// Track blocks
// =====================================================================================================
constexpr std::uint64_t kBlockMagic = 0x314B4C42484B4544ull;  // "DEKHBLK1"-style tag
constexpr std::uint32_t kBlockVersion = 1, kBlockSealed = 0x5EA1ED01u;
enum class BlockKind : std::uint32_t { Track = 1, Sample = 2 };
constexpr std::uint64_t kMaxBlockBytes = 8ull << 30;
constexpr std::uint64_t kMaxBlockFrames = 1ull << 31;
constexpr std::uint32_t kMaxGridSections = 100000, kMaxMemoryCues = 100000, kMaxPath = 511;

struct WireGrid {
  double first_beat_frame = 0;
  double bpm = 0;
  std::int32_t bar_beat = 1;
  std::int32_t pad = 0;
};
static_assert(sizeof(WireGrid) == 24);

struct BlockHeader {
  std::uint64_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t kind = 0;
  std::uint64_t total_size = 0;
  std::uint64_t generation = 0;
  std::uint32_t slot = 0;
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;
  std::uint64_t pcm_offset = 0;
  std::uint64_t grid_offset = 0;
  std::uint64_t memcue_offset = 0;
  std::uint32_t grid_count = 0;
  std::uint32_t memcue_count = 0;
  std::int64_t track_id = 0;
  double bpm = 0;
  double cue_point = 0;
  double hot_cue[kHotCues] = {};
  double hot_loop_end[kHotCues] = {};
  std::uint32_t flags = 0;  // sample: bit 0 loop, bit 1 sync
  float gain = 1.0f;
  std::uint32_t path_len = 0;
  std::uint32_t sealed = 0;
  char path[512] = {};
};

struct ParsedBlock {
  BlockHeader h;
  const float* pcm = nullptr;  // points into the mapping
  std::vector<GridSection> grid;
  std::vector<double> memory_cues;
  std::string path;
};

// Checks everything about a mapped block and copies the small parts out. Never reads outside [base, base+size).
// `scan_pcm`: also reject audio containing NaN / infinity (touches every page).
bool parse_block(const void* base, std::size_t size, BlockKind kind, ParsedBlock& out, std::string& error,
                 bool scan_pcm = true);

// UI side: creates the named segment, writes and seals the block, and unmaps it again (the name stays until the
// engine takes it or someone unlinks it).
bool create_track_block(const std::string& name, int slot, std::uint64_t generation, const LoadedTrack& t,
                        std::string& error);
bool create_sample_block(const std::string& name, int slot, std::uint64_t generation, const LoadedSample& s,
                         std::string& error);

// Engine side: maps a block read-only, validates it (slot and generation must match what the command said) and
// returns the engine objects. The mapping lives as long as the returned audio does.
std::unique_ptr<LoadedTrack> map_track_block(const std::string& name, int slot, std::uint64_t generation,
                                             std::string& error);
std::unique_ptr<LoadedSample> map_sample_block(const std::string& name, int slot, std::uint64_t generation,
                                               std::string& error);

// Converts a wire command into an engine command, rejecting anything the engine should not see (unknown or
// internal ops, out-of-range decks and slots, non-finite numbers). Op::Load is not handled here.
bool validate_command(const WireCommand& w, Command& out);
WireCommand to_wire(const Command& c);

// =====================================================================================================
// Control socket protocol
// =====================================================================================================
enum Req : std::uint8_t {
  kReqHello = 1,       // -> u32 version, u64 session, u32 pid
  kReqAttach,          // u32 pid, u32 version -> u8 ok, str error, u32 epoch
  kReqDetach,
  kReqOpenDevice,      // str hint, i32 master, i32 phones, u32 period, u8 follow -> u8 ok, str error
  kReqCloseDevice,
  kReqDevices,         // -> u32 n, str...
  kReqRecOpen,         // str path, u8 pre -> u8 ok, str error
  kReqRecStop,
  kReqRecClose,
  kReqSamplerLoad,     // i32 slot, i32 block slot, u64 generation -> u8 ok, str error
  kReqSchedule,        // WireCommand fields + schedule fields -> u64 id
  kReqCancel,          // u64 id -> u8
  kReqCancelAll,
  kReqClockAtWall,     // i64 steady ns -> u64 frame
  kReqStreamEnable,    // u8 on, u8 pre
  kReqQuit,            // u32 fade ms
  kReqHubLearnBegin,   // str action -> u8 ok, str error
  kReqHubLearnCancel,
  kReqHubLearnCommit,  // -> u8 ok
  kReqHubLearnResult,  // u8 finalize -> str line
  kReqPing,
};

class Wr {
 public:
  void u8(std::uint8_t v) { b_.push_back(v); }
  void u16(std::uint16_t v) { raw(&v, 2); }
  void u32(std::uint32_t v) { raw(&v, 4); }
  void i32(std::int32_t v) { raw(&v, 4); }
  void u64(std::uint64_t v) { raw(&v, 8); }
  void i64(std::int64_t v) { raw(&v, 8); }
  void f64(double v) { raw(&v, 8); }
  void str(const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 65535);
    u16(static_cast<std::uint16_t>(n));
    raw(s.data(), n);
  }
  const std::vector<std::uint8_t>& bytes() const { return b_; }

 private:
  void raw(const void* p, std::size_t n) {
    const auto* c = static_cast<const std::uint8_t*>(p);
    b_.insert(b_.end(), c, c + n);
  }
  std::vector<std::uint8_t> b_;
};

// Bounds-checked reader: reading past the end returns zeros and clears ok().
class Rd {
 public:
  Rd(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
  std::uint8_t u8() { std::uint8_t v = 0; take(&v, 1); return v; }
  std::uint16_t u16() { std::uint16_t v = 0; take(&v, 2); return v; }
  std::uint32_t u32() { std::uint32_t v = 0; take(&v, 4); return v; }
  std::int32_t i32() { std::int32_t v = 0; take(&v, 4); return v; }
  std::uint64_t u64() { std::uint64_t v = 0; take(&v, 8); return v; }
  std::int64_t i64() { std::int64_t v = 0; take(&v, 8); return v; }
  double f64() { double v = 0; take(&v, 8); return v; }
  std::string str() {
    const std::size_t n = u16();
    if (!ok_ || n > n_ - at_) {
      ok_ = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p_ + at_), n);
    at_ += n;
    return s;
  }
  bool ok() const { return ok_; }

 private:
  void take(void* out, std::size_t n) {
    if (!ok_ || n > n_ - at_) {
      ok_ = false;
      return;
    }
    std::memcpy(out, p_ + at_, n);
    at_ += n;
  }
  const std::uint8_t* p_;
  std::size_t n_, at_ = 0;
  bool ok_ = true;
};

constexpr std::size_t kMaxFrame = 1u << 20;
// Frame: u32 length (type byte + payload), u8 type, payload.
bool send_frame(Conn& c, std::uint8_t type, const std::vector<std::uint8_t>& payload);
bool recv_frame(Conn& c, std::uint8_t& type, std::vector<std::uint8_t>& payload, int timeout_ms, bool* timed_out = nullptr);

}  // namespace deckhand::ipc
