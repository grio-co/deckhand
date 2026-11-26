#include "deckhand/lighting/output.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace deckhand::lighting {

void Output::note_error(const char* msg) {
  stats_.errors++;
  std::snprintf(stats_.last_error, sizeof stats_.last_error, "%s", msg);
}

void CaptureOutput::send(const UniverseFrame* f, int count) {
  ++send_calls;
  stats_.frames++;
  last_count = std::min(count, kMaxUniverses);
  for (int i = 0; i < last_count; ++i) {
    last[static_cast<std::size_t>(i)] = f[i];
    stats_.packets++;
    if (keep_history) history.push_back({f[i].universe, f[i].data});
  }
}

// ---- Art-Net ----------------------------------------------------------------------------------------------------
namespace {
class ArtNetOutput : public Output {
 public:
  explicit ArtNetOutput(ArtNetConfig c) : cfg_(std::move(c)) {}
  ~ArtNetOutput() override { close(); }
  std::string name() const override { return "artnet"; }
  bool open(std::string* err) override {
    std::uint8_t ip[4];
    if (!parse_ipv4(cfg_.host, ip)) {
      if (err) *err = "Art-Net: destination host is not a dotted IPv4 address";
      return false;
    }
    if (!sock_.open(cfg_.bind_ip, cfg_.respond_to_poll ? cfg_.listen_port : 0, true, err)) return false;
    // Broadcast only when the user chose a broadcast destination (x.255 or the limited broadcast).
    if (ip[3] == 255) sock_.set_broadcast(true);
    return true;
  }
  void close() override { sock_.close(); }
  void send(const UniverseFrame* f, int count) override {
    if (!sock_.is_open()) return;
    stats_.frames++;
    universes_hint_ = count;
    std::uint8_t buf[kArtDmxHeader + kDmxSlots];
    for (int i = 0; i < count && i < kMaxUniverses; ++i) {
      auto& seq = seq_[static_cast<std::size_t>(i)];
      seq = next_artnet_sequence(seq);
      const int pa = std::clamp(f[i].universe - 1 + cfg_.first_universe, 0, 0x7fff);
      const std::size_t n = encode_artdmx(buf, seq, static_cast<std::uint16_t>(pa), f[i].data.data(), kDmxSlots);
      if (sock_.send_to(cfg_.host, cfg_.port, buf, n) < 0) note_error("Art-Net send failed");
      else { stats_.packets++; stats_.bytes += n; }
    }
  }
  void service() override {
    if (!sock_.is_open() || !cfg_.respond_to_poll) return;
    std::uint8_t buf[600];
    std::string from;
    std::uint16_t fport = 0;
    for (int guard = 0; guard < 8; ++guard) {
      const long n = sock_.recv_from(buf, sizeof buf, &from, &fport);
      if (n <= 0) break;
      if (!is_artpoll(buf, static_cast<std::size_t>(n))) continue;
      ArtPollReplyInfo info;
      parse_ipv4(cfg_.node_ip, info.ip);
      info.short_name = cfg_.short_name;
      info.long_name = cfg_.long_name;
      info.output_universes = static_cast<std::uint8_t>(std::min(4, std::max(1, universes_hint_)));
      info.port_address_net_sub = static_cast<std::uint16_t>(cfg_.first_universe >> 4);
      info.first_universe_low_nibble = static_cast<std::uint8_t>(cfg_.first_universe & 0x0f);
      info.node_report = "#0001 [0000] sending " + std::to_string(stats_.packets) + " packets";
      const auto r = encode_artpollreply(info);
      // Answered to the poller only (the spec's broadcast reply would put traffic on the whole LAN).
      if (sock_.send_to(from, fport, r.data(), r.size()) >= 0) stats_.poll_replies++;
    }
  }

