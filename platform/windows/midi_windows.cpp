// MIDI access through the Windows multimedia API (WinMM midiIn* / midiOut*), which every Windows 10/11 build offers
// and every class-compliant controller appears in. Inputs are opened as they appear and delivered with the device's
// display name; an output with the same name (if any) is opened next to it for send(). Hot-plug is followed by
// polling the device list once a second on a worker thread (WM_DEVICECHANGE needs a window, and the engine process
// has none).
//
// Limits, all of WinMM: names are cut to 31 characters by the driver interface, a device can usually be opened by one
// process at a time, and there is no BLE MIDI or MIDI 2.0. The WinRT MIDI APIs (Windows.Devices.Midi) and Windows
// MIDI Services would lift these; that is future work (docs/WINDOWS.md).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"

namespace deckhand {
namespace {

constexpr int kSysexBuffers = 4;
constexpr DWORD kSysexSize = 4096;

std::string narrow(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::size_t short_length(std::uint8_t status) {
  if (status >= 0xF8) return 1;
  switch (status & 0xF0) {
    case 0xC0:
    case 0xD0: return 2;
    case 0xF0:
      if (status == 0xF1 || status == 0xF3) return 2;
      if (status == 0xF2) return 3;
      return 1;
    default: return 3;
  }
}

class WinMidi;

struct Input {
  WinMidi* owner = nullptr;
  std::string name;
  HMIDIIN handle = nullptr;
  std::vector<std::unique_ptr<std::vector<char>>> storage;
  std::vector<MIDIHDR> headers;
};

class WinMidi final : public MidiIO {
 public:
  WinMidi(Receive receive, Changed changed) : receive_(std::move(receive)), changed_(std::move(changed)) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    rescan(false);
    thread_ = std::thread([this] { run(); });
  }

  ~WinMidi() override {
    stop_ = true;
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    std::map<std::string, std::unique_ptr<Input>> inputs;
    std::map<std::string, HMIDIOUT> outputs;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      inputs.swap(inputs_);
      outputs.swap(outputs_);
    }
    for (auto& kv : inputs) close_input(*kv.second);
    for (auto& kv : outputs) midiOutClose(kv.second);
    CloseHandle(wake_);
  }

  std::vector<std::string> devices() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return {seen_.begin(), seen_.end()};
  }

