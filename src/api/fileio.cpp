#include "fileio.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace deckhand::api {

#if defined(_WIN32)
namespace {
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
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
}  // namespace

bool write_file_atomic(const std::string& path, const std::string& data, bool owner_only) {
  const std::wstring target = widen(path), tmp = widen(path + ".tmp");
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (owner_only &&
      ConvertStringSecurityDescriptorToSecurityDescriptorW(owner_only_sddl().c_str(), SDDL_REVISION_1, &sd, nullptr))
    sa.lpSecurityDescriptor = sd;
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (sd) LocalFree(sd);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  const char* p = data.data();
  std::size_t left = data.size();
  while (left > 0) {
    DWORD n = 0;
    if (!WriteFile(h, p, static_cast<DWORD>(left > (1u << 30) ? (1u << 30) : left), &n, nullptr) || n == 0) {
      ok = false;
      break;
    }
    p += n;
    left -= n;
  }
  ok = ok && FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || !MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}
#else
bool write_file_atomic(const std::string& path, const std::string& data, bool owner_only) {
  const mode_t mode = owner_only ? 0600 : 0644;
  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return false;
  fchmod(fd, mode);
  const char* p = data.data();
  std::size_t left = data.size();
  bool ok = true;
  while (left > 0) {
    ssize_t n = ::write(fd, p, left);
    if (n <= 0) {
      ok = false;
      break;
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  ok = ok && fsync(fd) == 0;
  ::close(fd);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}
#endif

}  // namespace deckhand::api
