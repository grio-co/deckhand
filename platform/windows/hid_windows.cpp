// HID access on Windows: SetupAPI lists the HID interfaces, hid.dll (HidD_*, HidP_GetCaps) describes them,
// and a reader thread per opened device does overlapped ReadFile. Hot-plug is followed by polling the list
// once a second (no window is needed, so it works in the engine process).
//
// Safety: only interfaces whose vid/pid some mapping names are queried at all (read-only, with zero access
// rights, which does not touch the input stream), and a device is opened for I/O only when the hub asks and
// its usage is not a keyboard, mouse or pointer.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"

namespace deckhand {
namespace {

std::string utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<std::size_t>(std::max(n, 1) - 1), L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

// "vid_046d" / "pid_c52b" / "mi_01" out of a lower-cased device path (no I/O needed).
bool path_field(const std::string& path, const char* key, int base, int& out) {
  const auto at = path.find(key);
  if (at == std::string::npos) return false;
  out = static_cast<int>(std::strtol(path.c_str() + at + std::strlen(key), nullptr, base));
  return true;
}

class WinHid final : public HidIO {
 public:
  WinHid(Receive receive, Changed changed) : receive_(std::move(receive)), changed_(std::move(changed)) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
  }
  ~WinHid() override {
    stop_ = true;
    if (wake_) SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    std::vector<std::shared_ptr<Dev>> all;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      all = devs_;
      all.insert(all.end(), zombies_.begin(), zombies_.end());
    }
    for (auto& d : all) stop_reader(*d);
    for (auto& d : all) join(*d);
    if (wake_) CloseHandle(wake_);
  }