 private:
  ArtNetConfig cfg_;
  UdpSocket sock_;
  std::array<std::uint8_t, kMaxUniverses> seq_{};
  int universes_hint_ = 1;
};

class SacnOutput : public Output {
 public:
  explicit SacnOutput(SacnConfig c) : cfg_(std::move(c)) {
    bool ok = false;
    cid_ = cid_from_hex(cfg_.cid, &ok);
    if (!ok) {  // stable pseudo-identifier from the source name (FNV-1a stretched over 16 bytes)
      std::uint64_t h = 1469598103934665603ull;
      for (char ch : cfg_.source_name) { h ^= static_cast<std::uint8_t>(ch); h *= 1099511628211ull; }
      for (std::size_t i = 0; i < 16; ++i) { h ^= i + 0x9e37; h *= 1099511628211ull; cid_[i] = static_cast<std::uint8_t>(h >> 32); }
      cid_[6] = static_cast<std::uint8_t>((cid_[6] & 0x0f) | 0x40);  // looks like a version 4 UUID
      cid_[8] = static_cast<std::uint8_t>((cid_[8] & 0x3f) | 0x80);
    }
  }
  ~SacnOutput() override { close(); }
  std::string name() const override { return "sacn"; }
  bool open(std::string* err) override {
    std::uint8_t ip[4];
    if (!cfg_.multicast && !parse_ipv4(cfg_.host, ip)) {
      if (err) *err = "sACN: unicast needs a dotted IPv4 host";
      return false;
    }
    if (!sock_.open(cfg_.bind_ip, 0, false, err)) return false;
    if (cfg_.multicast) sock_.set_multicast_send(20, cfg_.interface_ip);
    return true;
  }
  void close() override {
    if (!sock_.is_open()) return;
    if (sent_any_) {  // tell receivers the streams ended (E1.31 stream termination, three times)
      for (int rep = 0; rep < 3; ++rep)
        for (std::size_t i = 0; i < used_; ++i) emit(i, universes_[i], zeros_.data(), true);
    }
    sock_.close();
    sent_any_ = false;
  }
  void send(const UniverseFrame* f, int count) override {
    if (!sock_.is_open()) return;
    stats_.frames++;
    used_ = static_cast<std::size_t>(std::min(count, kMaxUniverses));
    for (std::size_t i = 0; i < used_; ++i) {
      universes_[i] = f[i].universe;
      emit(i, f[i].universe, f[i].data.data(), false);
    }
    if (cfg_.sync_universe > 0) {
      std::uint8_t buf[kE131SyncSize];
      const std::size_t n = encode_e131_sync(buf, cid_, sync_seq_++, static_cast<std::uint16_t>(cfg_.sync_universe));
      transmit(static_cast<std::uint16_t>(cfg_.sync_universe), buf, n);
    }
    sent_any_ = true;
  }

 private:
  void emit(std::size_t slot, int universe, const std::uint8_t* data, bool terminate) {
    E131Params p;
    p.cid = cid_;
    p.source_name = cfg_.source_name;
    p.priority = static_cast<std::uint8_t>(std::clamp(cfg_.priority, 0, 200));
    p.universe = static_cast<std::uint16_t>(std::clamp(universe + cfg_.universe_offset, 1, 63999));
    p.sync_address = static_cast<std::uint16_t>(std::clamp(cfg_.sync_universe, 0, 63999));
    p.sequence = seq_[slot]++;
    p.stream_terminated = terminate;
    std::uint8_t buf[kE131DataHeader + kDmxSlots];
    const std::size_t n = encode_e131_data(buf, p, data, static_cast<std::size_t>(std::clamp(cfg_.slots, 24, kDmxSlots)));
    transmit(p.universe, buf, n);
  }
  void transmit(std::uint16_t universe, const std::uint8_t* buf, std::size_t n) {
    const std::string dest = cfg_.multicast ? e131_multicast_address(universe) : cfg_.host;
    if (sock_.send_to(dest, cfg_.port, buf, n) < 0) note_error("sACN send failed");
    else { stats_.packets++; stats_.bytes += n; }
  }
  SacnConfig cfg_;
  UdpSocket sock_;
  std::array<std::uint8_t, 16> cid_{};
  std::array<std::uint8_t, kMaxUniverses> seq_{};
  std::array<int, kMaxUniverses> universes_{};
  std::array<std::uint8_t, kDmxSlots> zeros_{};
  std::size_t used_ = 0;
  std::uint8_t sync_seq_ = 0;
  bool sent_any_ = false;
};

#if !defined(_WIN32)
class SerialOutput : public Output {
 public:
  explicit SerialOutput(SerialConfig c) : cfg_(std::move(c)) {}
  ~SerialOutput() override { close(); }
  std::string name() const override { return "serial"; }
  bool open(std::string* err) override {
    if (!serial_supported(cfg_.kind)) {
      if (err) *err = "serial DMX: this widget kind is not supported on this platform";
      return false;
    }
    fd_ = ::open(cfg_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
      if (err) *err = "cannot open " + cfg_.device;
      return false;
    }
    termios t;
    if (::tcgetattr(fd_, &t) != 0) { if (err) *err = "not a serial device"; close(); return false; }
    ::cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    if (cfg_.kind == SerialKind::OpenDmx) {
      t.c_cflag |= CSTOPB;  // 8N2
#if defined(__APPLE__)
      ::cfsetspeed(&t, 250000);  // macOS accepts arbitrary rates through the termios speed field
#endif
    } else {
      ::cfsetspeed(&t, B57600);  // framed widgets run the DMX timing themselves; the rate is nominal
    }
    if (::tcsetattr(fd_, TCSANOW, &t) != 0) { if (err) *err = "cannot configure serial port"; close(); return false; }
    return true;
  }
  void close() override { if (fd_ >= 0) ::close(fd_); fd_ = -1; }
  void send(const UniverseFrame* f, int count) override {
    if (fd_ < 0) return;
    const UniverseFrame* u = nullptr;
    for (int i = 0; i < count; ++i) if (f[i].universe == cfg_.universe) u = &f[i];
    if (!u) return;
    stats_.frames++;
    if (cfg_.kind == SerialKind::Pro) {
      std::uint8_t buf[kDmxSlots + 8];
      const std::size_t n = encode_widget_frame(buf, u->data.data(), kDmxSlots);
      write_all(buf, n);
    } else {
#if defined(TIOCSBRK) && defined(TIOCCBRK)
      // BREAK >= 88 us, mark-after-break >= 8 us, then start code + slots. The timing is only as good as the OS
      // scheduler: a framed widget is the dependable choice.
      ::ioctl(fd_, TIOCSBRK);
      nap_us(120);
      ::ioctl(fd_, TIOCCBRK);
      nap_us(16);
#endif
      std::uint8_t buf[kDmxSlots + 1];
      buf[0] = 0;
      std::memcpy(buf + 1, u->data.data(), kDmxSlots);
      write_all(buf, sizeof buf);
    }
  }

