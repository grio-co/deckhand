// Lighting: packet encoders (byte exact), fixture profiles, patch validation, look mapping, config, refresh pacing.
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/broadcast/json.hpp"
#include "deckhand/lighting/engine.hpp"
#include "deckhand/lighting/fixture.hpp"
#include "deckhand/lighting/output.hpp"
#include "deckhand/lighting/packets.hpp"

using namespace deckhand::lighting;
using deckhand::broadcast::Json;

static bool same(const std::uint8_t* a, const std::vector<std::uint8_t>& e) { return std::memcmp(a, e.data(), e.size()) == 0; }

static void test_artnet_packets() {
  // ArtDmx, Port-Address 0x0123 (Net 1, SubUni 0x23), sequence 1, three slots padded to four (length is even).
  const std::uint8_t dmx[3] = {1, 2, 3};
  std::uint8_t buf[600];
  const std::size_t n = encode_artdmx(buf, 1, 0x0123, dmx, 3);
  const std::vector<std::uint8_t> want = {0x41, 0x72, 0x74, 0x2d, 0x4e, 0x65, 0x74, 0x00,  // "Art-Net\0"
                                          0x00, 0x50,                                      // OpDmx 0x5000, little endian
                                          0x00, 0x0e,                                      // protocol version 14
                                          0x01, 0x00,                                      // sequence, physical
                                          0x23, 0x01,                                      // SubUni, Net
                                          0x00, 0x04,                                      // length, big endian
                                          0x01, 0x02, 0x03, 0x00};
  CHECK(n == want.size());
  CHECK(same(buf, want));
  // Full universe: 18 + 512 bytes, length field 0x0200.
  std::uint8_t full[512];
  for (int i = 0; i < 512; ++i) full[i] = static_cast<std::uint8_t>(i);
  CHECK(encode_artdmx(buf, 255, 0x7fff, full, 512) == 530);
  CHECK(buf[12] == 255 && buf[14] == 0xff && buf[15] == 0x7f && buf[16] == 0x02 && buf[17] == 0x00 && buf[18 + 300] == (300 & 255));
  ArtDmxPacket pk;
  CHECK(decode_artdmx(buf, 530, &pk) && pk.port_address == 0x7fff && pk.length == 512 && pk.sequence == 255 && pk.data[511] == 255);
  CHECK(!decode_artdmx(buf, 20, &pk));
  // Sequence: 1..255 then back to 1 (0 means "not sequenced" and is never produced).
  CHECK(next_artnet_sequence(0) == 1 && next_artnet_sequence(1) == 2 && next_artnet_sequence(254) == 255 && next_artnet_sequence(255) == 1);

  const std::uint8_t poll[14] = {'A', 'r', 't', '-', 'N', 'e', 't', 0, 0x00, 0x20, 0, 14, 0, 0};
  CHECK(is_artpoll(poll, 14));
  std::uint8_t notpoll[14];
  std::memcpy(notpoll, poll, 14);
  notpoll[9] = 0x50;
  CHECK(!is_artpoll(notpoll, 14) && !is_artpoll(poll, 10));

  ArtPollReplyInfo info;
  info.ip[0] = 10; info.ip[1] = 0; info.ip[2] = 0; info.ip[3] = 7;
  info.short_name = "DH";
  info.long_name = "DeckHand test";
  info.output_universes = 2;
  info.first_universe_low_nibble = 3;
  info.port_address_net_sub = 0x0012;  // Net 1, Sub-Net 2
  const auto r = encode_artpollreply(info);
  CHECK(r.size() == 239);
  CHECK(r[0] == 'A' && r[6] == 't' && r[7] == 0);
  CHECK(r[8] == 0x00 && r[9] == 0x21);                              // OpPollReply 0x2100
  CHECK(r[10] == 10 && r[13] == 7);                                  // IP
  CHECK(r[14] == 0x36 && r[15] == 0x19);                             // port 6454, little endian
  CHECK(r[18] == 1 && r[19] == 2);                                   // NetSwitch, SubSwitch
  CHECK(std::strcmp(reinterpret_cast<const char*>(&r[26]), "DH") == 0);
  CHECK(std::strcmp(reinterpret_cast<const char*>(&r[44]), "DeckHand test") == 0);
  CHECK(r[172] == 0 && r[173] == 2);                                 // two ports
  CHECK(r[174] == 0x80 && r[175] == 0x80 && r[176] == 0);            // outputs, DMX512
  CHECK(r[190] == 3 && r[191] == 4);                                 // SwOut low nibbles
  CHECK(r[200] == 0 && r[207] == 10 && r[210] == 7);                 // style node, BindIp
}