  void set_matches(const std::vector<HidMatch>& matches) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      matches_ = matches;
      rescan_ = true;
    }
    if (wake_) SetEvent(wake_);
  }

  std::vector<HidDeviceInfo> devices() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HidDeviceInfo> out;
    for (const auto& d : devs_) out.push_back(d->info);
    return out;
  }

  bool open(const std::string& id) override {
    std::shared_ptr<Dev> d = find(id);
    if (!d || hid_usage_refused(d->info.usage_page, d->info.usage)) return false;
    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->handle != INVALID_HANDLE_VALUE) return true;
    if (d->thread.joinable()) {
      if (!d->done) return false;  // still shutting down from a close: try again on the next change
      d->thread.join();
    }
    d->done = false;
    HANDLE h = CreateFileW(wide(d->path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      h = CreateFileW(wide(d->path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    d->handle = h;
    d->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d->thread = std::thread([this, d] { read_loop(d); });
    return true;
  }

  void close(const std::string& id) override {
    std::shared_ptr<Dev> d = find(id);
    if (d) stop_reader(*d);  // the reader closes the handle; nothing here waits for it
  }

  bool send_output(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size) override {
    std::shared_ptr<Dev> d = find(id);
    if (!d || report_id < 0 || report_id > 255 || (!data && size) || size > 4095) return false;
    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->handle == INVALID_HANDLE_VALUE) return false;
    std::vector<std::uint8_t> buf(std::max<std::size_t>(size + 1, d->out_len), 0);  // the length must match the device's
    buf[0] = static_cast<std::uint8_t>(report_id);
    if (size) std::copy(data, data + size, buf.begin() + 1);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    bool ok = WriteFile(d->handle, buf.data(), static_cast<DWORD>(buf.size()), &written, &ov) != 0;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      if (WaitForSingleObject(ov.hEvent, 200) == WAIT_OBJECT_0) ok = GetOverlappedResult(d->handle, &ov, &written, FALSE) != 0;
      else CancelIoEx(d->handle, &ov);
    }
    CloseHandle(ov.hEvent);
    return ok;
  }

  bool send_feature(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size) override {
    std::shared_ptr<Dev> d = find(id);
    if (!d || report_id < 0 || report_id > 255 || (!data && size) || size > 4095) return false;
    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->handle == INVALID_HANDLE_VALUE) return false;
    std::vector<std::uint8_t> buf(std::max<std::size_t>(size + 1, d->feature_len), 0);
    buf[0] = static_cast<std::uint8_t>(report_id);
    if (size) std::copy(data, data + size, buf.begin() + 1);
    return HidD_SetFeature(d->handle, buf.data(), static_cast<ULONG>(buf.size())) != 0;
  }

  bool get_feature(const std::string& id, int report_id, std::vector<std::uint8_t>& out) override {
    std::shared_ptr<Dev> d = find(id);
    if (!d || report_id < 0 || report_id > 255) return false;
    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->handle == INVALID_HANDLE_VALUE || d->feature_len == 0) return false;
    std::vector<std::uint8_t> buf(d->feature_len, 0);
    buf[0] = static_cast<std::uint8_t>(report_id);
    if (!HidD_GetFeature(d->handle, buf.data(), static_cast<ULONG>(buf.size()))) return false;
    out.assign(buf.begin() + 1, buf.end());
    return true;
  }

 private:
  struct Dev {
    std::string path;  // = info.id
    HidDeviceInfo info;
    std::size_t in_len = 0, out_len = 0, feature_len = 0;  // report lengths including the id byte
    std::mutex mutex;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
    std::thread thread;
    std::atomic<bool> done{false};
  };

  std::shared_ptr<Dev> find(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : devs_)
      if (d->path == id) return d;
    return nullptr;
  }

  void stop_reader(Dev& d) {
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.stop) SetEvent(d.stop);
  }

  void join(Dev& d) {
    if (d.thread.joinable() && std::this_thread::get_id() != d.thread.get_id()) d.thread.join();
  }

  void read_loop(std::shared_ptr<Dev> d) {
    std::vector<std::uint8_t> buf(std::max<std::size_t>(d->in_len, 65), 0);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE handle, stop;
    {
      std::lock_guard<std::mutex> lock(d->mutex);
      handle = d->handle;
      stop = d->stop;
    }
    while (true) {
      ResetEvent(ov.hEvent);
      DWORD got = 0;
      bool ok = ReadFile(handle, buf.data(), static_cast<DWORD>(buf.size()), &got, &ov) != 0;
      if (!ok && GetLastError() == ERROR_IO_PENDING) {
        HANDLE waits[2] = {ov.hEvent, stop};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {  // closing
          CancelIoEx(handle, &ov);
          GetOverlappedResult(handle, &ov, &got, TRUE);
          break;
        }
        ok = GetOverlappedResult(handle, &ov, &got, FALSE) != 0;
      }
      if (!ok) break;  // unplugged (or any other failure): the scan drops the entry
      if (got >= 1 && receive_) receive_(d->path, buf[0], buf.data() + 1, got - 1);  // the id is always the first byte
      if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) break;
    }
    CloseHandle(ov.hEvent);
    {
      std::lock_guard<std::mutex> lock(d->mutex);
      CloseHandle(d->handle);
      d->handle = INVALID_HANDLE_VALUE;
      CloseHandle(d->stop);
      d->stop = nullptr;
    }
    d->done = true;
    if (wake_) SetEvent(wake_);  // rescan soon: this may be an unplug
  }

  void scan() {
    std::vector<HidMatch> matches;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      matches = matches_;
    }
    std::vector<std::shared_ptr<Dev>> found;
    if (!matches.empty()) {
      GUID guid;
      HidD_GetHidGuid(&guid);
      HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
      if (set != INVALID_HANDLE_VALUE) {
        for (DWORD i = 0;; ++i) {
          SP_DEVICE_INTERFACE_DATA ifd = {};
          ifd.cbSize = sizeof ifd;
          if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd)) break;
          DWORD need = 0;
          SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
          if (need == 0 || need > 4096) continue;
          std::vector<std::uint8_t> raw(need, 0);
          auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
          detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
          if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
          std::string path = utf8(detail->DevicePath), low = path;
          std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });
          auto dev = std::make_shared<Dev>();
          if (!path_field(low, "vid_", 16, dev->info.vid) || !path_field(low, "pid_", 16, dev->info.pid)) continue;
          // Only interfaces some criterion could match by vid/pid are touched.
          const bool maybe = std::any_of(matches.begin(), matches.end(), [&](const HidMatch& m) {
            return (m.vid < 0 || m.vid == dev->info.vid) && (m.pid < 0 || m.pid == dev->info.pid);
          });
          if (!maybe) continue;
          int mi = -1;
          if (path_field(low, "mi_", 16, mi)) dev->info.interface_number = mi;
          dev->path = path;
          dev->info.id = path;
          // Zero access: asks the driver about the device without reading its reports.
          HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
          if (h == INVALID_HANDLE_VALUE) continue;
          PHIDP_PREPARSED_DATA pre = nullptr;
          if (HidD_GetPreparsedData(h, &pre)) {
            HIDP_CAPS caps = {};
            if (HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS) {
              dev->info.usage_page = caps.UsagePage;
              dev->info.usage = caps.Usage;
              dev->in_len = caps.InputReportByteLength;
              dev->out_len = caps.OutputReportByteLength;
              dev->feature_len = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(pre);
          }
          wchar_t text[256] = {0};
          if (HidD_GetProductString(h, text, sizeof text - sizeof(wchar_t))) dev->info.product = utf8(text);
          std::fill(std::begin(text), std::end(text), L'\0');
          if (HidD_GetManufacturerString(h, text, sizeof text - sizeof(wchar_t))) dev->info.manufacturer = utf8(text);
          CloseHandle(h);
          const bool ok = std::any_of(matches.begin(), matches.end(), [&](const HidMatch& m) { return hid_matches(m, dev->info); });
          if (ok) found.push_back(std::move(dev));
        }
        SetupDiDestroyDeviceInfoList(set);
      }
    }
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::set<std::string> before, after;
      for (const auto& d : devs_) before.insert(d->path);
      for (const auto& d : found) after.insert(d->path);
      changed = before != after;
      for (auto& n : found)  // keep the object (and its open reader) of a device that is still there
        for (auto& old : devs_)
          if (old->path == n->path) n = old;
      for (auto& old : devs_)
        if (!after.count(old->path)) {
          stop_reader(*old);
          zombies_.push_back(old);
        }
      devs_ = std::move(found);
      for (auto it = zombies_.begin(); it != zombies_.end();) {
        if ((*it)->done) {
          join(**it);
          it = zombies_.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (changed && changed_) changed_();
  }

  void run() {
    while (!stop_) {
      bool forced;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        forced = rescan_;
        rescan_ = false;
      }
      (void)forced;
      scan();
      if (wake_) WaitForSingleObject(wake_, 1000);
    }
  }

  Receive receive_;
  Changed changed_;
  std::thread thread_;
  std::mutex mutex_;
  std::atomic<bool> stop_{false};
  HANDLE wake_ = nullptr;
  bool rescan_ = false;
  std::vector<HidMatch> matches_;
  std::vector<std::shared_ptr<Dev>> devs_, zombies_;
};

}  // namespace

std::unique_ptr<HidIO> create_hid_io(HidIO::Receive receive, HidIO::Changed changed) {
  return std::make_unique<WinHid>(std::move(receive), std::move(changed));
}

}  // namespace deckhand
