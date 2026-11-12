// DeckHand Linux shell: window, input and lifecycle on SDL3 (X11 and Wayland), OpenGL 3.3 core (or ES 3.0)
// rendering. All behaviour lives in the C++ core; see src/app/include/deckhand/app/capi.h.
//
//   deckhand [--deck-a FILE] [--deck-b FILE] [FILE...] [--screenshot FILE.png [--frames N]]
//
// DECKHAND_LOG=error|warn|info|debug|trace sets the log level. A file named "portable" beside the
// binary keeps all data in DeckHandData/ next to it; otherwise data is in $XDG_DATA_HOME/DeckHand.
#include <SDL3/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "deckhand/app/capi.h"
#include "renderer.hpp"
#include "text.hpp"

namespace fs = std::filesystem;
using deckhand::shell::Renderer;

namespace {

struct Options {
  std::string deck_a, deck_b, screenshot;
  std::vector<std::string> files;
  int frames = 30;
  bool help = false;
};

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
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
    } else if (a == "--help" || a == "-h") {
      o.help = true;
    } else if (!a.empty() && a[0] == '-') {
      return false;
    } else {
      o.files.push_back(a);
    }
  }
  return true;
}

fs::path executable_dir() {
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::path(".") : exe.parent_path();
}

std::string data_dir(const fs::path& exe_dir) {
  std::error_code ec;
  if (fs::exists(exe_dir / "portable", ec)) return (exe_dir / "DeckHandData").string();
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return (fs::path(xdg) / "DeckHand").string();
  if (const char* home = std::getenv("HOME"); home && *home) return (fs::path(home) / ".local/share/DeckHand").string();
  return (exe_dir / "DeckHandData").string();
}

// Resources (controllers/, lang/, skins/) live in share/deckhand: beside the binary (a build tree or an
// unpacked archive) or in the installed prefix.
std::string resource_dir(const fs::path& exe_dir) {
  std::vector<fs::path> candidates;
  if (const char* env = std::getenv("DECKHAND_RESOURCE_DIR"); env && *env) candidates.emplace_back(env);
  candidates.push_back(exe_dir / "share/deckhand");
  candidates.push_back(exe_dir / "../share/deckhand");
  candidates.emplace_back("/usr/local/share/deckhand");
  candidates.emplace_back("/usr/share/deckhand");
  std::error_code ec;
  for (const auto& c : candidates)
    if (fs::is_directory(c / "lang", ec)) return fs::weakly_canonical(c, ec).string();
  return (exe_dir / "../share/deckhand").string();
}

int log_level() {
  const char* env = std::getenv("DECKHAND_LOG");
  const std::string s = env ? env : "info";
  static const char* names[] = {"error", "warn", "info", "debug", "trace"};
  for (int i = 0; i < 5; ++i)
    if (s == names[i]) return i;
  return 2;
}

std::uint32_t modifiers(SDL_Keymod m) {
  std::uint32_t mods = 0;
  if (m & SDL_KMOD_SHIFT) mods |= DH_MOD_SHIFT;
  if (m & SDL_KMOD_ALT) mods |= DH_MOD_ALT;
  // Ctrl and the Super key play the role Command has on macOS.
  if (m & SDL_KMOD_CTRL) mods |= DH_MOD_COMMAND | DH_MOD_CONTROL;
  if (m & SDL_KMOD_GUI) mods |= DH_MOD_COMMAND;
  return mods;
}

int map_key(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return DH_KEY_UP;
    case SDLK_DOWN: return DH_KEY_DOWN;
    case SDLK_LEFT: return DH_KEY_LEFT;
    case SDLK_RIGHT: return DH_KEY_RIGHT;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return DH_KEY_ENTER;
    case SDLK_ESCAPE: return DH_KEY_ESCAPE;
    case SDLK_BACKSPACE: return DH_KEY_BACKSPACE;
    case SDLK_TAB: return DH_KEY_TAB;
    default: return 0;
  }
}

struct Display {
  int w_px = 0, h_px = 0;
  float scale = 1, density = 1;  // density: pixels per window coordinate; scale: UI scale
};

Display query_display(SDL_Window* window) {
  Display d;
  SDL_GetWindowSizeInPixels(window, &d.w_px, &d.h_px);
  int w = 0, h = 0;
  SDL_GetWindowSize(window, &w, &h);
  d.density = w > 0 ? static_cast<float>(d.w_px) / static_cast<float>(w) : 1.0f;
  const float s = SDL_GetWindowDisplayScale(window);  // pixel density x desktop scale (X11 Xft.dpi etc.)
  d.scale = s > 0 ? s : d.density;
  return d;
}

