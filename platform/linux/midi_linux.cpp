// MIDI access through the ALSA sequencer. Every readable port of every other client is subscribed to
// one DeckHand input port; messages are delivered with the device's display name (the ALSA client
// name, plus the port name when the device has several ports). Hot-plug is followed through the
// sequencer's system announce port.
#include <alsa/asoundlib.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"

namespace deckhand {
namespace {

using Clock = std::chrono::steady_clock;

struct Endpoint {
  int client = 0, port = 0;
  std::string name;
};

class AlsaMidi final : public MidiIO {
 public:
  AlsaMidi(Receive receive, Changed changed) : receive_(std::move(receive)), changed_(std::move(changed)) {
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0 || !seq_) {
      seq_ = nullptr;
      DH_LOG(Error, "MIDI is unavailable (could not open the ALSA sequencer)");
      return;
    }
    snd_seq_set_client_name(seq_, "DeckHand");
    client_ = snd_seq_client_id(seq_);
    port_ = snd_seq_create_simple_port(seq_, "DeckHand",
                                       SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
                                           SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                       SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port_ < 0) {
      DH_LOG(Error, "MIDI is unavailable (could not create a port)");
      snd_seq_close(seq_);
      seq_ = nullptr;
      return;
    }
    snd_midi_event_new(4096, &decoder_);
    snd_midi_event_new(4096, &encoder_);
    if (decoder_) snd_midi_event_no_status(decoder_, 1);  // always deliver complete messages
    snd_seq_connect_from(seq_, port_, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    wake_[0] = wake_[1] = -1;
    if (pipe(wake_) != 0) wake_[0] = wake_[1] = -1;
    rescan();
    thread_ = std::thread([this] { run(); });
  }

  ~AlsaMidi() override {
    stop_ = true;
    if (wake_[1] >= 0) {
      const char c = 0;
      if (write(wake_[1], &c, 1) < 0) { /* the thread also wakes on its timeout */ }
    }
    if (thread_.joinable()) thread_.join();
    if (seq_) snd_seq_close(seq_);
    if (decoder_) snd_midi_event_free(decoder_);
    if (encoder_) snd_midi_event_free(encoder_);
    for (int fd : wake_)
      if (fd >= 0) close(fd);
  }

  std::vector<std::string> devices() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& s : sources_) names.push_back(s.name);
    return names;
  }

  void send(const std::string& device, const std::uint8_t* data, std::size_t size) override {
    if (!seq_ || !data || size == 0 || size > 4096) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const Endpoint* dest = nullptr;
    for (const auto& d : destinations_)
      if (d.name == device) dest = &d;
    if (!dest || !encoder_) return;
    snd_midi_event_reset_encode(encoder_);
    std::size_t at = 0;
    while (at < size) {
      snd_seq_event_t ev;
      snd_seq_ev_clear(&ev);
      const long used = snd_midi_event_encode(encoder_, data + at, static_cast<long>(size - at), &ev);
      if (used <= 0) break;
      at += static_cast<std::size_t>(used);
      if (ev.type == SND_SEQ_EVENT_NONE) continue;  // message not complete yet
      snd_seq_ev_set_source(&ev, port_);
      snd_seq_ev_set_dest(&ev, dest->client, dest->port);
      snd_seq_ev_set_direct(&ev);
      snd_seq_event_output_direct(seq_, &ev);
    }
  }

