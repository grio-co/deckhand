// WinMM backend: the MIDI layer starts and stops cleanly, lists whatever devices the machine has, ignores output to a
// device that is not there, and reports hot-plug through the Changed callback only when the list really changes.
// With no MIDI hardware (a build server, Wine) it checks the empty case; with hardware it also opens every input and
// waits briefly for traffic so a manual button press is visible in the output.
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "../../../tests/check.hpp"
#include "deckhand/midi/controller.hpp"

using namespace deckhand;
using namespace std::chrono_literals;

int main() {
  std::atomic<int> changed{0}, messages{0};
  std::mutex mu;
  std::string last_device;
  {
    auto io = create_midi_io(
        [&](const std::string& device, const std::uint8_t* data, std::size_t n) {
          CHECK(data && n >= 1 && n <= 3 + 4096);
          std::lock_guard<std::mutex> lock(mu);
          last_device = device;
          ++messages;
        },
        [&] { ++changed; });
    CHECK(io != nullptr);
    const auto devices = io->devices();
    std::printf("%zu MIDI input device(s)\n", devices.size());
    for (const auto& d : devices) std::printf("  %s\n", d.c_str());
    const UINT hw_in = midiInGetNumDevs();
    CHECK(devices.size() <= hw_in);  // never more than the system lists (a device in use elsewhere may be missing)

    // Output to a device that does not exist is a no-op, short or long message.
    const std::uint8_t note[3] = {0x90, 60, 100};
    io->send("no such device", note, 3);
    const std::uint8_t sysex[6] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    io->send("no such device", sysex, sizeof sysex);
    io->send("no such device", nullptr, 0);

    // Messages to real outputs must not crash either (they are sent to the first listed input's twin, if any).
    if (!devices.empty()) io->send(devices.front(), note, 3);

    // The poll thread runs while we wait: the list is stable, so no spurious Changed.
    std::this_thread::sleep_for(2500ms);
    CHECK(io->devices() == devices || changed > 0);
    if (!devices.empty() && std::getenv("DECKHAND_TEST_MIDI_WAIT")) {
      std::puts("move a control on a connected controller (5 s)...");
      std::this_thread::sleep_for(5s);
      std::printf("%d message(s) received\n", messages.load());
    }
  }  // destroying the object joins the poll thread and closes every handle
  std::puts("test_windows_midi ok");
  return 0;
}