static void test_sacn_packets() {
  E131Params p;
  for (int i = 0; i < 16; ++i) p.cid[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i + 1);
  p.source_name = "DeckHand";
  p.priority = 100;
  p.sequence = 7;
  p.universe = 1;
  const std::uint8_t dmx[4] = {0xaa, 0xbb, 0xcc, 0xdd};
  std::uint8_t buf[700];
  const std::size_t n = encode_e131_data(buf, p, dmx, 4);
  std::vector<std::uint8_t> want = {0x00, 0x10, 0x00, 0x00,                                                        // preamble, post-amble
                                    0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00,    // "ASC-E1.17"
                                    0x70, 0x72,                                                                  // root flags+length 114
                                    0x00, 0x00, 0x00, 0x04};                                                     // root vector: data
  for (int i = 1; i <= 16; ++i) want.push_back(static_cast<std::uint8_t>(i));                                    // CID
  want.insert(want.end(), {0x70, 0x5c, 0x00, 0x00, 0x00, 0x02});                                                 // framing: length 92, vector
  const std::string name = "DeckHand";
  for (std::size_t i = 0; i < 64; ++i) want.push_back(i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0);  // source name
  want.insert(want.end(), {100, 0x00, 0x00, 7, 0x00, 0x00, 0x01});                                              // priority, sync, seq, options, universe
  want.insert(want.end(), {0x70, 0x0f, 0x02, 0xa1, 0x00, 0x00, 0x00, 0x01, 0x00, 0x05, 0x00});                  // DMP, start code
  want.insert(want.end(), {0xaa, 0xbb, 0xcc, 0xdd});
  CHECK(n == want.size() && n == 130);
  CHECK(same(buf, want));
  // Full universe: 638 bytes and the well known lengths 0x726e / 0x7258 / 0x720b.
  std::uint8_t full[512] = {0};
  CHECK(encode_e131_data(buf, p, full, 512) == 638);
  CHECK(buf[16] == 0x72 && buf[17] == 0x6e && buf[38] == 0x72 && buf[39] == 0x58 && buf[115] == 0x72 && buf[116] == 0x0b);
  CHECK(buf[123] == 0x02 && buf[124] == 0x01);  // 513 property values
  // Options and sync address.
  p.stream_terminated = true; p.preview = true; p.force_sync = true; p.sync_address = 7999; p.universe = 300; p.priority = 250;
  encode_e131_data(buf, p, full, 24);
  CHECK(buf[108] == 200 && buf[109] == 0x1f && buf[110] == 0x3f && buf[112] == 0xe0 && buf[113] == 0x01 && buf[114] == 0x2c);

  std::uint8_t sy[64];
  CHECK(encode_e131_sync(sy, p.cid, 5, 0x1f3f) == 49);
  std::vector<std::uint8_t> sw = {0x00, 0x10, 0x00, 0x00, 0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00, 0x70, 0x21, 0x00, 0x00, 0x00, 0x08};
  for (int i = 1; i <= 16; ++i) sw.push_back(static_cast<std::uint8_t>(i));
  sw.insert(sw.end(), {0x70, 0x0b, 0x00, 0x00, 0x00, 0x01, 5, 0x1f, 0x3f, 0x00, 0x00});
  CHECK(same(sy, sw) && sw.size() == 49);

  CHECK(e131_multicast_address(1) == "239.255.0.1" && e131_multicast_address(0x1f3f) == "239.255.31.63");
  bool ok = false;
  const auto cid = cid_from_hex("000102030405060708090a0b0c0d0e0f", &ok);
  CHECK(ok && cid[0] == 0 && cid[15] == 15 && cid_to_hex(cid) == "000102030405060708090a0b0c0d0e0f");
  cid_from_hex("xyz", &ok);
  CHECK(!ok);

  std::uint8_t wf[600];
  const std::uint8_t two[2] = {9, 8};
  CHECK(encode_widget_frame(wf, two, 2) == 8);
  const std::vector<std::uint8_t> ww = {0x7e, 6, 3, 0, 0, 9, 8, 0xe7};
  CHECK(same(wf, ww));
}

