// The Windows HID backend (platform/windows/hid_windows.cpp) without a device: enumeration with criteria that match
// nothing (so no interface is even queried, let alone opened), the refusal rules for keyboards and mice, matching, the
// device-path parser, and that every operation on an unknown device fails cleanly. Real hardware needs a real PC.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../../../tests/check.hpp"
#include "deckhand/midi/controller.hpp"

namespace deckhand {
bool hid_parse_device_path(const std::string& path, int& vid, int& pid, int& interface_number);  // hid_windows.cpp
}

using namespace deckhand;

int main() {
  // ---- the refusal rules (shared with the hub: a keyboard, keypad, mouse or pointer is never opened) -------------
  CHECK(hid_usage_refused(0x01, 0x06) && hid_usage_refused(0x01, 0x07));  // keyboard, keypad
  CHECK(hid_usage_refused(0x01, 0x01) && hid_usage_refused(0x01, 0x02));  // pointer, mouse
  CHECK(hid_usage_refused(0x07, 0x00) && hid_usage_refused(0x07, 0x05));  // the keyboard / keypad usage page
  CHECK(!hid_usage_refused(0x01, 0x04));                                   // joystick
  CHECK(!hid_usage_refused(0xFF00, 0x01) && !hid_usage_refused(0x0C, 0x01));  // vendor page, consumer control
  std::puts("refusal rules ok");

  // ---- matching ---------------------------------------------------------------------------------------------------
  HidDeviceInfo info;
  info.vid = 0x2B73;
  info.pid = 0x0019;
  info.usage_page = 0xFF00;
  info.usage = 1;
  info.interface_number = 3;
  HidMatch any;
  CHECK(hid_matches(any, info));
  HidMatch m;
  m.vid = 0x2B73;
  m.pid = 0x0019;
  CHECK(hid_matches(m, info));
  m.interface_number = 2;
  CHECK(!hid_matches(m, info));
  m.interface_number = 3;
  m.usage_page = 0xFF00;
  CHECK(hid_matches(m, info));
  m.vid = 0x1234;
  CHECK(!hid_matches(m, info));
  std::puts("matching ok");

  // ---- device paths -------------------------------------------------------------------------------------------------
  int vid = 0, pid = 0, mi = 0;
  CHECK(hid_parse_device_path("\\\\?\\HID#VID_046D&PID_C52B&MI_01#7&2a8b3c&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}", vid, pid, mi));
  CHECK(vid == 0x046D && pid == 0xC52B && mi == 1);
  CHECK(hid_parse_device_path("\\\\?\\hid#vid_2b73&pid_0019#8&1&0&0000#{x}", vid, pid, mi) && vid == 0x2B73 && pid == 0x19 && mi == -1);
  CHECK(!hid_parse_device_path("\\\\?\\hid#{no ids}", vid, pid, mi));
  CHECK(!hid_parse_device_path("", vid, pid, mi));
  std::puts("device paths ok");

  // ---- the backend: nothing matches, so nothing is touched ------------------------------------------------------------
  int received = 0, changes = 0;
  auto io = create_hid_io([&](const std::string&, int, const std::uint8_t*, std::size_t) { ++received; }, [&] { ++changes; });
  CHECK(io != nullptr);
  CHECK(io->devices().empty());  // no criteria yet: an empty list lists nothing
  io->set_matches({});
  HidMatch nothing;
  nothing.vid = 0xFFFF;  // reserved: no device has it
  nothing.pid = 0xFFFF;
  io->set_matches({nothing});
  std::this_thread::sleep_for(std::chrono::milliseconds(1300));  // at least one background scan (they run once a second)
  CHECK(io->devices().empty());
  CHECK(!io->open("\\\\?\\hid#vid_ffff&pid_ffff#none"));
  std::vector<std::uint8_t> out;
  const std::uint8_t byte = 0;
  CHECK(!io->send_output("nope", 0, &byte, 1));
  CHECK(!io->send_feature("nope", 0, &byte, 1));
  CHECK(!io->get_feature("nope", 0, out));
  io->close("nope");  // no effect, no crash
  CHECK(received == 0 && changes == 0);
  io.reset();  // joins the scan thread
  std::puts("backend ok: no criteria matched, nothing queried or opened, unknown ids fail cleanly");
  std::puts("test_windows_hid ok");
  return 0;
}
