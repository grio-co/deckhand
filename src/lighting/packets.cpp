#include "deckhand/lighting/packets.hpp"

#include <algorithm>
#include <cstring>

namespace deckhand::lighting {

namespace {
const char kArtId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
void put_str(std::uint8_t* dst, std::size_t cap, const std::string& s) {  // NUL padded, always NUL terminated
  std::memset(dst, 0, cap);
  std::memcpy(dst, s.data(), std::min(s.size(), cap - 1));
}
void be16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v >> 8); p[1] = static_cast<std::uint8_t>(v); }
void be32(std::uint8_t* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
}  // namespace

std::size_t encode_artdmx(std::uint8_t* out, std::uint8_t sequence, std::uint16_t port_address, const std::uint8_t* dmx,
                          std::size_t len, std::uint8_t physical) {
  const std::size_t given = std::clamp<std::size_t>(len, 1, kDmxSlots);
  len = std::max<std::size_t>(2, given + (given & 1));  // the length field is even: pad with one zero slot
  std::memcpy(out, kArtId, 8);
  out[8] = kOpDmx & 0xff;  // OpCode is little endian
  out[9] = kOpDmx >> 8;
  out[10] = 0;  // ProtVerHi
  out[11] = 14; // ProtVerLo
  out[12] = sequence;
  out[13] = physical;
  out[14] = port_address & 0xff;                   // SubUni: Sub-Net and Universe
  out[15] = (port_address >> 8) & 0x7f;            // Net
  be16(out + 16, static_cast<std::uint16_t>(len));  // Length is big endian
  std::memcpy(out + kArtDmxHeader, dmx, given);
  if (len > given) out[kArtDmxHeader + given] = 0;
  return kArtDmxHeader + len;
}

std::uint8_t next_artnet_sequence(std::uint8_t prev) { return prev >= 255 || prev == 0 ? 1 : static_cast<std::uint8_t>(prev + 1); }

std::vector<std::uint8_t> encode_artpollreply(const ArtPollReplyInfo& in) {
  std::vector<std::uint8_t> r(kArtPollReplySize, 0);
  std::uint8_t* p = r.data();
  std::memcpy(p, kArtId, 8);
  p[8] = kOpPollReply & 0xff;
  p[9] = kOpPollReply >> 8;
  std::memcpy(p + 10, in.ip, 4);
  p[14] = kArtNetPort & 0xff;  // Port, little endian
  p[15] = kArtNetPort >> 8;
  p[16] = 0;  // VersInfoH
  p[17] = 1;  // VersInfoL
  p[18] = (in.port_address_net_sub >> 4) & 0x7f;  // NetSwitch
  p[19] = in.port_address_net_sub & 0x0f;         // SubSwitch
  p[20] = in.oem >> 8;
  p[21] = in.oem & 0xff;
  p[22] = 0;     // UbeaVersion
  p[23] = 0xc0;  // Status1: indicators normal
  p[24] = in.esta & 0xff;  // EstaManLo
  p[25] = in.esta >> 8;    // EstaManHi
  put_str(p + 26, 18, in.short_name);
  put_str(p + 44, 64, in.long_name);
  put_str(p + 108, 64, in.node_report);
  const std::uint8_t ports = std::clamp<std::uint8_t>(in.output_universes, 1, 4);
  p[172] = 0;
  p[173] = ports;
  for (int i = 0; i < ports; ++i) {
    p[174 + i] = 0x80;  // PortTypes: output from the Art-Net network, DMX512
    p[182 + i] = 0x80;  // GoodOutput: data is being transmitted
    p[190 + i] = static_cast<std::uint8_t>((in.first_universe_low_nibble + i) & 0x0f);  // SwOut
  }
  p[200] = 0x00;  // Style: StNode
  std::memcpy(p + 201, in.mac, 6);
  std::memcpy(p + 207, in.ip, 4);  // BindIp
  p[211] = in.bind_index;
  p[212] = 0x08;  // Status2: node supports 15-bit Port-Address
  p[226] = 0;     // RefreshRate 40 Hz
  p[227] = 40;
  return r;
}

bool is_artpoll(const std::uint8_t* b, std::size_t n) {
  return n >= 14 && std::memcmp(b, kArtId, 8) == 0 && b[8] == (kOpPoll & 0xff) && b[9] == (kOpPoll >> 8) &&
         (b[10] > 0 || b[11] >= 14);
}

bool decode_artdmx(const std::uint8_t* b, std::size_t n, ArtDmxPacket* o) {
  if (n < kArtDmxHeader || std::memcmp(b, kArtId, 8) != 0 || b[8] != (kOpDmx & 0xff) || b[9] != (kOpDmx >> 8)) return false;
  const std::size_t len = (static_cast<std::size_t>(b[16]) << 8) | b[17];
  if (len < 2 || len > kDmxSlots || n < kArtDmxHeader + len) return false;
  o->sequence = b[12];
  o->physical = b[13];
  o->port_address = static_cast<std::uint16_t>(((b[15] & 0x7f) << 8) | b[14]);
  o->length = static_cast<std::uint16_t>(len);
  o->data = b + kArtDmxHeader;
  return true;
}