static void test_profiles_and_patch() {
  CHECK(builtin_profiles().size() == 7);
  ProfileSet set = ProfileSet::with_builtins();
  CHECK(set.find("generic-rgb") && set.find("generic-rgb")->channel_count() == 3);
  CHECK(set.find("generic-moving-head")->pan16 && set.find("generic-moving-head")->has_wheel);

  Profile p;
  std::string err;
  CHECK(parse_profile_text(R"({"id":"x","name":"X","channels":[{"role":"dimmer","min":10,"max":200},{"role":"strobe","off":5,"slow":20,"fast":100,"hz_min":2,"hz_max":12}]})", &p, &err));
  CHECK(p.channels[0].min == 10 && p.channels[1].strobe_min == 20 && p.channels[1].hz_max == 12);
  CHECK(!parse_profile_text(R"({"id":"x","channels":[{"role":"flux"}]})", &p, &err) && err.find("role") != std::string::npos);
  CHECK(!parse_profile_text(R"({"id":"x","channels":[{"role":"red"},{"role":"red"}]})", &p, &err));
  CHECK(!parse_profile_text(R"({"id":"x","channels":[{"role":"pan_fine"}]})", &p, &err));
  CHECK(!parse_profile_text(R"({"id":"x","channels":[{"role":"dimmer","default":300}]})", &p, &err));
  CHECK(!parse_profile_text(R"({"channels":[{"role":"dimmer"}]})", &p, &err));
  CHECK(!parse_profile_text("nonsense", &p, &err));
  // Writer and reader agree.
  const Profile& mh = *set.find("generic-moving-head");
  Profile back;
  CHECK(parse_profile(profile_to_json(mh), &back, &err));
  CHECK(back.channel_count() == mh.channel_count() && back.channels[4].slots.size() == 8 && back.channels[6].role == Role::Strobe);

  std::vector<PatchEntry> patch = {{"a", "generic-rgb", 1, 1, {"front"}, 0}, {"b", "generic-rgb", 1, 4, {}, 1}};
  CHECK(!has_errors(validate_patch(patch, set)));
  patch[1].address = 3;  // 3..5 overlaps 1..3
  auto issues = validate_patch(patch, set);
  CHECK(has_errors(issues) && issues[0].message.find("overlaps") != std::string::npos);
  patch[1].address = 511;  // 511..513 does not fit
  CHECK(has_errors(validate_patch(patch, set)));
  patch[1].address = 0;
  CHECK(has_errors(validate_patch(patch, set)));
  patch[1].address = 4;
  patch[1].profile = "nope";
  CHECK(has_errors(validate_patch(patch, set)));
  patch[1].profile = "generic-rgb";
  patch[1].id = "a";
  CHECK(has_errors(validate_patch(patch, set)));
  patch[1].id = "b";
  patch[1].universe = 2;
  patch[1].address = 1;  // other universe: same address is fine
  CHECK(!has_errors(validate_patch(patch, set)));
  std::vector<PatchEntry> many;
  for (int u = 1; u <= 9; ++u) many.push_back({"f" + std::to_string(u), "generic-dimmer", u, 1, {}, u});
  CHECK(has_errors(validate_patch(many, set)));
  many.pop_back();
  CHECK(!has_errors(validate_patch(many, set)));

  CompiledPatch cp;
  std::vector<PatchEntry> three = {{"c", "generic-rgb", 3, 10, {"back", "left"}, 2}, {"a", "generic-dimmer", 1, 1, {}, 0}, {"b", "generic-rgb", 3, 1, {"front"}, 1}};
  CHECK(compile_patch(three, set, &cp, &issues));
  CHECK(cp.universes == std::vector<int>({1, 3}) && cp.fixtures.size() == 3 && cp.profiles.size() == 2);
  CHECK(cp.fixtures[1].u == 0.0f && cp.fixtures[2].u > 0.3f && cp.fixtures[0].u > cp.fixtures[2].u);  // by position, not patch order
  CHECK(cp.fixtures[0].tags == (kTagBack | kTagLeft) && cp.fixtures[0].universe_slot == 1 && cp.fixtures[0].address0 == 9);
  three[0].address = 2;
  CHECK(!compile_patch(three, set, &cp, &issues) && has_errors(issues));
}