 private:
  // Ports of one kind (readable = sources, writable = destinations), named for display.
  std::vector<Endpoint> enumerate(bool input) {
    struct Raw {
      int client, port;
      std::string client_name, port_name;
    };
    std::vector<Raw> raw;
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    const unsigned need = input ? (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)
                                : (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
    while (snd_seq_query_next_client(seq_, ci) >= 0) {
      const int client = snd_seq_client_info_get_client(ci);
      if (client == SND_SEQ_CLIENT_SYSTEM || client == client_) continue;
      const std::string cname = snd_seq_client_info_get_name(ci);
      if (cname.rfind("Midi Through", 0) == 0) continue;
      snd_seq_port_info_set_client(pi, client);
      snd_seq_port_info_set_port(pi, -1);
      while (snd_seq_query_next_port(seq_, pi) >= 0) {
        const unsigned caps = snd_seq_port_info_get_capability(pi);
        if ((caps & need) != need || (caps & SND_SEQ_PORT_CAP_NO_EXPORT)) continue;
        if (!(snd_seq_port_info_get_type(pi) & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
        raw.push_back({client, snd_seq_port_info_get_port(pi), cname, snd_seq_port_info_get_name(pi)});
      }
    }
    std::vector<Endpoint> out;
    for (const auto& r : raw) {
      const bool several = std::count_if(raw.begin(), raw.end(), [&](const Raw& o) { return o.client == r.client; }) > 1;
      out.push_back({r.client, r.port, several ? r.client_name + " " + r.port_name : r.client_name});
    }
    return out;
  }

  void rescan() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Endpoint> now = enumerate(true);
    for (const auto& old : sources_) {
      const bool kept = std::any_of(now.begin(), now.end(),
                                    [&](const Endpoint& e) { return e.client == old.client && e.port == old.port; });
      if (!kept) snd_seq_disconnect_from(seq_, port_, old.client, old.port);
    }
    for (const auto& e : now) {
      const bool known = std::any_of(sources_.begin(), sources_.end(), [&](const Endpoint& o) {
        return o.client == e.client && o.port == e.port;
      });
      if (!known) snd_seq_connect_from(seq_, port_, e.client, e.port);
    }
    sources_ = std::move(now);
    destinations_ = enumerate(false);
  }

  void run() {
    const int count = snd_seq_poll_descriptors_count(seq_, POLLIN);
    std::vector<pollfd> fds(static_cast<std::size_t>(count) + 1);
    bool dirty = false;
    Clock::time_point rescan_at;
    while (!stop_) {
      snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(count), POLLIN);
      fds[static_cast<std::size_t>(count)] = {wake_[0], POLLIN, 0};
      int timeout = 1000;
      if (dirty) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(rescan_at - Clock::now()).count();
        timeout = static_cast<int>(std::max<long long>(0, left));
      }
      if (poll(fds.data(), fds.size(), timeout) < 0) continue;
      if (stop_) break;
      for (;;) {
        std::string name;
        std::uint8_t buffer[4096];
        long n = 0;
        bool more = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          snd_seq_event_t* ev = nullptr;
          more = snd_seq_event_input(seq_, &ev) >= 0 && ev;
          if (more) n = handle(*ev, buffer, sizeof buffer, name, dirty, rescan_at);
        }
        if (!more) break;
        if (n > 0 && receive_) receive_(name, buffer, static_cast<std::size_t>(n));  // unlocked: it may call send()
      }
      if (dirty && Clock::now() >= rescan_at) {
        dirty = false;
        rescan();
        if (changed_) changed_();
      }
    }
  }

  // Called with mutex_ held. Returns the byte count of a message to deliver (0 for none).
  long handle(const snd_seq_event_t& ev, std::uint8_t* buffer, std::size_t capacity, std::string& name, bool& dirty,
              Clock::time_point& rescan_at) {
    switch (ev.type) {
      case SND_SEQ_EVENT_PORT_START:
      case SND_SEQ_EVENT_PORT_EXIT:
      case SND_SEQ_EVENT_CLIENT_EXIT:
        if (ev.data.addr.client != client_) {
          dirty = true;
          rescan_at = Clock::now() + std::chrono::milliseconds(150);  // let a USB device finish registering
        }
        return 0;
      case SND_SEQ_EVENT_CLIENT_START:
      case SND_SEQ_EVENT_PORT_CHANGE:
      case SND_SEQ_EVENT_PORT_SUBSCRIBED:
      case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
      case SND_SEQ_EVENT_CLIENT_CHANGE:
        return 0;
      default:
        break;
    }
    for (const auto& s : sources_)
      if (s.client == ev.source.client && s.port == ev.source.port) name = s.name;
    if (name.empty() || !decoder_) return 0;
    const long n = snd_midi_event_decode(decoder_, buffer, static_cast<long>(capacity), &ev);
    return n > 0 ? n : 0;
  }

  Receive receive_;
  Changed changed_;
  snd_seq_t* seq_ = nullptr;
  snd_midi_event_t* decoder_ = nullptr;
  snd_midi_event_t* encoder_ = nullptr;
  int client_ = 0, port_ = -1;
  int wake_[2] = {-1, -1};
  std::mutex mutex_;
  std::vector<Endpoint> sources_, destinations_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<MidiIO> create_midi_io(MidiIO::Receive receive, MidiIO::Changed changed) {
  return std::make_unique<AlsaMidi>(std::move(receive), std::move(changed));
}

}  // namespace deckhand
