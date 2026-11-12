// DeckHand Windows ui: Win32 window, input and lifecycle, Direct3D 11 rendering, DirectWrite text. All
// behaviour lives in the C++ core; see src/app/include/deckhand/app/capi.h.
//
//   deckhand [--deck-a FILE] [--deck-b FILE] [FILE...] [--screenshot FILE.png [--frames N] [--size WxH]]
//
// DECKHAND_LOG=error|warn|info|debug|trace sets the log level. A file named "portable" beside the exe keeps all
// data in DeckHandData\ next to it; otherwise data is in %APPDATA%\DeckHand. Resources (controllers, lang, skins)
// are read from share\deckhand beside the exe (DECKHAND_RESOURCE_DIR overrides).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "com.hpp"
#include "deckhand/app/capi.h"
#include "png.hpp"
#include "renderer.hpp"
#include "text.hpp"

namespace fs = std::filesystem;
using deckhand::shell::Com;
using deckhand::shell::Renderer;

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

// fs::path from a UTF-8 string, independent of the process code page.
fs::path path_from_utf8(const std::string& s) { return fs::path(widen(s)); }
std::string path_to_utf8(const fs::path& p) { return narrow(p.wstring()); }

struct Options {
  std::string deck_a, deck_b, screenshot;
  std::vector<std::string> files;
  int frames = 30;
  int shot_w = 1440, shot_h = 900;
  bool help = false;
};

bool parse_args(const std::vector<std::string>& args, Options& o) {
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= args.size()) return false;
      dst = args[++i];
      return true;
    };
    if (a == "--deck-a") {
      if (!value(o.deck_a)) return false;
    } else if (a == "--deck-b") {
      if (!value(o.deck_b)) return false;
    } else if (a == "--screenshot") {
      if (!value(o.screenshot)) return false;
    } else if (a == "--frames") {
      std::string n;
      if (!value(n)) return false;
      o.frames = std::max(1, std::atoi(n.c_str()));
    } else if (a == "--size") {
      std::string n;
      if (!value(n) || std::sscanf(n.c_str(), "%dx%d", &o.shot_w, &o.shot_h) != 2 || o.shot_w < 200 || o.shot_h < 200) return false;
    } else if (a == "--help" || a == "-h" || a == "/?") {
      o.help = true;
    } else if (!a.empty() && a[0] == '-') {
      return false;
    } else {
      o.files.push_back(a);
    }
  }
  return true;
}

std::string getenv_utf8(const wchar_t* name) {
  const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring v(n, L'\0');
  v.resize(GetEnvironmentVariableW(name, v.data(), n));
  return narrow(v);
}

fs::path executable_dir() {
  std::wstring buf(1024, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return fs::path(".");
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);
  }
  return fs::path(buf).parent_path();
}

std::string data_dir(const fs::path& exe_dir) {
  std::error_code ec;
  if (fs::exists(exe_dir / L"portable", ec)) return path_to_utf8(exe_dir / L"DeckHandData");
  PWSTR roaming = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming) {
    const std::string p = path_to_utf8(fs::path(roaming) / L"DeckHand");
    CoTaskMemFree(roaming);
    return p;
  }
  const std::string appdata = getenv_utf8(L"APPDATA");
  if (!appdata.empty()) return path_to_utf8(path_from_utf8(appdata) / L"DeckHand");
  return path_to_utf8(exe_dir / L"DeckHandData");
}

std::string resource_dir(const fs::path& exe_dir) {
  std::vector<fs::path> candidates;
  const std::string env = getenv_utf8(L"DECKHAND_RESOURCE_DIR");
  if (!env.empty()) candidates.push_back(path_from_utf8(env));
  candidates.push_back(exe_dir / L"share" / L"deckhand");
  candidates.push_back(exe_dir);
  candidates.push_back(exe_dir / L".." / L"share" / L"deckhand");
  std::error_code ec;
  for (const auto& c : candidates)
    if (fs::is_directory(c / L"lang", ec)) return path_to_utf8(fs::weakly_canonical(c, ec));
  return path_to_utf8(exe_dir / L"share" / L"deckhand");
}

int log_level() {
  const std::string s = [] {
    const std::string e = getenv_utf8(L"DECKHAND_LOG");
    return e.empty() ? std::string("info") : e;
  }();
  static const char* names[] = {"error", "warn", "info", "debug", "trace"};
  for (int i = 0; i < 5; ++i)
    if (s == names[i]) return i;
  return 2;
}