static std::vector<std::uint8_t> mapped(const char* profile, const Look& l) {
  static const ProfileSet set = ProfileSet::with_builtins();
  const Profile* p = set.find(profile);
  CHECK(p != nullptr);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(p->channel_count()));
  map_look(*p, l, out.data());
  return out;
}

static void test_mapping() {
  Look l;
  l.r = 1; l.g = 0.5f; l.b = 0; l.intensity = 0.5f;
  auto rgb = mapped("generic-rgb", l);  // no dimmer: intensity scales the colour
  CHECK(rgb[0] == 128 && rgb[1] == 64 && rgb[2] == 0);
  // RGBW: the common part goes to the white emitter.
  Look w;
  w.r = w.g = w.b = 1; w.intensity = 1;
  auto rgbw = mapped("generic-rgbw", w);
  CHECK(rgbw[0] == 0 && rgbw[1] == 0 && rgbw[2] == 0 && rgbw[3] == 255);
  Look pink;
  pink.r = 1; pink.g = 0.4f; pink.b = 0.4f; pink.intensity = 1;
  rgbw = mapped("generic-rgbw", pink);
  CHECK(rgbw[0] == 153 && rgbw[1] == 0 && rgbw[2] == 0 && rgbw[3] == 102);
  // Dimmer only: brightest component of the colour times intensity.
  Look blue;
  blue.b = 1; blue.intensity = 0.5f;
  CHECK(mapped("generic-dimmer", blue)[0] == 128);
  Look black = blue;
  black.b = 0;
  CHECK(mapped("generic-dimmer", black)[0] == 0);
  // RGBA par with dimmer and strobe: dimmer carries intensity, orange goes to amber.
  Look orange;
  orange.r = 1; orange.g = 0.6f; orange.intensity = 0.8f;
  auto par = mapped("generic-rgba-par", orange);
  CHECK(par[0] == 204 && par[1] == 0 && par[2] == 0 && par[3] == 0 && par[4] == 255 && par[5] == 0);
  orange.strobe_hz = 10;  // hz 1..20 -> 16..240
  CHECK(mapped("generic-rgba-par", orange)[5] == 122);
  orange.strobe_hz = 0.5f;  // below the slowest the fixture does: no strobe
  CHECK(mapped("generic-rgba-par", orange)[5] == 0);
  orange.strobe_hz = 50;  // beyond the range: clamped to the fastest
  CHECK(mapped("generic-rgba-par", orange)[5] == 240);
  // UV follows blue.
  CHECK(mapped("generic-uv", blue)[0] == 128);
  // Moving head: 16-bit pan/tilt, colour wheel slot, dimmer, strobe, macro default.
  Look mh;
  mh.r = 1; mh.intensity = 1; mh.pan = 0.5f; mh.tilt = 1.0f;
  auto h = mapped("generic-moving-head", mh);
  CHECK(h[0] == 128 && h[1] == 0 && h[2] == 255 && h[3] == 255);
  CHECK(h[4] == 10 /* red slot */ && h[7] == 255 && h[6] == 0);
  mh.r = 0; mh.g = 0; mh.b = 1;
  CHECK(mapped("generic-moving-head", mh)[4] == 60);
  mh.r = mh.g = mh.b = 1;
  CHECK(mapped("generic-moving-head", mh)[4] == 0);  // white
  // A profile range limits a continuous channel.
  Profile p;
  std::string err;
  CHECK(parse_profile_text(R"({"id":"r","channels":[{"role":"dimmer","min":10,"max":200}]})", &p, &err));
  std::uint8_t b[1];
  Look full;
  full.r = full.g = full.b = 1; full.intensity = 1;
  map_look(p, full, b);
  CHECK(b[0] == 200);
  full.intensity = 0;
  map_look(p, full, b);
  CHECK(b[0] == 10);
}

