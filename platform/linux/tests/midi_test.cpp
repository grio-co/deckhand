// ALSA sequencer integration: a fake controller (another sequencer client) is found, its messages arrive
// with its name, output reaches it, and hot-plug in both directions is announced. Skips (exit 0) when no
// sequencer is available (no /dev/snd/seq).
#include <alsa/asoundlib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "../../../tests/check.hpp"
#include "deckhand/midi/controller.hpp"

using namespace deckhand;
using namespace std::chrono_literals;

namespace {

struct Fake {
  snd_seq_t* seq = nullptr;
  int port = -1;
  explicit Fake(const char* name) {
    CHECK(snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) == 0);
    snd_seq_set_client_name(seq, name);
    port = snd_seq_create_simple_port(seq, name,
                                      SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE |
                                          SND_SEQ_PORT_CAP_SUBS_WRITE,
                                      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
    CHECK(port >= 0);
  }
  ~Fake() { if (seq) snd_seq_close(seq); }
  void note_on(int note, int vel) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_noteon(&ev, 0, note, vel);
    snd_seq_event_output_direct(seq, &ev);
  }
  void close() { snd_seq_close(seq); seq = nullptr; }
};

template <class F>
bool wait_for(F f, int ms = 3000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (f()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return f();
}

bool has(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v) if (x == s) return true;
  return false;
}

}  // namespace

int main() {
  snd_seq_t* probe = nullptr;
  if (snd_seq_open(&probe, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
    std::puts("test_linux_midi skipped: no ALSA sequencer");
    return 0;
  }
  snd_seq_close(probe);

  std::mutex m;
  std::string last_device;
  std::vector<std::uint8_t> last_bytes;
  std::atomic<int> messages{0}, changes{0};
  Fake dj("TestDJ-1");  // exists before DeckHand starts
  auto io = create_midi_io(
      [&](const std::string& device, const std::uint8_t* data, std::size_t n) {
        std::lock_guard<std::mutex> lock(m);
        last_device = device;
        last_bytes.assign(data, data + n);
        ++messages;
      },
      [&] { ++changes; });
  CHECK(io);
  CHECK(has(io->devices(), "TestDJ-1"));

  // Input: complete message with status byte, tagged with the device name.
  dj.note_on(60, 127);
  CHECK(wait_for([&] { return messages.load() >= 1; }));
  {
    std::lock_guard<std::mutex> lock(m);
    CHECK(last_device == "TestDJ-1");
    CHECK(last_bytes == (std::vector<std::uint8_t>{0x90, 60, 127}));
  }
  dj.note_on(61, 100);  // same status again: running status must not be used
  CHECK(wait_for([&] { return messages.load() >= 2; }));
  {
    std::lock_guard<std::mutex> lock(m);
    CHECK(last_bytes == (std::vector<std::uint8_t>{0x90, 61, 100}));
  }

  // Output (LEDs): a control change addressed by device name reaches the fake's port.
  const std::uint8_t cc[3] = {0xB0, 7, 99};
  io->send("TestDJ-1", cc, 3);
  snd_seq_event_t* ev = nullptr;
  bool got = false;
  for (int i = 0; i < 150 && !got; ++i) {
    if (snd_seq_event_input(dj.seq, &ev) >= 0 && ev && ev->type == SND_SEQ_EVENT_CONTROLLER) {
      CHECK(ev->data.control.param == 7 && ev->data.control.value == 99);
      got = true;
    } else {
      std::this_thread::sleep_for(20ms);
    }
  }
  CHECK(got);
  io->send("NoSuchDevice", cc, 3);  // ignored

  // Hot-plug: a device appearing and disappearing is announced and the device list follows.
  const int before = changes.load();
  {
    Fake late("TestDJ-2");
    CHECK(wait_for([&] { return changes.load() > before && has(io->devices(), "TestDJ-2"); }));
    late.note_on(10, 1);
    CHECK(wait_for([&] { std::lock_guard<std::mutex> lock(m); return last_device == "TestDJ-2"; }));
  }
  CHECK(wait_for([&] { return !has(io->devices(), "TestDJ-2"); }));
  CHECK(has(io->devices(), "TestDJ-1"));

  io.reset();  // joins the reader thread
  std::puts("test_linux_midi ok");
  return 0;
}