namespace {
// Root layer shared by data and sync packets (bytes 0..37); `root_vector` 4 = data, 8 = extended (sync).
void e131_root(std::uint8_t* o, std::size_t total, std::uint32_t root_vector, const std::array<std::uint8_t, 16>& cid) {
  be16(o + 0, 0x0010);  // preamble size
  be16(o + 2, 0x0000);  // post-amble size
  static const std::uint8_t kAcn[12] = {0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00};
  std::memcpy(o + 4, kAcn, 12);
  be16(o + 16, static_cast<std::uint16_t>(0x7000 | (total - 16)));  // flags and length of the root PDU
  be32(o + 18, root_vector);
  std::memcpy(o + 22, cid.data(), 16);
}
}  // namespace

std::size_t encode_e131_data(std::uint8_t* o, const E131Params& p, const std::uint8_t* dmx, std::size_t slots) {
  slots = std::clamp<std::size_t>(slots, 1, kDmxSlots);
  const std::size_t total = kE131DataHeader + slots;
  e131_root(o, total, 0x00000004, p.cid);
  be16(o + 38, static_cast<std::uint16_t>(0x7000 | (total - 38)));  // framing layer
  be32(o + 40, 0x00000002);                                         // VECTOR_E131_DATA_PACKET
  put_str(o + 44, 64, p.source_name);
  o[108] = std::min<std::uint8_t>(p.priority, 200);
  be16(o + 109, p.sync_address);
  o[111] = p.sequence;
  o[112] = static_cast<std::uint8_t>((p.preview ? 0x80 : 0) | (p.stream_terminated ? 0x40 : 0) | (p.force_sync ? 0x20 : 0));
  be16(o + 113, p.universe);
  be16(o + 115, static_cast<std::uint16_t>(0x7000 | (total - 115)));  // DMP layer
  o[117] = 0x02;                                                       // VECTOR_DMP_SET_PROPERTY
  o[118] = 0xa1;                                                       // address type & data type
  be16(o + 119, 0x0000);                                               // first property address
  be16(o + 121, 0x0001);                                               // address increment
  be16(o + 123, static_cast<std::uint16_t>(slots + 1));                // property value count (start code + slots)
  o[125] = 0x00;                                                       // DMX512-A start code
  std::memcpy(o + 126, dmx, slots);
  return total;
}

std::size_t encode_e131_sync(std::uint8_t* o, const std::array<std::uint8_t, 16>& cid, std::uint8_t seq, std::uint16_t sync) {
  e131_root(o, kE131SyncSize, 0x00000008, cid);
  be16(o + 38, static_cast<std::uint16_t>(0x7000 | (kE131SyncSize - 38)));
  be32(o + 40, 0x00000001);  // VECTOR_E131_EXTENDED_SYNCHRONIZATION
  o[44] = seq;
  be16(o + 45, sync);
  o[47] = o[48] = 0;  // reserved
  return kE131SyncSize;
}

std::string e131_multicast_address(std::uint16_t u) {
  return "239.255." + std::to_string(u >> 8) + "." + std::to_string(u & 0xff);
}

std::array<std::uint8_t, 16> cid_from_hex(std::string_view h, bool* ok) {
  std::array<std::uint8_t, 16> c{};
  bool good = h.size() == 32;
  for (std::size_t i = 0; good && i < 32; ++i) {
    const char ch = h[i];
    int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    if (v < 0) { good = false; break; }
    c[i / 2] = static_cast<std::uint8_t>((i & 1) ? (c[i / 2] | v) : (v << 4));
  }
  if (!good) c = {};
  if (ok) *ok = good;
  return c;
}

std::string cid_to_hex(const std::array<std::uint8_t, 16>& c) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (auto b : c) { s += d[b >> 4]; s += d[b & 15]; }
  return s;
}

std::size_t encode_widget_frame(std::uint8_t* o, const std::uint8_t* dmx, std::size_t slots) {
  slots = std::clamp<std::size_t>(slots, 1, kDmxSlots);
  const std::size_t n = slots + 1;  // start code + data
  o[0] = 0x7e;
  o[1] = 6;
  o[2] = static_cast<std::uint8_t>(n & 0xff);
  o[3] = static_cast<std::uint8_t>(n >> 8);
  o[4] = 0;
  std::memcpy(o + 5, dmx, slots);
  o[5 + slots] = 0xe7;
  return slots + 6;
}

}  // namespace deckhand::lighting
