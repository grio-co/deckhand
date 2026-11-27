// macOS Audio Unit backend (v2 component API through AudioToolbox; v3 units show up in the same enumeration and load
// through AudioComponentInstanceNew). Effects only: aufx and aumf.
#include "backend.hpp"

#if DH_PLUGINS_AU

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace deckhand::plugins {
namespace {

constexpr UInt32 kTypes[2] = {kAudioUnitType_Effect, kAudioUnitType_MusicEffect};

std::string cf_to_string(CFStringRef s) {
  if (!s) return {};
  const CFIndex len = CFStringGetLength(s);
  const CFIndex cap = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  std::string out((std::size_t)cap, '\0');
  if (!CFStringGetCString(s, out.data(), cap, kCFStringEncodingUTF8)) return {};
  out.resize(std::strlen(out.c_str()));
  return out;
}

std::string make_id(UInt32 t, UInt32 s, UInt32 m) {
  char b[64];
  std::snprintf(b, sizeof b, "au:%08x:%08x:%08x", (unsigned)t, (unsigned)s, (unsigned)m);
  return b;
}

const char* unit_name(UInt32 u) {
  switch (u) {
    case kAudioUnitParameterUnit_Generic: return "";
    case kAudioUnitParameterUnit_Indexed: return "index";
    case kAudioUnitParameterUnit_Boolean: return "boolean";
    case kAudioUnitParameterUnit_Percent: return "%";
    case kAudioUnitParameterUnit_Seconds: return "s";
    case kAudioUnitParameterUnit_SampleFrames: return "frames";
    case kAudioUnitParameterUnit_Phase: return "phase";
    case kAudioUnitParameterUnit_Rate: return "rate";
    case kAudioUnitParameterUnit_Hertz: return "Hz";
    case kAudioUnitParameterUnit_Cents: return "cents";
    case kAudioUnitParameterUnit_RelativeSemiTones: return "semitones";
    case kAudioUnitParameterUnit_MIDINoteNumber: return "note";
    case kAudioUnitParameterUnit_MIDIController: return "controller";
    case kAudioUnitParameterUnit_Decibels: return "dB";
    case kAudioUnitParameterUnit_LinearGain: return "gain";
    case kAudioUnitParameterUnit_Degrees: return "degrees";
    case kAudioUnitParameterUnit_EqualPowerCrossfade: return "crossfade";
    case kAudioUnitParameterUnit_MixerFaderCurve1: return "fader";
    case kAudioUnitParameterUnit_Pan: return "pan";
    case kAudioUnitParameterUnit_Meters: return "m";
    case kAudioUnitParameterUnit_AbsoluteCents: return "cents";
    case kAudioUnitParameterUnit_Octaves: return "octaves";
    case kAudioUnitParameterUnit_BPM: return "BPM";
    case kAudioUnitParameterUnit_Beats: return "beats";
    case kAudioUnitParameterUnit_Milliseconds: return "ms";
    case kAudioUnitParameterUnit_Ratio: return "ratio";
    case kAudioUnitParameterUnit_CustomUnit: return "custom";
    default: return "";
  }
}

AudioComponentDescription desc_of(const PluginInfo& i) {
  AudioComponentDescription d{};
  d.componentType = i.type_code;
  d.componentSubType = i.subtype_code;
  d.componentManufacturer = i.manufacturer_code;
  return d;
}

class AuInstance final : public PluginInstance {
 public:
  ~AuInstance() override {
    if (unit_) {
      if (inited_) AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
    }
  }

  bool open(const PluginInfo& info, double sr, int max_block, std::string* err) {
    info_ = info;
    sr_ = sr;
    max_block_ = max_block;
    auto fail = [&](const char* what, OSStatus st) {
      if (err) {
        char b[96];
        std::snprintf(b, sizeof b, "%s failed (OSStatus %d)", what, (int)st);
        *err = b;
      }
      return false;
    };
    AudioComponentDescription d = desc_of(info);
    AudioComponent comp = AudioComponentFindNext(nullptr, &d);
    if (!comp) {
      if (err) *err = "component not found";
      return false;
    }
    OSStatus st = AudioComponentInstanceNew(comp, &unit_);
    if (st != noErr || !unit_) return fail("AudioComponentInstanceNew", st);

    AudioStreamBasicDescription f{};
    f.mSampleRate = sr;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 4;
    f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    if ((st = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f)) != noErr)
      return fail("input stream format (stereo)", st);
    if ((st = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f)) != noErr)
      return fail("output stream format (stereo)", st);
    UInt32 mfs = (UInt32)max_block;
    AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mfs, sizeof mfs);
    AURenderCallbackStruct cb{&AuInstance::input_cb, this};
    if ((st = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb)) != noErr)
      return fail("render callback", st);
    if ((st = AudioUnitInitialize(unit_)) != noErr) return fail("AudioUnitInitialize", st);
    inited_ = true;

    in_buf_[0].assign((std::size_t)max_block, 0.0f);
    in_buf_[1].assign((std::size_t)max_block, 0.0f);
    abl_ = reinterpret_cast<AudioBufferList*>(abl_mem_);
    abl_->mNumberBuffers = 2;
    read_params();
    return true;
  }

  const PluginInfo& info() const override { return info_; }
  double sample_rate() const override { return sr_; }
  int max_block() const override { return max_block_; }

  void process(const float* const* in, float* const* out, int frames) override {
    if (frames <= 0) return;
    if (bypass_.load(std::memory_order_relaxed)) {
      for (int c = 0; c < 2; ++c)
        if (in[c] != out[c]) std::memmove(out[c], in[c], (std::size_t)frames * sizeof(float));
      return;
    }
    if (any_pending_.exchange(false, std::memory_order_acquire)) apply_pending();
    int done = 0;
    while (done < frames) {
      const int n = std::min(frames - done, max_block_);
      for (int c = 0; c < 2; ++c) std::memcpy(in_buf_[c].data(), in[c] + done, (std::size_t)n * sizeof(float));
      cur_n_ = n;
      for (int c = 0; c < 2; ++c) {
        abl_->mBuffers[c].mNumberChannels = 1;
        abl_->mBuffers[c].mDataByteSize = (UInt32)n * sizeof(float);
        abl_->mBuffers[c].mData = out[c] + done;
      }
      AudioUnitRenderActionFlags fl = 0;
      AudioTimeStamp ts{};
      ts.mSampleTime = sample_time_;
      ts.mFlags = kAudioTimeStampSampleTimeValid;
      const OSStatus st = AudioUnitRender(unit_, &fl, &ts, 0, (UInt32)n, abl_);
      if (st != noErr) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c) std::memcpy(out[c] + done, in_buf_[c].data(), (std::size_t)n * sizeof(float));
      } else {
        // The unit may have redirected its output to its own buffers (mData changed): copy back if so.
        for (int c = 0; c < 2; ++c)
          if (abl_->mBuffers[c].mData != out[c] + done)
            std::memcpy(out[c] + done, abl_->mBuffers[c].mData, (std::size_t)n * sizeof(float));
      }
      sample_time_ += n;
      done += n;
    }
  }

  void set_bypass(bool on) override { bypass_.store(on, std::memory_order_relaxed); }
  bool bypass() const override { return bypass_.load(std::memory_order_relaxed); }

  double tail_seconds() const override { return get_time_prop(kAudioUnitProperty_TailTime); }
  double latency_seconds() const override { return get_time_prop(kAudioUnitProperty_Latency); }
  int latency_frames() const override { return (int)std::lround(latency_seconds() * sr_); }
  std::uint64_t render_errors() const override { return errors_.load(std::memory_order_relaxed); }

  void reset() override {
    AudioUnitReset(unit_, kAudioUnitScope_Global, 0);
    sample_time_ = 0;
  }

  // ---- parameters ----------------------------------------------------------------------------------------------------
  const std::vector<ParamInfo>& params() const override { return params_; }
  int param_index(std::uint32_t id) const override {
    for (std::size_t i = 0; i < params_.size(); ++i)
      if (params_[i].id == id) return (int)i;
    return -1;
  }
  bool set_plain(std::uint32_t id, double v) override {
    const int i = param_index(id);
    if (i < 0 || params_[(std::size_t)i].read_only) return false;
    const ParamInfo& p = params_[(std::size_t)i];
    v = std::clamp(v, p.min, p.max);
    pend_val_[i].store((float)v, std::memory_order_relaxed);
    pend_flag_[i].store(1, std::memory_order_release);
    any_pending_.store(true, std::memory_order_release);
    return true;
  }
  bool set_norm(std::uint32_t id, double n) override {
    if (param_index(id) < 0) return false;
    return set_plain(id, to_plain(id, n));
  }
  double get_plain(std::uint32_t id) const override {
    const int i = param_index(id);
    if (i < 0) return 0;
    if (pend_flag_[i].load(std::memory_order_acquire)) return pend_val_[i].load(std::memory_order_relaxed);
    AudioUnitParameterValue v = 0;
    if (AudioUnitGetParameter(unit_, id, kAudioUnitScope_Global, 0, &v) != noErr) return params_[(std::size_t)i].def;
    return v;
  }
  double get_norm(std::uint32_t id) const override { return to_norm(id, get_plain(id)); }
  double to_norm(std::uint32_t id, double plain) const override {
    const int i = param_index(id);
    if (i < 0) return 0;
    const ParamInfo& p = params_[(std::size_t)i];
    if (p.max <= p.min) return 0;
    plain = std::clamp(plain, p.min, p.max);
    if (p.logarithmic && p.min > 0) return std::log(plain / p.min) / std::log(p.max / p.min);
    return (plain - p.min) / (p.max - p.min);
  }
  double to_plain(std::uint32_t id, double n) const override {
    const int i = param_index(id);
    if (i < 0) return 0;
    const ParamInfo& p = params_[(std::size_t)i];
    n = std::clamp(n, 0.0, 1.0);
    double v = (p.logarithmic && p.min > 0) ? p.min * std::pow(p.max / p.min, n) : p.min + n * (p.max - p.min);
    if (p.steps > 0 && p.unit == "index") v = std::round(v);
    if (p.steps == 2 && p.unit == "boolean") v = v >= 0.5 ? 1.0 : 0.0;
    return std::clamp(v, p.min, p.max);
  }
  std::string value_string(std::uint32_t id, double plain) const override {
    const int i = param_index(id);
    if (i < 0) return {};
    const ParamInfo& p = params_[(std::size_t)i];
    if (!p.value_strings.empty()) {
      const int k = (int)std::lround(plain - p.min);
      if (k >= 0 && k < (int)p.value_strings.size()) return p.value_strings[(std::size_t)k];
    }
    AudioUnitParameterValue v = (AudioUnitParameterValue)plain;
    AudioUnitParameterStringFromValue q{};
    q.inParamID = id;
    q.inValue = &v;
    q.outString = nullptr;
    UInt32 sz = sizeof q;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, 0, &q, &sz) == noErr &&
        q.outString) {
      std::string s = cf_to_string(q.outString);
      CFRelease(q.outString);
      if (!s.empty()) return s;
    }
    char b[48];
    std::snprintf(b, sizeof b, "%.4g", plain);
    return std::string(b) + (p.unit.empty() ? "" : " " + p.unit);
  }
  void flush_params() override {
    if (any_pending_.exchange(false, std::memory_order_acquire)) apply_pending();
  }

  // ---- state ---------------------------------------------------------------------------------------------------------
  bool save_state(std::vector<std::uint8_t>& out) override {
    flush_params();
    CFPropertyListRef cls = nullptr;
    UInt32 sz = sizeof cls;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &cls, &sz) != noErr || !cls)
      return false;
    CFDataRef data = CFPropertyListCreateData(nullptr, cls, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
    CFRelease(cls);
    if (!data) return false;
    out.assign(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + CFDataGetLength(data));
    CFRelease(data);
    return true;
  }
  bool restore_state(const std::uint8_t* d, std::size_t n) override {
    if (!d || n == 0) return false;
    CFDataRef data = CFDataCreate(nullptr, d, (CFIndex)n);
    if (!data) return false;
    CFPropertyListRef pl = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (!pl) return false;
    bool ok = false;
    if (CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
      ok = AudioUnitSetProperty(unit_, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, sizeof pl) == noErr;
    }
    CFRelease(pl);
    if (ok) {
      for (std::size_t i = 0; i < params_.size(); ++i) pend_flag_[i].store(0);
      any_pending_.store(false);
    }
    return ok;
  }
  std::vector<PresetInfo> presets() override {
    std::vector<PresetInfo> out;
    CFArrayRef arr = nullptr;
    UInt32 sz = sizeof arr;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &arr, &sz) != noErr || !arr)
      return out;
    for (CFIndex i = 0; i < CFArrayGetCount(arr); ++i) {
      const AUPreset* p = (const AUPreset*)CFArrayGetValueAtIndex(arr, i);
      if (p) out.push_back({(int)p->presetNumber, cf_to_string(p->presetName)});
    }
    CFRelease(arr);
    return out;
  }
  bool load_preset(int number) override {
    CFArrayRef arr = nullptr;
    UInt32 sz = sizeof arr;
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &arr, &sz) != noErr || !arr)
      return false;
    bool ok = false;
    for (CFIndex i = 0; i < CFArrayGetCount(arr) && !ok; ++i) {
      const AUPreset* p = (const AUPreset*)CFArrayGetValueAtIndex(arr, i);
      if (p && p->presetNumber == number) {
        AUPreset copy = *p;
        ok = AudioUnitSetProperty(unit_, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &copy, sizeof copy) == noErr;
      }
    }
    CFRelease(arr);
    for (std::size_t i = 0; i < params_.size(); ++i) pend_flag_[i].store(0);
    return ok;
  }

 private:
  static OSStatus input_cb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                           AudioBufferList* io) {
    auto* self = static_cast<AuInstance*>(ref);
    const UInt32 n = std::min<UInt32>(frames, (UInt32)self->cur_n_);
    for (UInt32 c = 0; c < io->mNumberBuffers; ++c) {
      const float* src = self->in_buf_[std::min<UInt32>(c, 1)].data();
      if (io->mBuffers[c].mData) std::memcpy(io->mBuffers[c].mData, src, n * sizeof(float));
      else io->mBuffers[c].mData = const_cast<float*>(src);
      io->mBuffers[c].mDataByteSize = n * sizeof(float);
    }
    return noErr;
  }

  double get_time_prop(AudioUnitPropertyID id) const {
    Float64 v = 0;
    UInt32 sz = sizeof v;
    if (AudioUnitGetProperty(unit_, id, kAudioUnitScope_Global, 0, &v, &sz) != noErr || !(v > 0)) return 0;
    return v;
  }

  void apply_pending() {
    for (std::size_t i = 0; i < params_.size(); ++i) {
      if (pend_flag_[i].exchange(0, std::memory_order_acq_rel))
        AudioUnitSetParameter(unit_, params_[i].id, kAudioUnitScope_Global, 0, pend_val_[i].load(std::memory_order_relaxed), 0);
    }
  }

  void read_params() {
    UInt32 sz = 0;
    Boolean w = 0;
    if (AudioUnitGetPropertyInfo(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &sz, &w) == noErr && sz) {
      std::vector<AudioUnitParameterID> ids(sz / sizeof(AudioUnitParameterID));
      if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &sz) == noErr) {
        for (AudioUnitParameterID pid : ids) {
          AudioUnitParameterInfo pi{};
          UInt32 psz = sizeof pi;
          if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, pid, &pi, &psz) != noErr) continue;
          ParamInfo p;
          p.id = pid;
          if ((pi.flags & kAudioUnitParameterFlag_HasCFNameString) && pi.cfNameString) {
            p.name = cf_to_string(pi.cfNameString);
            if (pi.flags & kAudioUnitParameterFlag_CFNameRelease) CFRelease(pi.cfNameString);
          } else {
            pi.name[sizeof pi.name - 1] = 0;
            p.name = pi.name;
          }
          p.unit = unit_name(pi.unit);
          if (pi.unit == kAudioUnitParameterUnit_CustomUnit && pi.unitName) p.unit = cf_to_string(pi.unitName);  // owned by the unit
          if (pi.unit == kAudioUnitParameterUnit_Indexed) p.unit = "index";
          if (pi.unit == kAudioUnitParameterUnit_Boolean) p.unit = "boolean";
          p.min = pi.minValue;
          p.max = pi.maxValue;
          p.def = pi.defaultValue;
          p.read_only = !(pi.flags & kAudioUnitParameterFlag_IsWritable);
          p.logarithmic = (pi.flags & kAudioUnitParameterFlag_DisplayLogarithmic) && p.min > 0;
          if (pi.unit == kAudioUnitParameterUnit_Indexed) {
            p.steps = (int)std::lround(p.max - p.min) + 1;
            CFArrayRef names = nullptr;
            UInt32 nsz = sizeof names;
            if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, pid, &names, &nsz) == noErr &&
                names) {
              for (CFIndex k = 0; k < CFArrayGetCount(names); ++k)
                p.value_strings.push_back(cf_to_string((CFStringRef)CFArrayGetValueAtIndex(names, k)));
              CFRelease(names);
            }
          } else if (pi.unit == kAudioUnitParameterUnit_Boolean) {
            p.steps = 2;
          }
          if (pi.clumpID) {
            char b[24];
            std::snprintf(b, sizeof b, "clump %u", (unsigned)pi.clumpID);
            p.group = b;
          }
          params_.push_back(std::move(p));
        }
      }
    }
    const std::size_t n = params_.size();
    pend_val_ = std::make_unique<std::atomic<float>[]>(n ? n : 1);
    pend_flag_ = std::make_unique<std::atomic<std::uint8_t>[]>(n ? n : 1);
    for (std::size_t i = 0; i < n; ++i) {
      pend_val_[i].store(0);
      pend_flag_[i].store(0);
    }
  }

  PluginInfo info_;
  AudioUnit unit_ = nullptr;
  bool inited_ = false;
  double sr_ = 48000;
  int max_block_ = 512;
  std::atomic<bool> bypass_{false};
  std::atomic<std::uint64_t> errors_{0};
  std::vector<float> in_buf_[2];
  int cur_n_ = 0;
  Float64 sample_time_ = 0;
  alignas(16) unsigned char abl_mem_[sizeof(AudioBufferList) + sizeof(AudioBuffer)] = {};
  AudioBufferList* abl_ = nullptr;
  std::vector<ParamInfo> params_;
  std::unique_ptr<std::atomic<float>[]> pend_val_;
  std::unique_ptr<std::atomic<std::uint8_t>[]> pend_flag_;
  std::atomic<bool> any_pending_{false};
};

}  // namespace