// ---- DPI (Windows 10 1607+ APIs, looked up at run time so the exe also starts on older builds) ------------------
using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using AdjustRectDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

template <class F>
F user32_fn(const char* name) {
  return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name)));
}

void enable_dpi_awareness() {
  // The manifest already asks for per-monitor v2; this covers a manifest that was stripped.
  if (auto f = user32_fn<SetDpiContextFn>("SetProcessDpiAwarenessContext")) f(reinterpret_cast<HANDLE>(-4));
}

float window_scale(HWND hwnd) {
  if (auto f = user32_fn<GetDpiForWindowFn>("GetDpiForWindow")) {
    const UINT dpi = f(hwnd);
    if (dpi > 0) return static_cast<float>(dpi) / 96.0f;
  }
  HDC dc = GetDC(hwnd);
  const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc) ReleaseDC(hwnd, dc);
  return static_cast<float>(dpi) / 96.0f;
}

RECT outer_rect(int client_w, int client_h, DWORD style, DWORD ex_style, float scale) {
  RECT r{0, 0, client_w, client_h};
  if (auto f = user32_fn<AdjustRectDpiFn>("AdjustWindowRectExForDpi")) f(&r, style, FALSE, ex_style, static_cast<UINT>(scale * 96.0f + 0.5f));
  else AdjustWindowRectEx(&r, style, FALSE, ex_style);
  return r;
}

// ---- platform hooks ------------------------------------------------------------------------------------------
struct ShellApp;
ShellApp* g_shell = nullptr;

bool choose_folder(void*, const char* title, char* out_path, std::uint32_t cap);
bool list_export_targets(void*, char* out, std::uint32_t cap);

constexpr int kMinW = 1100, kMinH = 700;

struct ShellApp {
  HWND hwnd = nullptr;
  dh_app* core = nullptr;
  Renderer renderer;
  float scale = 1.0f;
  bool vsync = true;
  bool minimized = false;
  bool button_down = false;
  bool tracking_size = false;
  std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
  wchar_t high_surrogate = 0;
  POINT last_click{0, 0};

  std::uint32_t modifiers() const {
    std::uint32_t m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= DH_MOD_SHIFT;
    if (GetKeyState(VK_MENU) & 0x8000) m |= DH_MOD_ALT;
    // Ctrl takes the role Command has on macOS.
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= DH_MOD_COMMAND | DH_MOD_CONTROL;
    return m;
  }
  void sync_modifiers() { dh_app_modifiers(core, modifiers()); }

  void frame() {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 1 || h < 1 || minimized) return;
    renderer.resize(w, h);
    dh_app_set_display(core, static_cast<float>(w), static_cast<float>(h), scale, 0, 0, 0, 0);
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::min(0.25, std::chrono::duration<double>(now - last_frame).count());
    last_frame = now;
    const dh_frame* f = dh_app_frame(core, dt);
    if (!f) return;
    renderer.render(*f, w, h);
    if (!renderer.present(vsync)) std::this_thread::sleep_for(std::chrono::milliseconds(50));  // occluded
  }

  void pointer(dh_ptr_kind kind, LPARAM lp, int clicks) {
    sync_modifiers();
    dh_app_pointer(core, kind, 0, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)), clicks);
  }

  void text(const wchar_t* w, std::size_t n) {
    const std::string u8 = narrow(std::wstring(w, n));
    if (!u8.empty()) dh_app_text(core, u8.c_str());
  }
};

