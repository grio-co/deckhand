// Windows implementation of the OS interface in deckhand/audio/ipc.hpp (the POSIX one is ipc_posix.cpp).
//
//  * Shared memory: pagefile-backed named file mappings in the per-session ("Local\") namespace. A named mapping
//    lives only while some handle to it is open, so a segment created here keeps its handle in a process-wide table
//    until Shm::remove() (which then plays the role of shm_unlink: mappings already made stay valid, the name goes).
//  * Single-instance lock: a lock file opened with no sharing; the OS drops it when the process dies.
//  * Control channel: a TCP socket on 127.0.0.1 with a random port. The "socket path" names a small file holding
//    "<port> <32 hex digits>"; the digits are a secret the client sends first, so only processes that can read
//    the (per-user) file can talk to the engine. AF_UNIX sockets would also work on Windows 10 1803+, but loopback
//    TCP behaves the same on every Windows 10/11 build and under Wine.
//  * Spawning: CreateProcessW with only the log and NUL handles inherited, detached from our console.
#include "deckhand/audio/ipc.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace deckhand::ipc {

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string win_error(const std::string& what, DWORD code = GetLastError()) {
  char buf[256] = {};
  const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf,
                                 sizeof buf - 1, nullptr);
  std::string m(buf, n);
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ' || m.back() == '.')) m.pop_back();
  return std::string(what) + ": " + (m.empty() ? "error " + std::to_string(code) : m) + " (" + std::to_string(code) + ")";
}

void ensure_winsock() {
  static const bool once = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)once;
}

SOCKET as_socket(OsHandle h) { return static_cast<SOCKET>(h); }
OsHandle from_socket(SOCKET s) { return s == INVALID_SOCKET ? OsHandle(-1) : static_cast<OsHandle>(s); }
HANDLE as_handle(OsHandle h) { return reinterpret_cast<HANDLE>(h); }

bool random_bytes(std::uint8_t* out, std::size_t n) {
  return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

constexpr std::size_t kSecretBytes = 16;

std::string to_hex(const std::uint8_t* p, std::size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

bool from_hex(const std::string& s, std::uint8_t* out, std::size_t n) {
  if (s.size() != n * 2) return false;
  auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (std::size_t i = 0; i < n; ++i) {
    const int a = v(s[2 * i]), b = v(s[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = static_cast<std::uint8_t>(a * 16 + b);
  }
  return true;
}

// "D:P(A;;FA;;;<current user>)(A;;FA;;;SY)": a protected ACL giving the user and SYSTEM full access and nobody else.
std::wstring owner_only_sddl() {
  std::wstring sddl = L"D:P(A;;FA;;;SY)";
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD n = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> buf(n);
    LPWSTR text = nullptr;
    if (n > 0 && GetTokenInformation(token, TokenUser, buf.data(), n, &n) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
      sddl = std::wstring(L"D:P(A;;FA;;;") + text + L")(A;;FA;;;SY)";
      LocalFree(text);
    }
    CloseHandle(token);
  }
  return sddl;
}

// Creates `path` readable by the owner (and SYSTEM) only and writes `data` into it, replacing any older file.
bool write_private_file(const std::string& path, const std::string& data) {
  const std::wstring target = widen(path), tmp = widen(path + ".tmp");
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(owner_only_sddl().c_str(), SDDL_REVISION_1, &sd, nullptr))
    sa.lpSecurityDescriptor = sd;
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (sd) LocalFree(sd);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &n, nullptr) && n == data.size();
  CloseHandle(h);
  if (!ok || !MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

bool read_small_file(const std::string& path, std::string& out) {
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char buf[256];
  DWORD n = 0;
  const bool ok = ReadFile(h, buf, sizeof buf, &n, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out.assign(buf, n);
  return true;
}

// Named mappings this process created and has not yet removed (see the file comment).
std::mutex g_mapping_mutex;
std::map<std::string, HANDLE>& mappings() {
  static std::map<std::string, HANDLE> m;
  return m;
}

// Children started by spawn_detached: their process handles, for reap_child.
std::mutex g_child_mutex;
std::map<std::uint32_t, HANDLE>& children() {
  static std::map<std::uint32_t, HANDLE> m;
  return m;
}

std::string quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
  std::string q = "\"";
  std::size_t backslashes = 0;
  for (const char c : a) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') q.append(backslashes * 2 + 1, '\\');
    else q.append(backslashes, '\\');
    backslashes = 0;
    q += c;
  }
  q.append(backslashes * 2, '\\');
  return q + "\"";
}

}  // namespace

std::uint64_t now_ns() {
  static const LONGLONG freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  const std::uint64_t ticks = static_cast<std::uint64_t>(t.QuadPart), f = static_cast<std::uint64_t>(freq);
  return (ticks / f) * 1000000000ull + (ticks % f) * 1000000000ull / f;  // QPC is system-wide: comparable between processes
}

std::uint32_t current_pid() { return static_cast<std::uint32_t>(GetCurrentProcessId()); }

bool process_alive(std::uint32_t pid) {
  if (pid == 0) return false;
  HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
  const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return alive;
}

bool reap_child(std::uint32_t pid) {
  if (pid == 0) return false;
  std::lock_guard<std::mutex> lock(g_child_mutex);
  auto it = children().find(pid);
  if (it == children().end()) return false;
  if (WaitForSingleObject(it->second, 0) != WAIT_OBJECT_0) return false;
  CloseHandle(it->second);
  children().erase(it);
  return true;
}

std::string executable_dir() {
  std::wstring buf(1024, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);
  }
  const std::size_t cut = buf.find_last_of(L"\\/");
  return cut == std::wstring::npos ? std::string() : narrow(buf.substr(0, cut));
}

