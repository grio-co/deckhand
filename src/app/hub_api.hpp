#pragma once
// The controller hub as the app uses it: in-process (LocalHub over a ControllerHub) or living in the engine
// process (RemoteHub over RemoteEngine). Only what the UI needs: MIDI learn, the list of connected controllers and
// the audio interface a controller asks for.
#include <string>
#include <vector>

#include "deckhand/audio/remote_engine.hpp"
#include "deckhand/midi/controller.hpp"

namespace deckhand {

class HubApi {
 public:
  virtual ~HubApi() = default;
  virtual bool learn_begin(const std::string& action) = 0;
  virtual void learn_cancel() = 0;
  virtual LearnState learn_state() = 0;
  virtual std::string learn_result(bool finalize) = 0;
  virtual std::string learn_device() = 0;
  virtual std::string learn_error() = 0;
  virtual bool learn_commit() = 0;
  virtual std::vector<std::string> connected() = 0;
  virtual bool audio_hint(std::string& match, int& master, int& phones) = 0;
  // Per-controller settings (jog sensitivity, calibration, soft takeover, LEDs), mapping reload and choice. The defaults
  // do nothing (a hub that cannot).
  virtual bool set_options(const std::string& mapping, const DeviceOptions& o) { (void)mapping; (void)o; return false; }
  virtual double jog_ticks(const std::string& mapping) { (void)mapping; return 0; }
  virtual bool reload_mappings(const std::vector<Mapping>& mappings) { (void)mappings; return false; }
  virtual bool prefer_mappings(const std::vector<std::string>& names) { (void)names; return false; }
};

class LocalHub final : public HubApi {
 public:
  explicit LocalHub(ControllerHub& h) : h_(h) {}
  bool learn_begin(const std::string& a) override { return h_.learn_begin(a); }
  void learn_cancel() override { h_.learn_cancel(); }
  LearnState learn_state() override { return h_.learn_state(); }
  std::string learn_result(bool f) override { return h_.learn_result(f); }
  std::string learn_device() override { return h_.learn_device(); }
  std::string learn_error() override { return h_.learn_error(); }
  bool learn_commit() override { return h_.learn_commit(); }
  std::vector<std::string> connected() override { return h_.connected(); }
  bool audio_hint(std::string& m, int& a, int& b) override { return h_.audio_hint(m, a, b); }
  bool set_options(const std::string& mapping, const DeviceOptions& o) override { h_.set_device_options(mapping, o); return true; }
  double jog_ticks(const std::string& mapping) override { return h_.last_jog_ticks(mapping); }
  bool reload_mappings(const std::vector<Mapping>& mappings) override { h_.reload_mappings(mappings); return true; }
  bool prefer_mappings(const std::vector<std::string>& names) override { h_.prefer_mappings(names); return true; }

 private:
  ControllerHub& h_;
};

class RemoteHub final : public HubApi {
 public:
  explicit RemoteHub(RemoteEngine& e) : e_(e) {}
  bool learn_begin(const std::string& a) override {
    begin_error_.clear();
    return e_.hub_learn_begin(a, begin_error_);
  }
  void learn_cancel() override { e_.hub_learn_cancel(); }
  LearnState learn_state() override { return static_cast<LearnState>(e_.hub_learn_state()); }
  std::string learn_result(bool f) override { return e_.hub_learn_result(f); }
  std::string learn_device() override { return e_.hub_learn_device(); }
  std::string learn_error() override {
    const std::string s = e_.hub_learn_error();
    return s.empty() ? begin_error_ : s;
  }
  bool learn_commit() override { return e_.hub_learn_commit(); }
  std::vector<std::string> connected() override { return e_.hub_connected(); }
  bool audio_hint(std::string& m, int& a, int& b) override { return e_.hub_audio_hint(m, a, b); }
  bool set_options(const std::string& mapping, const DeviceOptions& o) override {
    return e_.hub_set_options(mapping, o.jog_scale, o.ticks_per_rev, o.pickup, o.leds);
  }
  double jog_ticks(const std::string& mapping) override { return e_.hub_jog_ticks(mapping); }
  bool reload_mappings(const std::vector<Mapping>&) override { return e_.hub_reload_mappings(); }  // the engine process reads its own folders
  bool prefer_mappings(const std::vector<std::string>& names) override { return e_.hub_prefer_mappings(names); }

 private:
  RemoteEngine& e_;
  std::string begin_error_;
};

}  // namespace deckhand
