#pragma once
// Wire encoders for Art-Net (ArtDmx, ArtPoll, ArtPollReply) and sACN / ANSI E1.31 (data, synchronisation), plus the
// framed message of serial DMX widgets. Written from the public protocol specifications. Pure functions: no sockets.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace deckhand::lighting {

constexpr int kDmxSlots = 512;
constexpr int kMaxUniverses = 8;

// ---- Art-Net -------------------------------------------------------------------------------------------------
constexpr std::uint16_t kArtNetPort = 6454;
constexpr std::uint16_t kOpPoll = 0x2000, kOpPollReply = 0x2100, kOpDmx = 0x5000;
constexpr std::size_t kArtDmxHeader = 18;
constexpr std::size_t kArtPollReplySize = 239;

// ArtDmx. port_address is the 15-bit Port-Address (Net << 8 | Sub-Net << 4 | Universe). len is rounded up to an
// even number in 2..512 (the spec's rule); sequence 0 disables sequencing, otherwise 1..255. Returns the bytes written.
std::size_t encode_artdmx(std::uint8_t* out, std::uint8_t sequence, std::uint16_t port_address, const std::uint8_t* dmx,
                          std::size_t len, std::uint8_t physical = 0);
// Next Art-Net sequence number: 1..255, wrapping to 1 (0 is reserved for "off").
std::uint8_t next_artnet_sequence(std::uint8_t prev);

struct ArtPollReplyInfo {
  std::uint8_t ip[4] = {0, 0, 0, 0};
  std::uint16_t port_address_net_sub = 0;  // Net (7 bit) << 4 | Sub-Net (4 bit) of the first output port
  std::string short_name = "DeckHand";     // 17 characters + NUL
  std::string long_name = "DeckHand lighting";  // 63 characters + NUL
  std::string node_report = "#0001 [0000] ok";
  std::uint8_t output_universes = 1;        // number of output ports announced (1..4 per ArtPollReply)
  std::uint8_t first_universe_low_nibble = 0;
  std::uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  std::uint16_t oem = 0x00ff;               // unassigned/experimental
  std::uint16_t esta = 0x7ff0;              // prototype manufacturer code
  std::uint8_t bind_index = 1;
};
// 239-byte ArtPollReply announcing `output_universes` DMX output ports (sources of DMX on the network).
std::vector<std::uint8_t> encode_artpollreply(const ArtPollReplyInfo& info);
// True when buf is an ArtPoll packet ("Art-Net\0", OpPoll, protocol version >= 14).
bool is_artpoll(const std::uint8_t* buf, std::size_t n);
// Parsed header of a received ArtDmx (for tests and monitoring). false if malformed.
struct ArtDmxPacket {
  std::uint8_t sequence = 0, physical = 0;
  std::uint16_t port_address = 0;
  std::uint16_t length = 0;
  const std::uint8_t* data = nullptr;
};
bool decode_artdmx(const std::uint8_t* buf, std::size_t n, ArtDmxPacket* out);

// ---- sACN / E1.31 --------------------------------------------------------------------------------------------
constexpr std::uint16_t kSacnPort = 5568;
constexpr std::size_t kE131DataHeader = 126;  // + number of slots = total packet size
constexpr std::size_t kE131SyncSize = 49;
struct E131Params {
  std::array<std::uint8_t, 16> cid{};
  std::string source_name = "DeckHand";  // truncated to 63 bytes
  std::uint8_t priority = 100;           // 0..200
  std::uint16_t sync_address = 0;        // 0 = not synchronised
  std::uint16_t universe = 1;            // 1..63999
  std::uint8_t sequence = 0;
  bool preview = false;
  bool stream_terminated = false;
  bool force_sync = false;
};
// Data packet carrying `slots` (1..512) channels after a zero start code. Returns the packet size (126 + slots).
std::size_t encode_e131_data(std::uint8_t* out, const E131Params& p, const std::uint8_t* dmx, std::size_t slots);
// Synchronisation packet for sync_address.
std::size_t encode_e131_sync(std::uint8_t* out, const std::array<std::uint8_t, 16>& cid, std::uint8_t sequence,
                             std::uint16_t sync_address);
// Multicast group of a universe: "239.255.<hi>.<lo>".
std::string e131_multicast_address(std::uint16_t universe);
std::array<std::uint8_t, 16> cid_from_hex(std::string_view hex32, bool* ok = nullptr);  // 32 hex digits
std::string cid_to_hex(const std::array<std::uint8_t, 16>& cid);

// ---- Serial widget framing -----------------------------------------------------------------------------------
// "Pro" style widgets take: 0x7E, label 6 (send DMX), length LSB, length MSB, start code 0, data..., 0xE7.
std::size_t encode_widget_frame(std::uint8_t* out, const std::uint8_t* dmx, std::size_t slots);

}  // namespace deckhand::lighting