// ---- shared memory ------------------------------------------------------------------------------
Shm& Shm::operator=(Shm&& o) noexcept {
  if (this != &o) {
    close();
    base_ = o.base_;
    size_ = o.size_;
    o.base_ = nullptr;
    o.size_ = 0;
  }
  return *this;
}

void Shm::close() {
  if (base_) UnmapViewOfFile(base_);
  base_ = nullptr;
  size_ = 0;
}

bool Shm::create(const std::string& name, std::size_t size, Shm& out, std::string& error, bool replace) {
  out.close();
  const std::wstring wname = widen(name);
  {
    std::lock_guard<std::mutex> lock(g_mapping_mutex);
    auto it = mappings().find(name);
    if (it != mappings().end()) {
      if (!replace) {
        // Our own earlier segment of that name is still reachable by name: that is what "exists" means here.
        error = "segment " + name + " already exists";
        return false;
      }
      CloseHandle(it->second);
      mappings().erase(it);
    }
  }
  const std::uint64_t sz = size;
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(sz >> 32),
                                static_cast<DWORD>(sz & 0xFFFFFFFFu), wname.c_str());
  if (!h) {
    error = win_error("CreateFileMapping");
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {  // another process still holds a segment of that name
    CloseHandle(h);
    error = "segment " + name + " is in use by another process";
    return false;
  }
  void* p = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, size);
  if (!p) {
    error = win_error("MapViewOfFile");
    CloseHandle(h);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_mapping_mutex);
    mappings()[name] = h;
  }
  out.base_ = p;
  out.size_ = size;
  return true;
}

bool Shm::open(const std::string& name, bool writable, Shm& out, std::string& error) {
  out.close();
  HANDLE h = OpenFileMappingW(writable ? FILE_MAP_WRITE : FILE_MAP_READ, FALSE, widen(name).c_str());
  if (!h) {
    error = win_error("OpenFileMapping");
    return false;
  }
  void* p = MapViewOfFile(h, writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0);
  CloseHandle(h);  // the view keeps the section alive
  if (!p) {
    error = win_error("MapViewOfFile");
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.RegionSize == 0) {
    UnmapViewOfFile(p);
    error = "segment is empty";
    return false;
  }
  out.base_ = p;
  out.size_ = mbi.RegionSize;  // rounded up to whole pages; the headers inside carry the exact sizes
  return true;
}

void Shm::remove(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_mapping_mutex);
  auto it = mappings().find(name);
  if (it == mappings().end()) return;
  CloseHandle(it->second);
  mappings().erase(it);
}

// ---- instance lock ------------------------------------------------------------------------------
bool InstanceLock::acquire(const std::string& path, std::string& error) {
  release();
  const std::wstring wpath = widen(path);
  for (int attempt = 0; attempt < 10; ++attempt) {
    // No sharing: a second opener gets a sharing violation. DELETE access lets release_and_remove() unlink it.
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
      fd_ = reinterpret_cast<OsHandle>(h);
      path_ = path;
      return true;
    }
    const DWORD e = GetLastError();
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) {
      error = "another engine already runs for this data folder";
      return false;
    }
    if (e != ERROR_ACCESS_DENIED) {  // access denied also shows up while a previous owner's delete is pending
      error = win_error("open lock", e);
      return false;
    }
    Sleep(20);
  }
  error = "could not take the instance lock";
  return false;
}

void InstanceLock::release() {
  if (fd_ != -1) CloseHandle(as_handle(fd_));
  fd_ = -1;
}

void InstanceLock::release_and_remove() {
  const bool was_held = fd_ != -1;
  if (was_held) {
    FILE_DISPOSITION_INFO d{TRUE};
    SetFileInformationByHandle(as_handle(fd_), FileDispositionInfo, &d, sizeof d);
  }
  release();
  // Belt and braces for file systems that ignore the delete disposition. Another process cannot hold the file by now:
  // the lock opens it without sharing, so a delete that races with a new owner simply fails.
  if (was_held && !path_.empty()) DeleteFileW(widen(path_).c_str());
}