int map_key(WPARAM vk) {
  switch (vk) {
    case VK_UP: return DH_KEY_UP;
    case VK_DOWN: return DH_KEY_DOWN;
    case VK_LEFT: return DH_KEY_LEFT;
    case VK_RIGHT: return DH_KEY_RIGHT;
    case VK_RETURN: return DH_KEY_ENTER;
    case VK_ESCAPE: return DH_KEY_ESCAPE;
    case VK_BACK: return DH_KEY_BACKSPACE;
    case VK_TAB: return DH_KEY_TAB;
    default: return 0;
  }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  ShellApp* s = g_shell;
  if (!s || !s->core) return DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      s->frame();  // repaint while the message loop is busy elsewhere (e.g. a modal dialog)
      return 0;
    }
    case WM_SIZE:
      s->minimized = wp == SIZE_MINIMIZED;
      if (!s->minimized) s->frame();
      return 0;
    case WM_ENTERSIZEMOVE:  // the size/move loop blocks the message loop: keep drawing from a timer
      s->tracking_size = true;
      SetTimer(hwnd, 1, 16, nullptr);
      return 0;
    case WM_EXITSIZEMOVE:
      s->tracking_size = false;
      KillTimer(hwnd, 1);
      return 0;
    case WM_TIMER:
      if (wp == 1) s->frame();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      const RECT r = outer_rect(static_cast<int>(kMinW * s->scale), static_cast<int>(kMinH * s->scale),
                                static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), s->scale);
      mm->ptMinTrackSize.x = r.right - r.left;
      mm->ptMinTrackSize.y = r.bottom - r.top;
      return 0;
    }
    case 0x02E0: {  // WM_DPICHANGED
      s->scale = static_cast<float>(LOWORD(wp)) / 96.0f;
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      SetCapture(hwnd);
      s->button_down = true;
      s->last_click = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      s->pointer(DH_PTR_DOWN, lp, msg == WM_LBUTTONDBLCLK ? 2 : 1);
      return 0;
    case WM_LBUTTONUP:
      if (s->button_down) {
        s->button_down = false;
        ReleaseCapture();
        s->pointer(DH_PTR_UP, lp, 0);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (s->button_down) {  // another window took the mouse (alt-tab, a dialog): end the drag cleanly
        s->button_down = false;
        dh_app_pointer(s->core, DH_PTR_CANCEL, 0, 0, 0, 0);
      }
      return 0;
    case WM_MOUSEMOVE: s->pointer(DH_PTR_MOVE, lp, 0); return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &p);
      // Notches are multiples of 120; precision touchpads send smaller steps. 12 px per notch as on the other shells.
      const float amount = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f * 12.0f * s->scale;
      s->sync_modifiers();
      dh_app_scroll(s->core, static_cast<float>(p.x), static_cast<float>(p.y), msg == WM_MOUSEHWHEEL ? amount : 0.0f,
                    msg == WM_MOUSEWHEEL ? amount : 0.0f);
      return 0;
    }
    case WM_KEYDOWN: {
      s->sync_modifiers();
      const int key = map_key(wp);
      if (key) {
        dh_app_key(s->core, static_cast<dh_key>(key), s->modifiers());
        return 0;
      }
      break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: s->sync_modifiers(); break;
    case WM_SYSKEYDOWN: s->sync_modifiers(); break;
    case WM_KILLFOCUS:
      dh_app_modifiers(s->core, 0);
      break;
    case WM_CHAR: {
      const wchar_t c = static_cast<wchar_t>(wp);
      if (c < 0x20 || c == 0x7F) return 0;  // Enter, Tab, Backspace, Ctrl+letter arrive as keys, not text
      if (IS_HIGH_SURROGATE(c)) {
        s->high_surrogate = c;
        return 0;
      }
      if (IS_LOW_SURROGATE(c) && s->high_surrogate) {
        const wchar_t pair[2] = {s->high_surrogate, c};
        s->high_surrogate = 0;
        s->text(pair, 2);
        return 0;
      }
      s->high_surrogate = 0;
      s->text(&c, 1);
      return 0;
    }
    case WM_IME_STARTCOMPOSITION: {
      // Put the IME's composition window where the user last clicked (the text field they are typing in). The result
      // arrives as WM_CHAR when the composition is committed.
      if (HIMC imc = ImmGetContext(hwnd)) {
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = s->last_click;
        ImmSetCompositionWindow(imc, &cf);
        ImmReleaseContext(hwnd, imc);
      }
      break;
    }
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      POINT pt{};
      DragQueryPoint(drop, &pt);
      const UINT n = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
      for (UINT i = 0; i < n; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(len + 1, L'\0');
        path.resize(DragQueryFileW(drop, i, path.data(), len + 1));
        const std::string u8 = narrow(path);
        if (!u8.empty()) dh_app_open_file(s->core, u8.c_str(), static_cast<float>(pt.x), static_cast<float>(pt.y));
      }
      DragFinish(drop);
      return 0;
    }
    default: break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- platform hooks ---------------------------------------------------------------------------------------------