  void send(const std::string& device, const std::uint8_t* data, std::size_t size) override {
    if (!data || size == 0) return;
    HMIDIOUT out = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = outputs_.find(device);
      if (it == outputs_.end()) return;
      out = it->second;
    }
    // One MIDI message, or one system-exclusive dump, per call.
    if (data[0] == 0xF0) {
      std::vector<char> copy(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + size);
      MIDIHDR h{};
      h.lpData = copy.data();
      h.dwBufferLength = h.dwBytesRecorded = static_cast<DWORD>(copy.size());
      if (midiOutPrepareHeader(out, &h, sizeof h) != MMSYSERR_NOERROR) return;
      if (midiOutLongMsg(out, &h, sizeof h) == MMSYSERR_NOERROR)
        for (int i = 0; i < 500 && !(h.dwFlags & MHDR_DONE); ++i) Sleep(2);
      midiOutUnprepareHeader(out, &h, sizeof h);
      return;
    }
    const std::size_t n = std::min(size, short_length(data[0]));
    DWORD msg = 0;
    for (std::size_t i = 0; i < n; ++i) msg |= static_cast<DWORD>(data[i]) << (8 * i);
    midiOutShortMsg(out, msg);
  }

  // Called by WinMM on its own thread (see on_input).
  void on_short(Input* in, DWORD packed) {
    const std::uint8_t b[3] = {static_cast<std::uint8_t>(packed & 0xFF), static_cast<std::uint8_t>((packed >> 8) & 0xFF),
                               static_cast<std::uint8_t>((packed >> 16) & 0xFF)};
    if (b[0] < 0x80 || b[0] == 0xFE) return;  // active sensing floods the controller code with nothing
    if (receive_) receive_(in->name, b, short_length(b[0]));
  }

  void on_long(Input* in, MIDIHDR* h) {
    if (h->dwBytesRecorded > 0) {
      Pending p;
      p.in = in;
      p.hdr = h;
      p.bytes.assign(reinterpret_cast<const std::uint8_t*>(h->lpData), reinterpret_cast<const std::uint8_t*>(h->lpData) + h->dwBytesRecorded);
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(std::move(p));
      }
      SetEvent(wake_);
    }
  }

 private:
  struct Pending {
    Input* in = nullptr;
    MIDIHDR* hdr = nullptr;
    std::vector<std::uint8_t> bytes;
  };

  static void CALLBACK on_input(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
    auto* in = reinterpret_cast<Input*>(instance);
    if (!in || !in->owner) return;
    if (msg == MIM_DATA) in->owner->on_short(in, static_cast<DWORD>(p1));
    else if (msg == MIM_LONGDATA) in->owner->on_long(in, reinterpret_cast<MIDIHDR*>(p1));
  }

  void close_input(Input& in) {
    if (!in.handle) return;
    midiInReset(in.handle);  // returns the queued buffers
    for (MIDIHDR& h : in.headers) midiInUnprepareHeader(in.handle, &h, sizeof h);
    midiInClose(in.handle);
    in.handle = nullptr;
  }

  std::unique_ptr<Input> open_input(UINT id, const std::string& name) {
    auto in = std::make_unique<Input>();
    in->owner = this;
    in->name = name;
    const MMRESULT r = midiInOpen(&in->handle, id, reinterpret_cast<DWORD_PTR>(&on_input), reinterpret_cast<DWORD_PTR>(in.get()),
                                  CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) return nullptr;
    in->headers.resize(kSysexBuffers);
    for (int i = 0; i < kSysexBuffers; ++i) {
      in->storage.push_back(std::make_unique<std::vector<char>>(kSysexSize));
      MIDIHDR& h = in->headers[static_cast<std::size_t>(i)];
      h = MIDIHDR{};
      h.lpData = in->storage.back()->data();
      h.dwBufferLength = kSysexSize;
      if (midiInPrepareHeader(in->handle, &h, sizeof h) == MMSYSERR_NOERROR) midiInAddBuffer(in->handle, &h, sizeof h);
    }
    if (midiInStart(in->handle) != MMSYSERR_NOERROR) {
      close_input(*in);
      return nullptr;
    }
    return in;
  }

  // Display names, with a number appended to the second and later device of the same name.
  static std::vector<std::string> list_names(bool inputs) {
    const UINT n = inputs ? midiInGetNumDevs() : midiOutGetNumDevs();
    std::vector<std::string> names;
    std::map<std::string, int> count;
    for (UINT i = 0; i < n; ++i) {
      std::string name;
      if (inputs) {
        MIDIINCAPSW c{};
        if (midiInGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        name = narrow(c.szPname);
      } else {
        MIDIOUTCAPSW c{};
        if (midiOutGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        name = narrow(c.szPname);
      }
      const int k = ++count[name];
      names.push_back(k > 1 ? name + " #" + std::to_string(k) : name);
    }
    return names;
  }

  void rescan(bool announce) {
    const std::vector<std::string> in_names = list_names(true);
    const std::vector<std::string> out_names = list_names(false);
    const std::set<std::string> now(in_names.begin(), in_names.end());
    std::vector<std::unique_ptr<Input>> gone;
    std::vector<HMIDIOUT> gone_out;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (now != seen_) changed = true;
      seen_ = now;
      for (auto it = inputs_.begin(); it != inputs_.end();) {
        if (now.count(it->first)) {
          ++it;
          continue;
        }
        gone.push_back(std::move(it->second));
        it = inputs_.erase(it);
      }
      for (auto it = outputs_.begin(); it != outputs_.end();) {
        if (now.count(it->first)) {
          ++it;
          continue;
        }
        gone_out.push_back(it->second);
        it = outputs_.erase(it);
      }
    }
    for (auto& g : gone) close_input(*g);  // outside the lock: closing waits for callbacks that may call send()
    for (HMIDIOUT o : gone_out) midiOutClose(o);
    for (std::size_t i = 0; i < in_names.size(); ++i) {
      const std::string& name = in_names[i];
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inputs_.count(name)) continue;
      }
      std::unique_ptr<Input> in = open_input(static_cast<UINT>(index_of(true, name)), name);
      if (!in) {
        if (failed_.insert(name).second) DH_LOG(Warn, "MIDI input '%s' could not be opened (in use by another program?)", name.c_str());
        continue;
      }
      failed_.erase(name);
      std::lock_guard<std::mutex> lock(mutex_);
      inputs_[name] = std::move(in);
    }
    for (const std::string& name : out_names) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!now.count(name) || outputs_.count(name)) continue;
      }
      HMIDIOUT out = nullptr;
      if (midiOutOpen(&out, static_cast<UINT>(index_of(false, name)), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR || !out) continue;
      std::lock_guard<std::mutex> lock(mutex_);
      outputs_[name] = out;
    }
    if (changed && announce && changed_) changed_();
  }

  // Device id of the device that list_names() shows under `name`.
  static int index_of(bool inputs, const std::string& name) {
    const UINT n = inputs ? midiInGetNumDevs() : midiOutGetNumDevs();
    std::map<std::string, int> count;
    for (UINT i = 0; i < n; ++i) {
      std::string base;
      if (inputs) {
        MIDIINCAPSW c{};
        if (midiInGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        base = narrow(c.szPname);
      } else {
        MIDIOUTCAPSW c{};
        if (midiOutGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        base = narrow(c.szPname);
      }
      const int k = ++count[base];
      if ((k > 1 ? base + " #" + std::to_string(k) : base) == name) return static_cast<int>(i);
    }
    return -1;
  }

  void deliver_queue() {
    std::deque<Pending> q;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      q.swap(queue_);
    }
    for (Pending& p : q) {
      std::string name;
      bool alive = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : inputs_)
          if (kv.second.get() == p.in) {
            alive = true;
            name = p.in->name;
          }
      }
      if (!alive) continue;
      if (receive_) receive_(name, p.bytes.data(), p.bytes.size());
      p.hdr->dwBytesRecorded = 0;
      midiInAddBuffer(p.in->handle, p.hdr, sizeof *p.hdr);  // hand the buffer back for the next dump
    }
  }

  void run() {
    auto last = std::chrono::steady_clock::now();
    while (!stop_) {
      WaitForSingleObject(wake_, 250);
      if (stop_) break;
      deliver_queue();
      const auto now = std::chrono::steady_clock::now();
      if (now - last >= std::chrono::seconds(1)) {
        last = now;
        rescan(true);
      }
    }
  }

  Receive receive_;
  Changed changed_;
  HANDLE wake_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::set<std::string> seen_, failed_;
  std::map<std::string, std::unique_ptr<Input>> inputs_;
  std::map<std::string, HMIDIOUT> outputs_;
  std::mutex queue_mutex_;
  std::deque<Pending> queue_;
};

}  // namespace

std::unique_ptr<MidiIO> create_midi_io(MidiIO::Receive receive, MidiIO::Changed changed) {
  return std::make_unique<WinMidi>(std::move(receive), std::move(changed));
}

}  // namespace deckhand
