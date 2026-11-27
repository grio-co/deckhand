// Third-party audio effect plug-in hosting: format-neutral interface. This round has one backend (macOS Audio Units);
// a VST3 backend adds a Format value, a scan function and an open function behind the same two entry points
// (scan_installed / open_plugin) without touching callers.
//
// Threading: everything is non-real-time (open, params(), save_state, load_preset, reset, ...) except
// process(), set_*()/get_*() of parameters (any thread, lock free) and bypass. Never call two methods that are not
// marked thread-safe concurrently with process().
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace deckhand::plugins {

enum class Format : std::uint8_t { AudioUnit = 0, Vst3 = 1 /* reserved, not implemented */ };

struct PluginInfo {
  Format format = Format::AudioUnit;
  std::string id;            // stable, format-prefixed: "au:<type>:<subtype>:<manufacturer>" (hex codes)
  std::string name;          // "Delay"
  std::string maker;         // "Apple"
  std::string type, subtype, manufacturer;  // the four character codes as text ("aufx", "dely", "appl")
  std::uint32_t type_code = 0, subtype_code = 0, manufacturer_code = 0;
  std::uint32_t version = 0;  // as reported by the format (AU: 0xMMMMmmbb)
  bool has_midi = false;      // music effect (aumf): accepts MIDI too; audio only is hosted this round
  bool blocked = false;       // set by the catalog when the id is on the blocklist
};

struct ParamInfo {
  std::uint32_t id = 0;
  std::string name, unit, group;
  double min = 0, max = 1, def = 0;
  int steps = 0;                         // 0 = continuous; N >= 2 = N discrete values (min, min+1, ... for indexed)
  std::vector<std::string> value_strings;  // names of the discrete values (may be empty)
  bool read_only = false;
  bool logarithmic = false;  // normalised mapping is logarithmic (min must be > 0)
};

struct PresetInfo {
  int number = 0;
  std::string name;
};

class PluginInstance {
 public:
  virtual ~PluginInstance() = default;
  virtual const PluginInfo& info() const = 0;
  virtual double sample_rate() const = 0;
  virtual int max_block() const = 0;

  // Stereo in / out. `in` and `out` may alias (in place). Any frames count; blocks longer than max_block() are
  // split internally. No allocation, no locks. A plug-in render error passes the input through and bumps
  // render_errors().
  virtual void process(const float* const* in, float* const* out, int frames) = 0;
  virtual void set_bypass(bool on) = 0;  // thread-safe; bypassed = input copied to output, plug-in not run
  virtual bool bypass() const = 0;
  virtual double tail_seconds() const = 0;     // how long the plug-in rings after silence (0 = none)
  virtual double latency_seconds() const = 0;  // report only: the host does not compensate
  virtual int latency_frames() const = 0;
  virtual void reset() = 0;  // clears delay lines and tails; call off the audio thread or between blocks
  virtual std::uint64_t render_errors() const = 0;

  // ---- parameters -------------------------------------------------------------------------------------------------
  virtual const std::vector<ParamInfo>& params() const = 0;     // fixed after open
  virtual int param_index(std::uint32_t id) const = 0;          // -1 when unknown
  // Plain values are in the parameter's own unit (ParamInfo::min..max). set_* is thread-safe and lock free: the value
  // is queued and applied at the start of the next process() (or by flush_params()). get_* returns the queued value
  // when one is pending, else the plug-in's value.
  virtual bool set_plain(std::uint32_t id, double v) = 0;
  virtual bool set_norm(std::uint32_t id, double n) = 0;  // n in 0..1
  virtual double get_plain(std::uint32_t id) const = 0;
  virtual double get_norm(std::uint32_t id) const = 0;
  virtual double to_norm(std::uint32_t id, double plain) const = 0;
  virtual double to_plain(std::uint32_t id, double norm) const = 0;
  virtual std::string value_string(std::uint32_t id, double plain) const = 0;  // "250 ms", "Low Pass"; non-RT
  virtual void flush_params() = 0;  // apply queued values now (call when process() is not running)

  // ---- state and presets (non-RT) -------------------------------------------------------------------------------------
  // The blob is opaque to the caller (AU: a binary property list of the ClassInfo). It is only valid for the plug-in
  // that produced it. restore_state also re-reads all parameter values.
  virtual bool save_state(std::vector<std::uint8_t>& out) = 0;
  virtual bool restore_state(const std::uint8_t* data, std::size_t size) = 0;
  virtual std::vector<PresetInfo> presets() = 0;  // factory presets
  virtual bool load_preset(int number) = 0;
};

// Lists installed effect plug-ins without instantiating any of them (metadata only). Blocklist is not applied here.
std::vector<PluginInfo> scan_installed();
// A string that changes when the installed set (or the OS) may have changed; used to stamp the cache.
std::string scan_fingerprint();
// Opens a plug-in at `sample_rate`, stereo in / stereo out, rendering at most `max_block` frames at a time.
// Allocates (non-RT). nullptr on failure with *err set.
std::unique_ptr<PluginInstance> open_plugin(const PluginInfo& info, double sample_rate, int max_block, std::string* err = nullptr);
// True when this build can host plug-ins at all (false = stub: scan returns nothing, open fails).
bool hosting_supported();

}  // namespace deckhand::plugins
