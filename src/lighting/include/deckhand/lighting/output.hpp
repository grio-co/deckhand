#pragma once
// DMX outputs. Every output is OFF unless the user enabled it: constructing one never touches the network.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/lighting/net.hpp"
#include "deckhand/lighting/packets.hpp"

namespace deckhand::lighting {

// One universe worth of channel values. `universe` is the user-facing number (1-based, as in the patch).
struct UniverseFrame {
  int universe = 1;
  std::array<std::uint8_t, kDmxSlots> data{};
};

struct OutputStats {
  std::uint64_t frames = 0, packets = 0, bytes = 0, errors = 0, poll_replies = 0;
  char last_error[96] = {0};
};

class Output {
 public:
  virtual ~Output() = default;
  virtual std::string name() const = 0;
  virtual bool open(std::string* err) = 0;
  virtual void close() {}
  // Sends the given universes (one packet or frame each). Called at the refresh rate or the keep-alive rate.
  virtual void send(const UniverseFrame* frames, int count) = 0;
  // Housekeeping from the engine thread (answers ArtPoll).
  virtual void service() {}
  OutputStats stats() const { return stats_; }

 protected:
  void note_error(const char* msg);
  OutputStats stats_;
};

// Swallows everything (preview only).
class NullOutput : public Output {
 public:
  std::string name() const override { return "null"; }
  bool open(std::string*) override { return true; }
  void send(const UniverseFrame*, int count) override { stats_.frames++; stats_.packets += static_cast<unsigned>(count); }
};

// Keeps the last frame per universe slot (and optionally the whole history) in memory; for tests and UI previews.
class CaptureOutput : public Output {
 public:
  struct Sent {
    int universe;
    std::array<std::uint8_t, kDmxSlots> data;
  };
  std::string name() const override { return "capture"; }
  bool open(std::string*) override { return true; }
  void send(const UniverseFrame* frames, int count) override;
  bool keep_history = false;  // set before use; history growth allocates (tests only)
  std::vector<Sent> history;
  std::array<UniverseFrame, kMaxUniverses> last{};
  int last_count = 0;
  std::uint64_t send_calls = 0;
};

struct ArtNetConfig {
  bool enabled = false;
  std::string host;                    // destination: a node, or a broadcast address you chose; empty = not configured
  std::uint16_t port = kArtNetPort;    // destination port
  std::string bind_ip = "0.0.0.0";
  std::uint16_t listen_port = kArtNetPort;  // where ArtPoll is received (0 = ephemeral)
  bool respond_to_poll = true;
  int first_universe = 0;              // Port-Address of patch universe 1
  std::string node_ip;                 // announced in ArtPollReply (dotted quad, "" = 0.0.0.0)
  std::string short_name = "DeckHand", long_name = "DeckHand lighting";
};

struct SacnConfig {
  bool enabled = false;
  bool multicast = true;               // false: unicast to `host`
  std::string host;                    // unicast destination
  std::uint16_t port = kSacnPort;
  std::string bind_ip = "0.0.0.0";
  std::string interface_ip;            // multicast egress interface ("" = system default)
  int priority = 100;                  // 0..200
  int universe_offset = 0;             // sACN universe = patch universe + offset
  int sync_universe = 0;               // 0 = no synchronisation; else data packets name it and a sync packet follows each frame
  int slots = kDmxSlots;               // 24..512 channels per packet
  std::string source_name = "DeckHand";
  std::string cid;                     // 32 hex digits; empty = derived from the source name (stable)
};

enum class SerialKind { Pro, OpenDmx };
struct SerialConfig {
  bool enabled = false;
  std::string device;                  // e.g. /dev/cu.usbserial-XXXX or /dev/ttyUSB0
  SerialKind kind = SerialKind::Pro;
  int universe = 1;                    // which patch universe it carries
};

std::unique_ptr<Output> make_artnet_output(const ArtNetConfig& cfg);
std::unique_ptr<Output> make_sacn_output(const SacnConfig& cfg);
std::unique_ptr<Output> make_serial_output(const SerialConfig& cfg);
// True where serial DMX can work at all on this platform / for this kind (see docs/LIGHTING.md).
bool serial_supported(SerialKind kind);

// Refresh pacing with an injected clock: render-and-check at the refresh rate; send when something changed, otherwise
// only every keep-alive interval.
class Pacer {
 public:
  void configure(double refresh_hz, double keepalive_s);
  bool frame_due(double now_s);                   // true once per refresh period (no catch-up bursts after a stall)
  bool should_send(double now_s, bool changed);   // call after a due frame was rendered
  double next_due() const { return next_; }
  double period() const { return period_; }

 private:
  double period_ = 1.0 / 40, keepalive_ = 1.0, next_ = 0, last_sent_ = -1e18;
  bool started_ = false;
};

}  // namespace deckhand::lighting
