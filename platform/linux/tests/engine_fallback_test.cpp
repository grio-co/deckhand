// No sound card: Engine::start reports "No audio output" but keeps running on miniaudio's null backend, so the
// decks play and the clock advances; a later start() that still finds no device leaves the stream untouched.
// Skips (exit 0) when the machine really has an output (a sound server or card answers).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

#include "../../../tests/check.hpp"
#include "deckhand/audio/engine.hpp"

using namespace deckhand;

int main() {
  // An empty ALSA configuration: no "default" PCM, so no device can open even if the host has a card.
  const char* dir = std::getenv("TMPDIR");
  const std::string conf = std::string(dir && *dir ? dir : "/tmp") + "/dh-empty-alsa-" + std::to_string(getpid()) + ".conf";
  std::ofstream(conf) << "# empty\n";
  setenv("ALSA_CONFIG_PATH", conf.c_str(), 1);
  setenv("PULSE_SERVER", "unix:/nonexistent/dh-pulse", 1);

  Engine e;
  std::string error;
  OutputConfig oc;
  if (e.start(oc, error)) {
    std::printf("skipped: an audio output opened anyway\n");
    std::remove(conf.c_str());
    return 0;
  }
  CHECK(error == "No audio output");
  CHECK(!e.running());  // callers retry on this
  CHECK(e.device_name() == "No audio output");
  CHECK(e.sample_rate() > 0 && e.output_channels() >= 2);

  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = 44100;
  audio->frames = 44100 * 20;
  audio->pcm.assign(audio->frames * 2, 0.1f);
  build_waveform(*audio);
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->bpm = 120;
  t->grid.push_back({0, 120, 1});
  e.load(0, std::move(t));
  Command c;
  c.op = Op::PlayToggle;
  c.deck = 0;
  e.send(c);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  const double p1 = e.deck(0).position;
  CHECK(e.deck(0).loaded && e.deck(0).playing && p1 > 10000);

  CHECK(!e.start(oc, error) && error == "No audio output");  // retry: still nothing, stream untouched
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const double p2 = e.deck(0).position;
  CHECK(p2 > p1 && e.deck(0).playing);
  std::printf("fallback: deck advanced %.0f frames across a failed retry\n", p2 - p1);
  e.stop();
  std::remove(conf.c_str());
  return 0;
}
