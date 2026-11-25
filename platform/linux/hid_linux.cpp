// HID access through hidraw (Linux), without libudev: /sys/class/hidraw is rescanned about once a second
// (hot-plug) and read-only metadata (uevent, report descriptor) is taken from sysfs, so a device the hub
// did not ask for is never opened - not even to read its ids. Opening /dev/hidrawN needs permission: see
// platform/linux/packaging/70-deckhand-hid.rules.example.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"

namespace deckhand {
namespace {

std::string read_file(const std::string& path, std::size_t max = 65536) {
  std::ifstream f(path, std::ios::binary);
  std::string s;
  char buf[1024];
  while (f && s.size() < max) {
    f.read(buf, sizeof buf);
    s.append(buf, static_cast<std::size_t>(f.gcount()));
  }
  return s;
}

// First top-level usage page / usage of a report descriptor, and whether it numbers its reports.
void parse_descriptor(const std::string& d, int& page, int& usage, bool& numbered) {
  page = usage = 0;
  numbered = false;
  bool got_page = false, got_usage = false;
  std::size_t i = 0;
  while (i < d.size()) {
    const std::uint8_t prefix = static_cast<std::uint8_t>(d[i]);
    if (prefix == 0xFE) {  // long item: skip
      i += 3 + (i + 1 < d.size() ? static_cast<std::uint8_t>(d[i + 1]) : 0);
      continue;
    }
    std::size_t size = prefix & 3;
    if (size == 3) size = 4;
    const int type = (prefix >> 2) & 3, tag = prefix >> 4;
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < size && i + 1 + k < d.size(); ++k) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[i + 1 + k])) << (8 * k);
    if (type == 1 && tag == 0 && !got_page) { page = static_cast<int>(v & 0xFFFF); got_page = true; }
    else if (type == 2 && tag == 0 && got_page && !got_usage) { usage = static_cast<int>(v & 0xFFFF); got_usage = true; }
    else if (type == 1 && tag == 8) numbered = true;  // Report ID
    i += 1 + size;
  }
}

class LinuxHid final : public HidIO {
 public:
  LinuxHid(Receive receive, Changed changed) : receive_(std::move(receive)), changed_(std::move(changed)) {
    if (pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) {
      DH_LOG(Error, "HID is unavailable (no wake pipe)");
      return;
    }
    thread_ = std::thread([this] { run(); });
  }
  ~LinuxHid() override {
    stop_ = true;
    wake();
    if (thread_.joinable()) thread_.join();
    for (auto& d : devs_) if (d.fd >= 0) ::close(d.fd);
    if (wake_[0] >= 0) { ::close(wake_[0]); ::close(wake_[1]); }
  }

