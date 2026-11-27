// Lighting outputs over UDP, loopback (127.0.0.1) only, ephemeral ports: nothing here touches the real network.
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

#include "check.hpp"
#include "deckhand/lighting/engine.hpp"
#include "deckhand/lighting/net.hpp"
#include "deckhand/lighting/output.hpp"

using namespace deckhand::lighting;

// Loopback delivery is immediate on the platforms we run on; a few yields cover scheduler noise without sleeping.
static long recv_one(UdpSocket& s, std::uint8_t* buf, std::size_t cap, std::string* from = nullptr, std::uint16_t* fp = nullptr,
                     int wait_ms = 10000) {
  // Wait by the clock, not by a count of yields: a loaded machine can deliver the datagram late.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const long n = s.recv_from(buf, cap, from, fp);
    if (n != 0) return n;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 0;
}

static UniverseFrame frame(int universe, std::uint8_t seed) {
  UniverseFrame f;
  f.universe = universe;
  for (int i = 0; i < kDmxSlots; ++i) f.data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(seed + i);
  return f;
}

static std::uint16_t free_port() {
  UdpSocket t;
  CHECK(t.open("127.0.0.1", 0, false));
  return t.local_port();
}

static void test_artnet_loopback() {
  UdpSocket rx;
  CHECK(rx.open("127.0.0.1", 0, false));
  ArtNetConfig c;
  c.enabled = true;
  c.host = "127.0.0.1";
  c.port = rx.local_port();
  c.bind_ip = "127.0.0.1";
  c.listen_port = free_port();
  c.first_universe = 4;
  auto out = make_artnet_output(c);
  std::string err;
  CHECK(out->open(&err));
  UniverseFrame fr[2] = {frame(1, 10), frame(3, 50)};
  out->send(fr, 2);
  std::uint8_t buf[700];
  ArtDmxPacket p;
  CHECK(recv_one(rx, buf, sizeof buf) == 530 && decode_artdmx(buf, 530, &p));
  CHECK(p.port_address == 4 && p.sequence == 1 && p.data[0] == 10 && p.data[511] == static_cast<std::uint8_t>(10 + 511));
  CHECK(recv_one(rx, buf, sizeof buf) == 530 && decode_artdmx(buf, 530, &p));
  CHECK(p.port_address == 6 && p.data[5] == 55);
  // Sequence advances per frame and wraps 255 -> 1 (never 0).
  std::uint8_t last = p.sequence;
  CHECK(last == 1);
  for (int i = 0; i < 300; ++i) {
    out->send(fr, 1);
    CHECK(recv_one(rx, buf, sizeof buf) == 530);
    decode_artdmx(buf, 530, &p);
    CHECK(p.sequence == next_artnet_sequence(last) && p.sequence != 0);
    last = p.sequence;
  }
  CHECK(out->stats().packets == 302 && out->stats().errors == 0);

  // ArtPoll -> ArtPollReply, answered to the poller only.
  UdpSocket console;
  CHECK(console.open("127.0.0.1", 0, false));
  const std::uint8_t poll[14] = {'A', 'r', 't', '-', 'N', 'e', 't', 0, 0x00, 0x20, 0, 14, 0, 0};
  CHECK(console.send_to("127.0.0.1", c.listen_port, poll, 14) == 14);
  for (const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(10); out->stats().poll_replies == 0 && std::chrono::steady_clock::now() < dl;) { out->service(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  CHECK(out->stats().poll_replies == 1);
  const long n = recv_one(console, buf, sizeof buf);
  CHECK(n == 239 && buf[8] == 0x00 && buf[9] == 0x21 && buf[173] == 1);
  // Other packets are ignored.
  const std::uint8_t junk[14] = {1, 2, 3};
  console.send_to("127.0.0.1", c.listen_port, junk, 14);
  out->service();
  CHECK(out->stats().poll_replies == 1);
  out->close();
}

static void test_artnet_needs_destination() {
  ArtNetConfig c;
  c.enabled = true;  // no host: refuses to open instead of guessing a broadcast address
  c.listen_port = 0;
  auto out = make_artnet_output(c);
  std::string err;
  CHECK(!out->open(&err) && !err.empty());
}

static void test_sacn_loopback() {
  UdpSocket rx;
  CHECK(rx.open("127.0.0.1", 0, false));
  SacnConfig c;
  c.enabled = true;
  c.multicast = false;
  c.host = "127.0.0.1";
  c.port = rx.local_port();
  c.bind_ip = "127.0.0.1";
  c.priority = 150;
  c.source_name = "loopback";
  c.cid = "000102030405060708090a0b0c0d0e0f";
  auto out = make_sacn_output(c);
  CHECK(out->open(nullptr));
  UniverseFrame fr[2] = {frame(1, 1), frame(2, 100)};
  out->send(fr, 2);
  std::uint8_t buf[700];
  CHECK(recv_one(rx, buf, sizeof buf) == 638);
  CHECK(buf[18 + 3] == 4 && buf[22] == 0 && buf[37] == 15);            // data vector, CID
  CHECK(buf[108] == 150 && buf[111] == 0 && buf[113] == 0 && buf[114] == 1);
  CHECK(buf[126] == 1 && std::string(reinterpret_cast<char*>(buf + 44)) == "loopback");
  CHECK(recv_one(rx, buf, sizeof buf) == 638 && buf[114] == 2 && buf[111] == 0 && buf[126] == 100);
  // The per-universe sequence wraps from 255 to 0.
  std::uint8_t expect = 1;
  for (int i = 0; i < 260; ++i) {
    out->send(fr, 1);
    CHECK(recv_one(rx, buf, sizeof buf) == 638);
    CHECK(buf[111] == expect);
    ++expect;  // uint8_t arithmetic wraps like the wire value
  }
  CHECK(expect == 5);  // 1 + 260 mod 256
  // Closing sends the stream-terminated flag so receivers drop the source.
  out->close();
  int term = 0;
  std::uint8_t b2[700];
  while (recv_one(rx, b2, sizeof b2, nullptr, nullptr, 500) == 638) { if (b2[112] & 0x40) ++term; if (term >= 3) break; }
  CHECK(term >= 3);
}

static void test_sacn_sync() {
  UdpSocket rx;
  CHECK(rx.open("127.0.0.1", 0, false));
  SacnConfig c;
  c.enabled = true;
  c.multicast = false;
  c.host = "127.0.0.1";
  c.port = rx.local_port();
  c.bind_ip = "127.0.0.1";
  c.sync_universe = 7000;
  c.slots = 100;
  c.universe_offset = 10;
  auto out = make_sacn_output(c);
  CHECK(out->open(nullptr));
  UniverseFrame f = frame(1, 0);
  out->send(&f, 1);
  std::uint8_t buf[700];
  CHECK(recv_one(rx, buf, sizeof buf) == 126 + 100);
  CHECK(buf[109] == (7000 >> 8) && buf[110] == (7000 & 255) && buf[113] == 0 && buf[114] == 11);
  CHECK(recv_one(rx, buf, sizeof buf) == 49 && buf[21] == 8 && buf[45] == (7000 >> 8));
  out->close();
}

static void test_engine_outputs_off_by_default() {
  LightingEngine eng;
  LightingConfig cfg;
  cfg.artnet.host = "127.0.0.1";  // configured but not enabled
  cfg.artnet.enabled = false;
  cfg.patch = {{"a", "generic-rgb", 1, 1, {}, 0}};
  std::string err;
  CHECK(eng.configure(cfg, &err));
  CHECK(eng.start(&err, false));
  CHECK(eng.status().outputs.empty());  // nothing was opened
  eng.stop();
  // enabled = false master switch beats the per-output switches.
  cfg.artnet.enabled = true;
  cfg.enabled = false;
  CHECK(eng.configure(cfg, &err) && eng.start(&err, false) && eng.status().outputs.empty());
  eng.stop();

  // Enabled: frames go out at the refresh rate (injected clock) and are repeated as keep-alive when unchanged.
  UdpSocket rx;
  CHECK(rx.open("127.0.0.1", 0, false));
  cfg.enabled = true;
  cfg.artnet.host = "127.0.0.1";
  cfg.artnet.port = rx.local_port();
  cfg.artnet.bind_ip = "127.0.0.1";
  cfg.artnet.listen_port = free_port();
  CHECK(eng.configure(cfg, &err) && eng.start(&err, false));
  CHECK(eng.status().outputs.size() == 1 && eng.status().outputs[0].open);
  int sent = 0;
  for (int i = 0; i < 80; ++i) eng.tick(i * 0.025);  // two seconds, nothing playing: dark and unchanged
  sent = static_cast<int>(eng.status().frames_sent);
  CHECK(sent == 2 || sent == 3);  // first frame, then one per second of keep-alive, not 80
  CHECK(eng.status().frames_rendered == 80);
  eng.stop();  // sends a final dark frame
  std::uint8_t buf[700];
  int got = 0;
  while (recv_one(rx, buf, sizeof buf, nullptr, nullptr, 500) == 530 && got < 100) ++got;
  CHECK(got >= 3);
}

int main() {
  test_artnet_loopback();
  test_artnet_needs_destination();
  test_sacn_loopback();
  test_sacn_sync();
  test_engine_outputs_off_by_default();
  std::puts("test_lighting_net: ok");
  return 0;
}