bool copy_out(const std::string& s, char* out, std::uint32_t cap) {
  if (cap == 0 || s.size() + 1 > cap) return false;
  std::memcpy(out, s.c_str(), s.size() + 1);
  return true;
}

bool choose_folder(void*, const char* title, char* out_path, std::uint32_t cap) {
  Com<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, dialog.put_void()))) return false;
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  if (title && *title) dialog->SetTitle(widen(title).c_str());
  if (FAILED(dialog->Show(g_shell ? g_shell->hwnd : nullptr))) return false;  // cancelled
  Com<IShellItem> item;
  if (FAILED(dialog->GetResult(item.put()))) return false;
  PWSTR path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return false;
  const bool ok = copy_out(narrow(path), out_path, cap);
  CoTaskMemFree(path);
  return ok;
}

// Removable drives, and fixed drives other than the one Windows runs from (external SSDs and many USB sticks report
// "fixed"). The list is authoritative: the core does not scan mount points of its own here.
bool list_export_targets(void*, char* out, std::uint32_t cap) {
  wchar_t system_dir[MAX_PATH] = L"";
  GetWindowsDirectoryW(system_dir, MAX_PATH);
  const wchar_t system_drive = system_dir[0] ? static_cast<wchar_t>(towupper(system_dir[0])) : L'C';
  std::string list;
  const DWORD mask = GetLogicalDrives();
  for (int i = 2; i < 26; ++i) {  // skip A: and B:
    if (!(mask & (1u << i))) continue;
    const wchar_t letter = static_cast<wchar_t>(L'A' + i);
    const wchar_t root[] = {letter, L':', L'\\', 0};
    const UINT type = GetDriveTypeW(root);
    if (type == DRIVE_REMOVABLE || (type == DRIVE_FIXED && letter != system_drive)) {
      if (!list.empty()) list += '\n';
      list += narrow(root);
    }
  }
  return copy_out(list, out, cap);
}

void show_error(const std::wstring& text) {
  MessageBoxW(nullptr, text.c_str(), L"DeckHand", MB_OK | MB_ICONERROR);
}