  void set_matches(const std::vector<HidMatch>& matches) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      matches_ = matches;
      rescan_ = true;
    }
    wake();
  }

  std::vector<HidDeviceInfo> devices() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HidDeviceInfo> out;
    for (const Dev& d : devs_) out.push_back(d.info);
    return out;
  }

  bool open(const std::string& id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    Dev* d = find(id);
    if (!d || hid_usage_refused(d->info.usage_page, d->info.usage)) return false;
    if (d->fd >= 0) return true;
    d->fd = ::open(id.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (d->fd < 0) {
      DH_LOG(Warn, "cannot open %s: %s (see platform/linux/packaging/70-deckhand-hid.rules.example)", id.c_str(), std::strerror(errno));
      return false;
    }
    wake();
    return true;
  }

  void close(const std::string& id) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Dev* d = find(id);
      if (!d || d->fd < 0) return;
      ::close(d->fd);  // the reader thread rebuilds its poll set under the same lock
      d->fd = -1;
    }
    wake();
  }

  bool send_output(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size) override {
    std::vector<std::uint8_t> buf;
    if (!frame(report_id, data, size, buf)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    Dev* d = find(id);
    return d && d->fd >= 0 && ::write(d->fd, buf.data(), buf.size()) == static_cast<ssize_t>(buf.size());
  }

  bool send_feature(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size) override {
    std::vector<std::uint8_t> buf;
    if (!frame(report_id, data, size, buf)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    Dev* d = find(id);
    return d && d->fd >= 0 && ::ioctl(d->fd, HIDIOCSFEATURE(static_cast<unsigned>(buf.size())), buf.data()) >= 0;
  }

  bool get_feature(const std::string& id, int report_id, std::vector<std::uint8_t>& out) override {
    if (report_id < 0 || report_id > 255) return false;
    std::vector<std::uint8_t> buf(512, 0);
    buf[0] = static_cast<std::uint8_t>(report_id);
    std::lock_guard<std::mutex> lock(mutex_);
    Dev* d = find(id);
    if (!d || d->fd < 0) return false;
    const int n = ::ioctl(d->fd, HIDIOCGFEATURE(static_cast<unsigned>(buf.size())), buf.data());
    if (n < 1) return false;
    out.assign(buf.begin() + (d->numbered ? 1 : 0), buf.begin() + n);
    return true;
  }

 private:
  struct Dev {
    std::string path;  // = info.id
    HidDeviceInfo info;
    bool numbered = false;
    int fd = -1;
  };

  static bool frame(int report_id, const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& buf) {
    if (report_id < 0 || report_id > 255 || (!data && size) || size > 4095) return false;
    buf.assign(1, static_cast<std::uint8_t>(report_id));  // hidraw: the report id (0 if none) comes first
    buf.insert(buf.end(), data, data + size);
    return true;
  }

  Dev* find(const std::string& id) {
    for (Dev& d : devs_)
      if (d.path == id) return &d;
    return nullptr;
  }

  void wake() {
    if (wake_[1] >= 0) {
      const char c = 0;
      if (::write(wake_[1], &c, 1) < 0) {}
    }
  }

  bool wanted(const HidDeviceInfo& i) const {
    for (const HidMatch& m : matches_)
      if (hid_matches(m, i)) return true;
    return false;
  }

  // Lists /sys/class/hidraw entries whose vid/pid some criterion could match, then checks the rest.
  void scan() {
    std::vector<HidMatch> matches;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      matches = matches_;
    }
    std::vector<Dev> found;
    if (!matches.empty()) {
      namespace fs = std::filesystem;
      std::error_code ec;
      for (const auto& e : fs::directory_iterator("/sys/class/hidraw", ec)) {
        const std::string name = e.path().filename().string();
        if (name.compare(0, 6, "hidraw") != 0) continue;
        const std::string base = e.path().string() + "/device/";
        std::istringstream uevent(read_file(base + "uevent"));
        std::string line;
        Dev d;
        unsigned bus = 0, vid = 0, pid = 0;
        bool have_id = false;
        while (std::getline(uevent, line)) {
          if (line.compare(0, 7, "HID_ID=") == 0) have_id = std::sscanf(line.c_str() + 7, "%x:%x:%x", &bus, &vid, &pid) == 3;
          else if (line.compare(0, 9, "HID_NAME=") == 0) d.info.product = line.substr(9);
        }
        if (!have_id) continue;
        d.info.vid = static_cast<int>(vid);
        d.info.pid = static_cast<int>(pid);
        const bool maybe = std::any_of(matches.begin(), matches.end(), [&](const HidMatch& m) {
          return (m.vid < 0 || m.vid == d.info.vid) && (m.pid < 0 || m.pid == d.info.pid);
        });
        if (!maybe) continue;
        parse_descriptor(read_file(base + "report_descriptor"), d.info.usage_page, d.info.usage, d.numbered);
        const std::string iface = fs::canonical(e.path() / "device", ec).parent_path().filename().string();  // e.g. 1-2:1.0
        const auto dot = iface.rfind('.');
        if (iface.find(':') != std::string::npos && dot != std::string::npos) d.info.interface_number = std::atoi(iface.c_str() + dot + 1);
        d.path = "/dev/" + name;
        d.info.id = d.path;
        bool ok = false;
        for (const HidMatch& m : matches) ok = ok || hid_matches(m, d.info);
        if (ok) found.push_back(std::move(d));
      }
    }
    std::sort(found.begin(), found.end(), [](const Dev& a, const Dev& b) { return a.path < b.path; });
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::set<std::string> before, after;
      for (const Dev& d : devs_) before.insert(d.path);
      for (const Dev& d : found) after.insert(d.path);
      changed = before != after;
      for (Dev& n : found)
        if (Dev* old = find(n.path)) n.fd = old->fd;
      for (Dev& old : devs_)
        if (!after.count(old.path) && old.fd >= 0) ::close(old.fd);
      devs_ = std::move(found);
    }
    if (changed && changed_) changed_();
  }

  void run() {
    auto last_scan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::uint8_t buf[4096];
    while (!stop_) {
      std::vector<pollfd> fds{{wake_[0], POLLIN, 0}};
      std::vector<std::string> ids{std::string()};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Dev& d : devs_)
          if (d.fd >= 0) {
            fds.push_back({d.fd, POLLIN, 0});
            ids.push_back(d.path);
          }
      }
      const int ready = ::poll(fds.data(), fds.size(), 500);
      if (stop_) break;
      if (ready > 0 && (fds[0].revents & POLLIN)) {
        char drain[64];
        while (::read(wake_[0], drain, sizeof drain) > 0) {}
      }
      for (std::size_t i = 1; i < fds.size(); ++i) {
        if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
        std::string id = ids[i];
        int n;
        bool numbered;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          Dev* d = find(id);
          if (!d || d->fd != fds[i].fd) continue;  // closed meanwhile
          numbered = d->numbered;
          n = static_cast<int>(::read(d->fd, buf, sizeof buf));
          if (n < 0 && errno != EAGAIN && errno != EINTR) {  // unplugged: drop it now, the next scan removes the entry
            ::close(d->fd);
            d->fd = -1;
            last_scan = std::chrono::steady_clock::time_point{};
          }
        }
        if (n > 0 && receive_) {
          if (numbered) receive_(id, buf[0], buf + 1, static_cast<std::size_t>(n) - 1);
          else receive_(id, 0, buf, static_cast<std::size_t>(n));
        }
      }
      bool force;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        force = rescan_;
        rescan_ = false;
      }
      if (force || std::chrono::steady_clock::now() - last_scan > std::chrono::seconds(1)) {
        last_scan = std::chrono::steady_clock::now();
        scan();
      }
    }
  }

  Receive receive_;
  Changed changed_;
  std::thread thread_;
  std::mutex mutex_;
  std::atomic<bool> stop_{false};
  int wake_[2] = {-1, -1};
  bool rescan_ = false;
  std::vector<HidMatch> matches_;
  std::vector<Dev> devs_;
};

}  // namespace

std::unique_ptr<HidIO> create_hid_io(HidIO::Receive receive, HidIO::Changed changed) {
  return std::make_unique<LinuxHid>(std::move(receive), std::move(changed));
}

}  // namespace deckhand
