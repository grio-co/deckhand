// HID access through IOKit's HIDManager (macOS). iOS has no IOHID access: create_hid_io returns nullptr there.
//
// Safety: the manager is never opened (so nothing is seized or read), it only lists devices matching the
// criteria the hub gave it (nothing at all while there are none), and a device is opened individually, with
// kIOHIDOptionsTypeNone (never seize), only when the hub asks and its usage is not a keyboard, mouse or pointer.
#include <TargetConditionals.h>

#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"

#if TARGET_OS_IPHONE

namespace deckhand {
std::unique_ptr<HidIO> create_hid_io(HidIO::Receive, HidIO::Changed) { return nullptr; }
}  // namespace deckhand

#else

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDLib.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace deckhand {
namespace {

int int_property(IOHIDDeviceRef dev, CFStringRef key, int fallback) {
  CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
  int out = fallback;
  if (v && CFGetTypeID(v) == CFNumberGetTypeID() && !CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &out)) out = fallback;
  return out;
}

std::string string_property(IOHIDDeviceRef dev, CFStringRef key) {
  CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
  char buf[256] = {0};
  CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8);
  return buf;
}

CFNumberRef number(int v) { return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v); }

class AppleHid final : public HidIO {
 public:
  AppleHid(Receive receive, Changed changed) : receive_(std::move(receive)), changed_(std::move(changed)) {
    thread_ = std::thread([this] { run(); });
    std::unique_lock<std::mutex> lock(mutex_);
    ready_cv_.wait(lock, [this] { return ready_; });
  }
  ~AppleHid() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    if (loop_) {
      CFRunLoopStop(loop_);
      CFRunLoopWakeUp(loop_);
    }
    if (thread_.joinable()) thread_.join();
  }

  void set_matches(const std::vector<HidMatch>& matches) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      matches_ = matches;
    }
    on_loop([this] { apply_matches(); });
  }

  std::vector<HidDeviceInfo> devices() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HidDeviceInfo> out;
    for (const auto& d : devs_)
      if (wanted_locked(d->info)) out.push_back(d->info);
    return out;
  }

  bool open(const std::string& id) override {
    std::shared_ptr<Dev> d = find(id);
    if (!d || !wanted(d->info) || hid_usage_refused(d->info.usage_page, d->info.usage)) return false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (d->opened) return true;
    }
    if (IOHIDDeviceOpen(d->ref, kIOHIDOptionsTypeNone) != kIOReturnSuccess) return false;  // never seize
    int size = int_property(d->ref, CFSTR(kIOHIDMaxInputReportSizeKey), 64);
    size = std::max(8, std::min(size + 8, 4096));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      d->buf.assign(static_cast<std::size_t>(size), 0);
      d->opened = true;
    }
    on_loop([this, d] {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!d->opened || d->scheduled) return;
      IOHIDDeviceRegisterInputReportCallback(d->ref, d->buf.data(), static_cast<CFIndex>(d->buf.size()), &AppleHid::report, d.get());
      IOHIDDeviceScheduleWithRunLoop(d->ref, loop_, kCFRunLoopDefaultMode);
      d->scheduled = true;
    });
    return true;
  }

  void close(const std::string& id) override {
    std::shared_ptr<Dev> d = find(id);
    if (!d) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!d->opened) return;
      d->opened = false;  // input reports are dropped from now on
    }
    on_loop([this, d] { release(*d); });
  }

  bool send_output(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size) override {
    return set_report(id, kIOHIDReportTypeOutput, report_id, data, size);
  }
  bool send_feature(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size) override {
    return set_report(id, kIOHIDReportTypeFeature, report_id, data, size);
  }

  bool get_feature(const std::string& id, int report_id, std::vector<std::uint8_t>& out) override {
    std::shared_ptr<Dev> d = find(id);
    if (!d || !is_open(*d) || report_id < 0 || report_id > 255) return false;
    std::vector<std::uint8_t> buf(512, 0);
    CFIndex len = static_cast<CFIndex>(buf.size());
    if (IOHIDDeviceGetReport(d->ref, kIOHIDReportTypeFeature, report_id, buf.data(), &len) != kIOReturnSuccess || len < 0) return false;
    std::size_t from = (report_id != 0 && len > 0 && buf[0] == report_id) ? 1 : 0;
    out.assign(buf.begin() + static_cast<std::ptrdiff_t>(from), buf.begin() + len);
    return true;
  }

 private:
  struct Dev {
    IOHIDDeviceRef ref = nullptr;
    HidDeviceInfo info;
    std::vector<std::uint8_t> buf;
    bool opened = false, scheduled = false;
    AppleHid* owner = nullptr;
  };

  void run() {
    loop_ = CFRunLoopGetCurrent();
    manager_ = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (manager_) {
      IOHIDManagerRegisterDeviceMatchingCallback(manager_, &AppleHid::matched, this);
      IOHIDManagerRegisterDeviceRemovalCallback(manager_, &AppleHid::removed, this);
      apply_matches();  // (the manager itself is not opened; that would open every matching device)
      IOHIDManagerScheduleWithRunLoop(manager_, loop_, kCFRunLoopDefaultMode);
    } else {
      DH_LOG(Error, "HID is unavailable (could not create a manager)");
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_ = true;
    }
    ready_cv_.notify_all();
    // A run loop with no source returns at once: keep it alive with a long timer.
    CFRunLoopTimerRef keep = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0, [](CFRunLoopTimerRef, void*) {}, nullptr);
    CFRunLoopAddTimer(loop_, keep, kCFRunLoopDefaultMode);
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) break;
      }
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
    }
    CFRunLoopRemoveTimer(loop_, keep, kCFRunLoopDefaultMode);
    CFRelease(keep);
    std::vector<std::shared_ptr<Dev>> all;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      all = devs_;
      for (auto& d : all) d->opened = false;
    }
    for (auto& d : all) release(*d);
    if (manager_) {
      IOHIDManagerUnscheduleFromRunLoop(manager_, loop_, kCFRunLoopDefaultMode);
      CFRelease(manager_);
    }
    for (auto& d : all) CFRelease(d->ref);
  }

  template <class F>
  void on_loop(F f) {
    if (!loop_) return;
    CFRunLoopPerformBlock(loop_, kCFRunLoopDefaultMode, ^{ f(); });
    CFRunLoopWakeUp(loop_);
  }

  // Matching dictionaries from the hub's criteria. An empty list must match nothing (NULL would match all).
  void apply_matches() {
    std::vector<HidMatch> matches;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      matches = matches_;
    }
    CFMutableArrayRef list = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    for (const HidMatch& m : matches) {
      CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      const auto put = [&](CFStringRef key, int v) {
        if (v < 0) return;
        CFNumberRef n = number(v);
        CFDictionarySetValue(d, key, n);
        CFRelease(n);
      };
      put(CFSTR(kIOHIDVendorIDKey), m.vid);
      put(CFSTR(kIOHIDProductIDKey), m.pid);
      put(CFSTR(kIOHIDPrimaryUsagePageKey), m.usage_page);
      put(CFSTR(kIOHIDPrimaryUsageKey), m.usage);
      if (CFDictionaryGetCount(d) == 0) CFDictionarySetValue(d, CFSTR("DeckHandNoMatch"), kCFBooleanTrue);  // never match all
      CFArrayAppendValue(list, d);
      CFRelease(d);
    }
    if (matches.empty()) {
      CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CFDictionarySetValue(d, CFSTR("DeckHandNoMatch"), kCFBooleanTrue);
      CFArrayAppendValue(list, d);
      CFRelease(d);
    }
    if (manager_) IOHIDManagerSetDeviceMatchingMultiple(manager_, list);
    CFRelease(list);
  }

  bool wanted_locked(const HidDeviceInfo& info) const {
    for (const HidMatch& m : matches_)
      if (hid_matches(m, info)) return true;
    return false;
  }
  bool wanted(const HidDeviceInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    return wanted_locked(info);
  }

  std::shared_ptr<Dev> find(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : devs_)
      if (d->info.id == id) return d;
    return nullptr;
  }

  bool is_open(Dev& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    return d.opened;
  }

  bool set_report(const std::string& id, IOHIDReportType type, int report_id, const std::uint8_t* data, std::size_t size) {
    std::shared_ptr<Dev> d = find(id);
    if (!d || !is_open(*d) || report_id < 0 || report_id > 255 || (!data && size) || size > 4095) return false;
    std::vector<std::uint8_t> buf;
    if (report_id != 0) buf.push_back(static_cast<std::uint8_t>(report_id));  // numbered reports carry the id first
    buf.insert(buf.end(), data, data + size);
    if (buf.empty()) return false;
    return IOHIDDeviceSetReport(d->ref, type, report_id, buf.data(), static_cast<CFIndex>(buf.size())) == kIOReturnSuccess;
  }

  void release(Dev& d) {  // on the loop thread
    bool was_scheduled;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      was_scheduled = d.scheduled;
      d.scheduled = false;
    }
    if (was_scheduled) IOHIDDeviceUnscheduleFromRunLoop(d.ref, loop_, kCFRunLoopDefaultMode);
    IOHIDDeviceClose(d.ref, kIOHIDOptionsTypeNone);
  }

  static void matched(void* self_ptr, IOReturn, void*, IOHIDDeviceRef ref) {
    auto* self = static_cast<AppleHid*>(self_ptr);
    auto d = std::make_shared<Dev>();
    d->ref = ref;
    d->owner = self;
    HidDeviceInfo& i = d->info;
    i.vid = int_property(ref, CFSTR(kIOHIDVendorIDKey), 0);
    i.pid = int_property(ref, CFSTR(kIOHIDProductIDKey), 0);
    i.usage_page = int_property(ref, CFSTR(kIOHIDPrimaryUsagePageKey), 0);
    i.usage = int_property(ref, CFSTR(kIOHIDPrimaryUsageKey), 0);
    i.interface_number = int_property(ref, CFSTR("bInterfaceNumber"), -1);
    i.product = string_property(ref, CFSTR(kIOHIDProductKey));
    i.manufacturer = string_property(ref, CFSTR(kIOHIDManufacturerKey));
    char id[80];
    std::snprintf(id, sizeof id, "mac:%08X:%04X:%04X:%X:%X", static_cast<unsigned>(int_property(ref, CFSTR(kIOHIDLocationIDKey), 0)),
                  i.vid, i.pid, i.usage_page, i.usage);
    i.id = id;
    CFRetain(ref);
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->devs_.push_back(std::move(d));
    }
    if (self->changed_) self->changed_();
  }

  static void removed(void* self_ptr, IOReturn, void*, IOHIDDeviceRef ref) {
    auto* self = static_cast<AppleHid*>(self_ptr);
    std::shared_ptr<Dev> gone;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      for (auto it = self->devs_.begin(); it != self->devs_.end(); ++it)
        if ((*it)->ref == ref) {
          gone = *it;
          self->devs_.erase(it);
          break;
        }
      if (gone) gone->opened = false;
    }
    if (!gone) return;
    self->release(*gone);
    CFRelease(gone->ref);
    if (self->changed_) self->changed_();
  }

  static void report(void* ctx, IOReturn, void*, IOHIDReportType type, uint32_t id, uint8_t* data, CFIndex len) {
    auto* d = static_cast<Dev*>(ctx);
    if (type != kIOHIDReportTypeInput || !d || len < 0) return;
    std::string device;
    {
      std::lock_guard<std::mutex> lock(d->owner->mutex_);
      if (!d->opened) return;
      device = d->info.id;
    }
    // Numbered reports arrive with the id as their first byte; the hub wants the payload only.
    // (Unverified on hardware: if a device's first data byte equals its report id, nothing is lost only when the id is present.)
    std::size_t from = (id != 0 && len > 0 && data[0] == id) ? 1 : 0;
    if (d->owner->receive_) d->owner->receive_(device, static_cast<int>(id), data + from, static_cast<std::size_t>(len) - from);
  }

  Receive receive_;
  Changed changed_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_cv_;
  bool ready_ = false, stopping_ = false;
  CFRunLoopRef loop_ = nullptr;
  IOHIDManagerRef manager_ = nullptr;
  std::vector<HidMatch> matches_;
  std::vector<std::shared_ptr<Dev>> devs_;
};

}  // namespace

std::unique_ptr<HidIO> create_hid_io(HidIO::Receive receive, HidIO::Changed changed) {
  return std::make_unique<AppleHid>(std::move(receive), std::move(changed));
}

}  // namespace deckhand

#endif  // TARGET_OS_IPHONE