// ---- sockets ------------------------------------------------------------------------------------
namespace {

// Waits for `s` to become readable; 1 = readable, 0 = timeout, -1 = error.
int wait_readable(SOCKET s, int timeout_ms) {
  WSAPOLLFD pf{s, POLLRDNORM, 0};
  const int r = WSAPoll(&pf, 1, timeout_ms);
  if (r < 0) return WSAGetLastError() == WSAEINTR ? 0 : -1;
  return r > 0 ? 1 : 0;
}

bool send_exact(SOCKET s, const void* data, std::size_t n) {
  const auto* p = static_cast<const char*>(data);
  while (n > 0) {
    const int w = ::send(s, p, static_cast<int>(std::min<std::size_t>(n, 1u << 20)), 0);
    if (w < 0 && WSAGetLastError() == WSAEINTR) continue;
    if (w <= 0) return false;
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

bool recv_exact(SOCKET s, void* data, std::size_t n, int timeout_ms, bool* timed_out) {
  if (timed_out) *timed_out = false;
  auto* p = static_cast<char*>(data);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (n > 0) {
    const long long left = std::max<long long>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    const int r = wait_readable(s, static_cast<int>(left));
    if (r < 0) return false;
    if (r == 0) {
      if (timed_out) *timed_out = true;
      return false;
    }
    const int got = ::recv(s, p, static_cast<int>(std::min<std::size_t>(n, 1u << 20)), 0);
    if (got < 0 && WSAGetLastError() == WSAEINTR) continue;
    if (got <= 0) return false;
    p += got;
    n -= static_cast<std::size_t>(got);
  }
  return true;
}

void no_inherit(SOCKET s) { SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0); }
}  // namespace

void Conn::close() {
  if (fd_ != -1) ::closesocket(as_socket(fd_));
  fd_ = -1;
}

bool Conn::connect(const std::string& path, Conn& out) {
  out.close();
  ensure_winsock();
  std::string text;
  if (!read_small_file(path, text)) return false;
  unsigned port = 0;
  char hex[64] = {};
  if (std::sscanf(text.c_str(), "%u %63s", &port, hex) != 2 || port == 0 || port > 65535) return false;
  std::uint8_t secret[kSecretBytes];
  if (!from_hex(hex, secret, sizeof secret)) return false;
  const SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return false;
  no_inherit(s);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<u_short>(port));
  if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || !send_exact(s, secret, sizeof secret)) {
    ::closesocket(s);
    return false;
  }
  out.fd_ = from_socket(s);
  return true;
}

bool Conn::send_all(const void* data, std::size_t n) { return fd_ != -1 && send_exact(as_socket(fd_), data, n); }

bool Conn::recv_all(void* data, std::size_t n, int timeout_ms, bool* timed_out) {
  if (fd_ == -1) {
    if (timed_out) *timed_out = false;
    return false;
  }
  return recv_exact(as_socket(fd_), data, n, timeout_ms, timed_out);
}

void Listener::close() {
  if (fd_ != -1) ::closesocket(as_socket(fd_));
  fd_ = -1;
  if (!path_.empty()) DeleteFileW(widen(path_).c_str());
  path_.clear();
}

bool Listener::listen(const std::string& path, std::string& error) {
  close();
  ensure_winsock();
  const SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) {
    error = win_error("socket", static_cast<DWORD>(WSAGetLastError()));
    return false;
  }
  no_inherit(s);
  const BOOL excl = TRUE;  // nobody else may share the port
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  int len = sizeof a;
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 8) != 0 ||
      getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
    error = win_error("bind/listen", static_cast<DWORD>(WSAGetLastError()));
    ::closesocket(s);
    return false;
  }
  std::uint8_t secret[kSecretBytes];
  if (!random_bytes(secret, sizeof secret)) {
    error = "no random source for the control channel";
    ::closesocket(s);
    return false;
  }
  const std::string text = std::to_string(ntohs(a.sin_port)) + " " + to_hex(secret, sizeof secret) + "\n";
  if (!write_private_file(path, text)) {
    error = win_error("write " + path);
    ::closesocket(s);
    return false;
  }
  fd_ = from_socket(s);
  path_ = path;
  secret_.assign(reinterpret_cast<const char*>(secret), sizeof secret);
  return true;
}