static void test_config_and_pacer() {
  LightingConfig c;
  CHECK(!c.enabled && !c.artnet.enabled && !c.sacn.enabled && !c.serial.enabled);  // everything off by default
  c.enabled = true;
  c.artnet.enabled = true;
  c.artnet.host = "192.0.2.5";
  c.sacn.multicast = false;
  c.sacn.host = "192.0.2.6";
  c.sacn.priority = 120;
  c.show.master = 0.5f;
  c.show.strobe_max_hz = 6;
  c.show.palette.mode = PaletteMode::Key;
  c.show.palette.colours = {{1, 0, 0}, {0, 0, 1}};
  c.patch = {{"p1", "generic-rgbw", 2, 17, {"front", "left"}, 3}};
  const std::string text = config_to_json(c).dump();
  auto j = Json::parse(text);
  CHECK(j.has_value());
  LightingConfig d;
  std::string err;
  CHECK(config_from_json(*j, &d, &err));
  CHECK(config_to_json(d).dump() == text);
  CHECK(d.sacn.priority == 120 && d.show.palette.mode == PaletteMode::Key && d.patch[0].tags.size() == 2 && d.show.palette.colours.size() == 2);
  CHECK(!config_from_json(*Json::parse("[1]"), &d, &err));
  CHECK(!config_from_json(*Json::parse(R"({"show":{"palette":{"colours":["red"]}}})"), &d, &err));
  // Out of range values are clamped, not trusted.
  LightingConfig e;
  CHECK(config_from_json(*Json::parse(R"({"refresh_hz":9000,"show":{"strobe_max_hz":99,"master":4}})"), &e, &err));
  CHECK(e.refresh_hz == 60 && e.show.strobe_max_hz == 25.0f && e.show.master == 1.0f);

  // A bad patch is rejected and the old configuration stays.
  LightingEngine eng;
  LightingConfig bad;
  bad.patch = {{"a", "generic-rgb", 1, 1, {}, 0}, {"b", "generic-rgb", 1, 2, {}, 1}};
  CHECK(!eng.configure(bad, &err) && err.find("overlaps") != std::string::npos);
  CHECK(eng.configure(LightingConfig{}, &err));

  // Pacer with an injected clock: refresh at 40 Hz, no catch-up burst, keep-alive for unchanged frames.
  Pacer pc;
  pc.configure(40, 1.0);
  CHECK(pc.frame_due(0.0));
  CHECK(!pc.frame_due(0.010));
  CHECK(pc.frame_due(0.025));
  CHECK(!pc.frame_due(0.026));
  CHECK(pc.frame_due(3.0));    // after a stall: exactly one frame
  CHECK(!pc.frame_due(3.001));
  Pacer ka;
  ka.configure(40, 1.0);
  CHECK(ka.should_send(0.0, false));   // first frame always goes out (nothing was sent yet)
  CHECK(!ka.should_send(0.5, false));
  CHECK(ka.should_send(0.6, true));    // changed
  CHECK(!ka.should_send(1.5, false));  // 0.9 s since the last
  CHECK(ka.should_send(1.6, false));   // keep-alive after a full second
}

int main() {
  test_artnet_packets();
  test_sacn_packets();
  test_profiles_and_patch();
  test_mapping();
  test_config_and_pacer();
  std::puts("test_lighting: ok");
  return 0;
}
