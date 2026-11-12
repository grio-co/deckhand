// MIDI access through CoreMIDI (macOS and iOS). Every connected source is opened;
// messages are delivered with the source's display name.
#include <CoreMIDI/CoreMIDI.h>

#include <mutex>

#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"

// The packet-list API is the simplest one that suits plain MIDI 1.0 controllers.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace deckhand {
namespace {

std::string endpoint_name(MIDIEndpointRef ep) {
  CFStringRef name = nullptr;
  if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) != noErr || !name) return "MIDI device";
  char buf[256] = {0};
  CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
  CFRelease(name);
  return buf;
}

class AppleMidi final : public MidiIO {
 public:
  AppleMidi(Receive receive, Changed changed) : receive_(std::move(receive)), changed_(std::move(changed)) {
    if (MIDIClientCreate(CFSTR("DeckHand"), &AppleMidi::notify, this, &client_) != noErr) {
      DH_LOG(Error, "MIDI is unavailable (could not create a client)");
      return;
    }
    MIDIInputPortCreate(client_, CFSTR("DeckHand In"), &AppleMidi::read, this, &in_);
    MIDIOutputPortCreate(client_, CFSTR("DeckHand Out"), &out_);
    rescan();
  }
  ~AppleMidi() override {
    if (client_) MIDIClientDispose(client_);
  }

  std::vector<std::string> devices() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& s : sources_) names.push_back(s->name);
    return names;
  }

  void send(const std::string& device, const std::uint8_t* data, std::size_t size) override {
    MIDIEndpointRef dest = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& d : destinations_)
        if (d.name == device) dest = d.endpoint;
    }
    if (!dest || !out_ || size == 0 || size > 256) return;
    Byte buffer[512];
    MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(buffer);
    MIDIPacket* packet = MIDIPacketListInit(list);
    packet = MIDIPacketListAdd(list, sizeof buffer, packet, 0, static_cast<UInt16>(size), data);
    if (packet) MIDISend(out_, dest, list);
  }

 private:
  struct Source {
    MIDIEndpointRef endpoint;
    std::string name;
    AppleMidi* owner;
  };
  struct Destination {
    MIDIEndpointRef endpoint;
    std::string name;
  };

  void rescan() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sources_) {
      MIDIPortDisconnectSource(in_, s->endpoint);
      retired_.push_back(std::move(s));  // a read callback may still hold the pointer
    }
    sources_.clear();
    destinations_.clear();
    const ItemCount sources = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < sources; ++i) {
      const MIDIEndpointRef ep = MIDIGetSource(i);
      if (!ep) continue;
      auto s = std::make_unique<Source>(Source{ep, endpoint_name(ep), this});
      MIDIPortConnectSource(in_, ep, s.get());
      sources_.push_back(std::move(s));
    }
    const ItemCount dests = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < dests; ++i) {
      const MIDIEndpointRef ep = MIDIGetDestination(i);
      if (ep) destinations_.push_back({ep, endpoint_name(ep)});
    }
  }

  static void notify(const MIDINotification* message, void* self_ptr) {
    if (message->messageID != kMIDIMsgSetupChanged) return;
    auto* self = static_cast<AppleMidi*>(self_ptr);
    self->rescan();
    if (self->changed_) self->changed_();
  }

  static void read(const MIDIPacketList* list, void*, void* source_ptr) {
    const auto* source = static_cast<const Source*>(source_ptr);
    if (!source || !source->owner->receive_) return;
    const MIDIPacket* packet = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
      source->owner->receive_(source->name, packet->data, packet->length);
      packet = MIDIPacketNext(packet);
    }
  }

  Receive receive_;
  Changed changed_;
  MIDIClientRef client_ = 0;
  MIDIPortRef in_ = 0, out_ = 0;
  std::mutex mutex_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<std::unique_ptr<Source>> retired_;
  std::vector<Destination> destinations_;
};

}  // namespace

std::unique_ptr<MidiIO> create_midi_io(MidiIO::Receive receive, MidiIO::Changed changed) {
  return std::make_unique<AppleMidi>(std::move(receive), std::move(changed));
}

}  // namespace deckhand