// A GUI-subsystem exe has no console of its own: when started from one, borrow it so --help, errors and log lines
// show up there (unless the streams are already redirected).
void attach_parent_console() {
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  attach_parent_console();
  enable_dpi_awareness();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  std::vector<std::string> args;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 0; argv && i < argc; ++i) args.push_back(narrow(argv[i]));
    if (argv) LocalFree(argv);
  }
  Options opt;
  if (!parse_args(args, opt) || opt.help) {
    std::fprintf(stderr, "usage: deckhand [--deck-a FILE] [--deck-b FILE] [FILE...] [--screenshot FILE.png [--frames N] [--size WxH]]\n");
    return opt.help ? 0 : 2;
  }

  const fs::path exe_dir = executable_dir();
  const std::string data = data_dir(exe_dir);
  const std::string resources = resource_dir(exe_dir);
  const bool offscreen = !opt.screenshot.empty();

  ShellApp ui;
  g_shell = &ui;
  dh_platform platform{};
  platform.rasterise_text = &deckhand::shell::rasterise_text;
  platform.choose_folder = &choose_folder;
  platform.list_export_targets = &list_export_targets;
  dh_config config{};
  config.data_dir = data.c_str();
  config.resource_dir = resources.c_str();
  config.log_level = log_level();
  config.touch = false;
  config.manage_audio = true;

  int client_w = opt.shot_w, client_h = opt.shot_h;
  if (offscreen) {
    // Debug path: no window, a fixed-size offscreen target (at the scale DECKHAND_SCALE names, default 1).
    const std::string s = getenv_utf8(L"DECKHAND_SCALE");
    ui.scale = s.empty() ? 1.0f : std::max(0.5f, static_cast<float>(std::atof(s.c_str())));
    if (!ui.renderer.init(nullptr, client_w, client_h)) {
      std::fprintf(stderr, "DeckHand: the renderer could not start: %s\n", ui.renderer.error().c_str());
      return 1;
    }
  } else {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"DeckHandWindow";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPEDWINDOW, ex_style = WS_EX_ACCEPTFILES;
    ui.hwnd = CreateWindowExW(ex_style, wc.lpszClassName, L"DeckHand", style, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr,
                                 instance, nullptr);
    if (!ui.hwnd) {
      show_error(L"Could not create the window.");
      return 1;
    }
    ui.scale = window_scale(ui.hwnd);
    // 1440x900 logical pixels, limited to what the monitor's work area holds.
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(ui.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int work_w = mi.rcWork.right - mi.rcWork.left, work_h = mi.rcWork.bottom - mi.rcWork.top;
    RECT outer = outer_rect(static_cast<int>(1440 * ui.scale), static_cast<int>(900 * ui.scale), style, ex_style, ui.scale);
    const int ow = std::min<int>(outer.right - outer.left, work_w), oh = std::min<int>(outer.bottom - outer.top, work_h);
    SetWindowPos(ui.hwnd, nullptr, mi.rcWork.left + (work_w - ow) / 2, mi.rcWork.top + (work_h - oh) / 2, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
    ChangeWindowMessageFilterEx(ui.hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);  // drops also reach an elevated process
    ChangeWindowMessageFilterEx(ui.hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
    RECT rc{};
    GetClientRect(ui.hwnd, &rc);
    client_w = std::max<int>(1, rc.right - rc.left);
    client_h = std::max<int>(1, rc.bottom - rc.top);
    if (!ui.renderer.init(ui.hwnd, client_w, client_h)) {
      show_error(L"DeckHand needs Direct3D 11 (a DirectX 10 or newer graphics adapter).\n\n" + widen(ui.renderer.error()));
      return 1;
    }
  }

  platform.user = nullptr;
  ui.core = dh_app_create(&platform, &config);
  if (!ui.core) {
    const std::string msg = "DeckHand could not start. See deckhand.log in " + data;
    if (offscreen) std::fprintf(stderr, "%s\n", msg.c_str());
    else show_error(widen(msg));
    return 1;
  }
  dh_app_set_display(ui.core, static_cast<float>(client_w), static_cast<float>(client_h), ui.scale, 0, 0, 0, 0);

  // --deck-a / --deck-b load a track at launch by dropping it on that half; bare files open without a target.
  if (!opt.deck_a.empty()) dh_app_open_file(ui.core, opt.deck_a.c_str(), 10.0f * ui.scale, 100.0f * ui.scale);
  if (!opt.deck_b.empty()) dh_app_open_file(ui.core, opt.deck_b.c_str(), static_cast<float>(client_w - 10), 100.0f * ui.scale);
  for (const auto& f : opt.files) dh_app_open_file(ui.core, f.c_str(), -1.0f, -1.0f);

  int exit_code = 0;
  if (offscreen) {
    const dh_frame* frame = nullptr;
    for (int i = 0; i < opt.frames; ++i) {
      frame = dh_app_frame(ui.core, 1.0 / 60.0);
      if (!frame) continue;
      ui.renderer.render(*frame, client_w, client_h);
      std::this_thread::sleep_for(std::chrono::milliseconds(16));  // lets background loads and analysis progress
    }
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    if (!frame || !ui.renderer.read_pixels(rgba, w, h) ||
        !deckhand::shell::write_png(widen(opt.screenshot).c_str(), w, h, rgba.data())) {
      std::fprintf(stderr, "DeckHand: could not write %s\n", opt.screenshot.c_str());
      exit_code = 1;
    } else {
      std::fprintf(stderr, "DeckHand: wrote %s (%dx%d) with %s%s\n", opt.screenshot.c_str(), w, h,
                   ui.renderer.adapter_name().empty() ? "unknown adapter" : ui.renderer.adapter_name().c_str(),
                   ui.renderer.software() ? " (software)" : "");
    }
  } else {
    ShowWindow(ui.hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
    UpdateWindow(ui.hwnd);
    bool running = true;
    while (running) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
          running = false;
          break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (!running) break;
      if (ui.minimized) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        continue;
      }
      const auto t0 = std::chrono::steady_clock::now();
      ui.frame();
      // Present(1) paces the loop on a real display; where it does not (software renderer, remote desktop) wait out
      // the rest of ~16 ms, waking early for input.
      const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      if (spent < 12) MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(14 - spent), QS_ALLINPUT);
    }
  }

  dh_app_destroy(ui.core);
  ui.core = nullptr;
  g_shell = nullptr;
  CoUninitialize();
  return exit_code;
}