std::vector<PluginInfo> au_scan() {
  std::vector<PluginInfo> out;
  for (UInt32 type : kTypes) {
    AudioComponentDescription want{};
    want.componentType = type;
    AudioComponent c = nullptr;
    while ((c = AudioComponentFindNext(c, &want)) != nullptr) {
      AudioComponentDescription d{};
      if (AudioComponentGetDescription(c, &d) != noErr) continue;
      PluginInfo p;
      p.format = Format::AudioUnit;
      p.type_code = d.componentType;
      p.subtype_code = d.componentSubType;
      p.manufacturer_code = d.componentManufacturer;
      p.type = fourcc_text(p.type_code);
      p.subtype = fourcc_text(p.subtype_code);
      p.manufacturer = fourcc_text(p.manufacturer_code);
      p.id = make_id(p.type_code, p.subtype_code, p.manufacturer_code);
      p.has_midi = type == kAudioUnitType_MusicEffect;
      UInt32 ver = 0;
      AudioComponentGetVersion(c, &ver);
      p.version = ver;
      CFStringRef cn = nullptr;
      if (AudioComponentCopyName(c, &cn) == noErr && cn) {
        std::string full = cf_to_string(cn);
        CFRelease(cn);
        const std::size_t k = full.find(": ");
        if (k != std::string::npos) {
          p.maker = full.substr(0, k);
          p.name = full.substr(k + 2);
        } else {
          p.name = full;
        }
      }
      if (p.name.empty()) p.name = p.subtype;
      bool dup = false;
      for (const auto& o : out) dup = dup || o.id == p.id;
      if (!dup) out.push_back(std::move(p));
    }
  }
  return out;
}

std::string au_fingerprint() {
  char os[64] = {};
  std::size_t n = sizeof os - 1;
  sysctlbyname("kern.osversion", os, &n, nullptr, 0);
  UInt32 count = 0;
  for (UInt32 type : kTypes) {
    AudioComponentDescription want{};
    want.componentType = type;
    count += AudioComponentCount(&want);
  }
  char b[128];
  std::snprintf(b, sizeof b, "au1|%s|%u", os, (unsigned)count);
  return b;
}

std::unique_ptr<PluginInstance> au_open(const PluginInfo& info, double sr, int max_block, std::string* err) {
  if (info.type_code != kAudioUnitType_Effect && info.type_code != kAudioUnitType_MusicEffect) {
    if (err) *err = "not an effect unit";
    return nullptr;
  }
  auto inst = std::make_unique<AuInstance>();
  if (!inst->open(info, sr, max_block, err)) return nullptr;
  return inst;
}

}  // namespace deckhand::plugins

#endif  // DH_PLUGINS_AU