 private:
  static void nap_us(long us) { timespec ts{0, us * 1000}; ::nanosleep(&ts, nullptr); }
  void write_all(const std::uint8_t* p, std::size_t n) {
    std::size_t off = 0;
    int spins = 0;
    while (off < n) {
      const ssize_t w = ::write(fd_, p + off, n - off);
      if (w > 0) { off += static_cast<std::size_t>(w); stats_.bytes += static_cast<unsigned>(w); }
      else if (w < 0 && errno == EAGAIN && ++spins < 200) nap_us(500);
      else { note_error("serial write failed"); return; }
    }
    stats_.packets++;
  }
  SerialConfig cfg_;
  int fd_ = -1;
};
#else
class SerialOutput : public Output {  // Windows: not implemented (see docs/LIGHTING.md)
 public:
  explicit SerialOutput(SerialConfig) {}
  std::string name() const override { return "serial"; }
  bool open(std::string* err) override {
    if (err) *err = "serial DMX widgets are not implemented on Windows yet; use Art-Net or sACN";
    return false;
  }
  void send(const UniverseFrame*, int) override {}
};
#endif
}  // namespace

bool serial_supported(SerialKind kind) {
#if defined(_WIN32)
  (void)kind;
  return false;
#elif defined(__APPLE__)
  (void)kind;
  return true;
#else
  return kind == SerialKind::Pro;  // 250 kbaud needs termios2/BOTHER on Linux: not done
#endif
}

std::unique_ptr<Output> make_artnet_output(const ArtNetConfig& c) { return std::make_unique<ArtNetOutput>(c); }
std::unique_ptr<Output> make_sacn_output(const SacnConfig& c) { return std::make_unique<SacnOutput>(c); }
std::unique_ptr<Output> make_serial_output(const SerialConfig& c) { return std::make_unique<SerialOutput>(c); }

void Pacer::configure(double hz, double keepalive_s) {
  period_ = 1.0 / std::clamp(hz, 1.0, 60.0);
  keepalive_ = std::max(keepalive_s, period_);
}
bool Pacer::frame_due(double now) {
  if (!started_) { started_ = true; next_ = now; }
  if (now + 1e-9 < next_) return false;
  next_ += period_;
  if (next_ <= now) next_ = now + period_;  // after a stall: one frame, not a burst
  return true;
}
bool Pacer::should_send(double now, bool changed) {
  if (changed || now - last_sent_ >= keepalive_ - 1e-9) { last_sent_ = now; return true; }
  return false;
}

}  // namespace deckhand::lighting