SDL_Window* create_window(bool& gles, SDL_GLContext& context) {
  const SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  for (int attempt = 0; attempt < 2; ++attempt) {
    gles = attempt == 1;
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 0);  // values pass straight through, as on Metal
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    if (gles) {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    } else {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    }
    SDL_Window* window = SDL_CreateWindow("DeckHand", 1440, 900, flags);
    if (!window) continue;
    context = SDL_GL_CreateContext(window);
    if (context) return window;
    SDL_DestroyWindow(window);
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt) || opt.help) {
    std::fprintf(stderr,
                 "usage: deckhand [--deck-a FILE] [--deck-b FILE] [FILE...] [--screenshot FILE.png [--frames N]]\n");
    return opt.help ? 0 : 2;
  }

  SDL_SetAppMetadata("DeckHand", "0.1.0", "org.deckhand.DeckHand");
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "DeckHand: could not initialise video: %s\n", SDL_GetError());
    return 1;
  }
  bool gles = false;
  SDL_GLContext gl = nullptr;
  SDL_Window* window = create_window(gles, gl);
  if (!window) {
    std::fprintf(stderr, "DeckHand needs OpenGL 3.3 or OpenGL ES 3.0: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_SetWindowMinimumSize(window, 1100, 700);
  const bool vsync = SDL_GL_SetSwapInterval(1);
  SDL_StartTextInput(window);

  Renderer renderer;
  if (!renderer.init(gles)) {
    std::fprintf(stderr, "DeckHand: the renderer could not start\n");
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  const fs::path exe_dir = executable_dir();
  const std::string data = data_dir(exe_dir);
  const std::string resources = resource_dir(exe_dir);
  dh_platform platform{nullptr, &deckhand::shell::rasterise_text};
  dh_config config{};
  config.data_dir = data.c_str();
  config.resource_dir = resources.c_str();
  config.log_level = log_level();
  config.touch = false;
  config.manage_audio = true;
  dh_app* core = dh_app_create(&platform, &config);
  if (!core) {
    std::fprintf(stderr, "DeckHand could not start. See deckhand.log in %s\n", data.c_str());
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  Display disp = query_display(window);
  auto set_display = [&] {
    disp = query_display(window);
    dh_app_set_display(core, static_cast<float>(disp.w_px), static_cast<float>(disp.h_px), disp.scale, 0, 0, 0, 0);
  };
  set_display();

  // --deck-a / --deck-b load a track at launch by dropping it on that half; bare files open without a target.
  int win_w = 0, win_h = 0;
  SDL_GetWindowSize(window, &win_w, &win_h);
  if (!opt.deck_a.empty()) dh_app_open_file(core, opt.deck_a.c_str(), 10.0f * disp.density, 100.0f * disp.density);
  if (!opt.deck_b.empty())
    dh_app_open_file(core, opt.deck_b.c_str(), static_cast<float>(win_w - 10) * disp.density, 100.0f * disp.density);
  for (const auto& f : opt.files) dh_app_open_file(core, f.c_str(), -1.0f, -1.0f);

  auto px = [&](float v) { return v * disp.density; };
  Uint64 last = SDL_GetPerformanceCounter();
  const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
  int frames = 0;
  bool running = true;
  int exit_code = 0;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          if (e.button.button == SDL_BUTTON_LEFT)
            dh_app_pointer(core, DH_PTR_DOWN, 0, px(e.button.x), px(e.button.y), e.button.clicks);
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
          if (e.button.button == SDL_BUTTON_LEFT) dh_app_pointer(core, DH_PTR_UP, 0, px(e.button.x), px(e.button.y), 0);
          break;
        case SDL_EVENT_MOUSE_MOTION: dh_app_pointer(core, DH_PTR_MOVE, 0, px(e.motion.x), px(e.motion.y), 0); break;
        case SDL_EVENT_MOUSE_WHEEL: {
          // Wheel notches are whole numbers; touchpads report fractions of one. Match the macOS 12 px per notch.
          const float unit = 12.0f * disp.scale;
          dh_app_scroll(core, px(e.wheel.mouse_x), px(e.wheel.mouse_y), e.wheel.x * unit, e.wheel.y * unit);
          break;
        }
        case SDL_EVENT_KEY_DOWN: {
          const int key = map_key(e.key.key);
          if (key) dh_app_key(core, static_cast<dh_key>(key), modifiers(e.key.mod));
          break;
        }
        case SDL_EVENT_TEXT_INPUT:
          if (e.text.text && *e.text.text) dh_app_text(core, e.text.text);
          break;
        case SDL_EVENT_DROP_FILE:
          if (e.drop.data) dh_app_open_file(core, e.drop.data, px(e.drop.x), px(e.drop.y));
          break;
        default: break;
      }
    }
    if (!running) break;
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(50);
      continue;
    }

    set_display();
    const Uint64 now = SDL_GetPerformanceCounter();
    const double dt = static_cast<double>(now - last) / freq;
    last = now;
    const dh_frame* frame = dh_app_frame(core, dt);
    if (!frame) continue;
    renderer.render(*frame, disp.w_px, disp.h_px);
    if (frame->wants_text_input) SDL_StartTextInput(window);

    ++frames;
    if (!opt.screenshot.empty() && frames >= opt.frames) {
      std::vector<std::uint8_t> rgba;
      if (!renderer.read_pixels(disp.w_px, disp.h_px, rgba) ||
          !deckhand::shell::write_png(opt.screenshot.c_str(), disp.w_px, disp.h_px, rgba.data())) {
        std::fprintf(stderr, "DeckHand: could not write %s\n", opt.screenshot.c_str());
        exit_code = 1;
      }
      running = false;
    }
    SDL_GL_SwapWindow(window);
    if (!vsync) SDL_Delay(16);
  }

  dh_app_destroy(core);
  SDL_GL_DestroyContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return exit_code;
}