bool Listener::accept(Conn& out, int timeout_ms) {
  if (fd_ == -1) return false;
  if (wait_readable(as_socket(fd_), timeout_ms) <= 0) return false;
  const SOCKET c = ::accept(as_socket(fd_), nullptr, nullptr);
  if (c == INVALID_SOCKET) return false;
  no_inherit(c);
  std::uint8_t got[kSecretBytes];
  unsigned diff = 0;
  if (!recv_exact(c, got, sizeof got, 1000, nullptr)) {
    ::closesocket(c);
    return false;
  }
  for (std::size_t i = 0; i < sizeof got; ++i) diff |= static_cast<unsigned>(got[i] ^ static_cast<std::uint8_t>(secret_[i]));
  if (diff != 0) {  // not the engine's client
    ::closesocket(c);
    return false;
  }
  out.close();
  out.fd_ = from_socket(c);
  return true;
}

// ---- naming -------------------------------------------------------------------------------------
std::string SessionNames::block(int slot) const {
  char n[8];
  std::snprintf(n, sizeof n, "%02d", slot < 0 ? 0 : slot % 100);
  return shm_block_prefix + n;
}

bool session_names(const std::string& data_dir, SessionNames& out, std::string& error) {
  std::error_code ec;
  std::string canon = std::filesystem::weakly_canonical(data_dir, ec).string();
  if (ec) canon = data_dir;
  std::transform(canon.begin(), canon.end(), canon.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  wchar_t user[256] = L"";
  DWORD ulen = 256;
  if (!GetUserNameW(user, &ulen)) user[0] = 0;
  const std::string key = narrow(user) + ":" + canon;
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (const unsigned char c : key) h = (h ^ c) * 1099511628211ull;
  char hex[16];
  std::snprintf(hex, sizeof hex, "%08x", static_cast<unsigned>((h >> 32) ^ (h & 0xFFFFFFFFu)));

  const char* over = std::getenv("DECKHAND_IPC_DIR");  // tests and unusual setups
  if (over && *over) {
    out.dir = over;
  } else {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local && *local) {
      out.dir = std::string(local) + "\\DeckHand\\ipc";
    } else {
      wchar_t tmp[MAX_PATH + 2];
      const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
      out.dir = narrow(std::wstring(tmp, n)) + "DeckHand-ipc";
    }
  }
  std::filesystem::create_directories(std::filesystem::path(widen(out.dir)), ec);
  const DWORD attr = GetFileAttributesW(widen(out.dir).c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
    error = "unsafe IPC folder " + out.dir;
    return false;
  }
  out.lock = out.dir + "\\" + hex + ".lock";
  out.socket = out.dir + "\\" + hex + ".sock";
  out.shm_main = std::string("Local\\dh") + hex + ".m";
  out.shm_block_prefix = std::string("Local\\dh") + hex + ".b";
  return true;
}

bool spawn_detached(const std::string& exe, const std::vector<std::string>& args, const std::string& log_file,
                    std::uint32_t& pid, std::string& error) {
  std::string image = exe;
  std::replace(image.begin(), image.end(), '/', '\\');
  std::error_code ec;
  if (!std::filesystem::exists(std::filesystem::path(widen(image)), ec) &&
      std::filesystem::exists(std::filesystem::path(widen(image + ".exe")), ec))
    image += ".exe";
  std::string cmd = quote_arg(image);
  for (const std::string& a : args) cmd += " " + quote_arg(a);
  std::wstring wcmd = widen(cmd);
  wcmd.push_back(L'\0');

  SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
  HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  HANDLE log = CreateFileW(widen(log_file.empty() ? std::string("NUL") : log_file).c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  auto close_all = [&] {
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  };
  if (in == INVALID_HANDLE_VALUE || log == INVALID_HANDLE_VALUE) {
    error = win_error("open the engine log " + log_file);
    close_all();
    return false;
  }

  // Inherit exactly the two handles above, nothing else of ours (sockets, mappings, locks).
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<char> attr_mem(bytes);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_mem.data());
  HANDLE list[2] = {in, log};
  if (!InitializeProcThreadAttributeList(attrs, 1, 0, &bytes) ||
      !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof list, nullptr, nullptr)) {
    error = win_error("process attributes");
    close_all();
    return false;
  }
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in;
  si.StartupInfo.hStdOutput = log;
  si.StartupInfo.hStdError = log;
  si.lpAttributeList = attrs;
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(widen(image).c_str(), wcmd.data(), nullptr, nullptr, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP |
                                     CREATE_UNICODE_ENVIRONMENT,
                                 nullptr, nullptr, &si.StartupInfo, &pi);
  const DWORD err = GetLastError();
  DeleteProcThreadAttributeList(attrs);
  close_all();
  if (!ok) {
    error = win_error("could not start " + image, err);
    return false;
  }
  CloseHandle(pi.hThread);
  pid = static_cast<std::uint32_t>(pi.dwProcessId);
  std::lock_guard<std::mutex> lock(g_child_mutex);
  auto old = children().find(pid);
  if (old != children().end()) CloseHandle(old->second);
  children()[pid] = pi.hProcess;
  return true;
}

}  // namespace deckhand::ipc

#endif  // _WIN32
