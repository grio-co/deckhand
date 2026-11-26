/* DeckHand shell interface (C). A platform shell creates the app, forwards input,
   and each frame renders the returned draw list with one textured-triangle pipeline.
   All coordinates are pixels. All calls must come from one thread (the UI thread). */
#ifndef DECKHAND_CAPI_H
#define DECKHAND_CAPI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dh_app dh_app;

typedef struct { float x, y, u, v; uint32_t rgba; } dh_vertex; /* rgba bytes in memory: R,G,B,A */

typedef struct {
  uint32_t texture_id;
  float clip[4]; /* scissor: x0, y0, x1, y1 */
  uint32_t idx_offset, idx_count, vtx_offset;
} dh_draw_cmd;

typedef enum { DH_TEX_A8 = 1, DH_TEX_RGBA8 = 2 } dh_tex_format;

/* Update a region of a texture; create it at tex_w x tex_h if the id is new. Id 1 is the glyph atlas (A8);
   the core assigns ids from 2 up to the images it uploads (RGBA8, straight alpha, sampled bilinearly).
   For A8 textures the shader output is vertex colour with alpha multiplied by the texel; for RGBA8 textures
   it is vertex colour times the texel (white vertices draw the image unchanged). */
typedef struct {
  uint32_t id;
  uint32_t tex_w, tex_h;
  dh_tex_format fmt;
  uint32_t x, y, w, h;   /* region */
  uint32_t stride;       /* bytes per row at `pixels` */
  const uint8_t* pixels; /* first pixel of the region; valid until the next dh_app_frame */
} dh_tex_upload;

typedef struct {
  const dh_vertex* vtx;      uint32_t vtx_count;
  const uint32_t* idx;       uint32_t idx_count;
  const dh_draw_cmd* cmds;   uint32_t cmd_count;
  const dh_tex_upload* uploads; uint32_t upload_count; /* apply before drawing */
  const uint32_t* releases;  uint32_t release_count;   /* textures to destroy */
  float clear_rgba[4];
  bool wants_text_input; /* a text field has focus (show the keyboard on touch devices) */
} dh_frame;

/* Text rasterisation is supplied by the platform so every script and writing direction
   works. `alpha` is allocated with malloc() by the platform; the core frees it. */
typedef struct { uint32_t w, h; float ascent; uint8_t* alpha; } dh_bitmap;

typedef struct {
  void* user;
  bool (*rasterise_text)(void* user, const char* utf8, float px_size, int weight /*0 regular, 1 bold*/,
                         dh_bitmap* out);

  /* ---- Optional members, added after the first release. A shell that zero-initialises the struct and leaves
     them NULL gets the default behaviour; new members are only ever appended. ---- */

  /* Open the system's Bluetooth MIDI pairing UI. NULL = the platform has none: the core hides its button. */
  void (*show_bluetooth_midi)(void* user);

  /* Let the user pick a folder (modal is fine; called on the UI thread from within dh_app_frame). Write the
     UTF-8 path, NUL-terminated, into out_path[cap] and return true; false = cancelled. `title` is a prompt in
     the active language. NULL = the core shows no "Choose..." buttons and the user types the path. */
  bool (*choose_folder)(void* user, const char* title, char* out_path, uint32_t cap);

  /* Where "Export to USB" can write: removable drives and the like. Write the folder paths, one per line
     ('\n'-separated, UTF-8, NUL-terminated), into out[cap]; return true if the list is authoritative (it may
     be empty) or false to let the core fall back to its own scan of /Volumes, /media, /run/media and /mnt. */
  bool (*list_export_targets)(void* user, char* out, uint32_t cap);
} dh_platform;

typedef struct {
  const char* data_dir;      /* library, settings, logs; created if missing */
  const char* resource_dir;  /* read-only resources (controllers/), may be NULL */
  int log_level;             /* 0 error .. 3 debug, 4 trace */
  bool touch;                /* touch-first device (larger hit targets; reserved) */
  bool manage_audio;         /* false: the shell owns the audio session (iOS); the core just opens the route */
} dh_config;

typedef enum { DH_PTR_DOWN, DH_PTR_MOVE, DH_PTR_UP, DH_PTR_CANCEL } dh_ptr_kind;
typedef enum {
  DH_KEY_UP = 1, DH_KEY_DOWN, DH_KEY_LEFT, DH_KEY_RIGHT, DH_KEY_ENTER, DH_KEY_ESCAPE, DH_KEY_BACKSPACE, DH_KEY_TAB
} dh_key;
enum { DH_MOD_SHIFT = 1, DH_MOD_COMMAND = 2, DH_MOD_ALT = 4, DH_MOD_CONTROL = 8 };

dh_app* dh_app_create(const dh_platform* platform, const dh_config* config);
void    dh_app_destroy(dh_app* app);

void    dh_app_set_display(dh_app* app, float w_px, float h_px, float scale,
                           float inset_l, float inset_t, float inset_r, float inset_b);
/* Optional: the keyboard modifiers (DH_MOD_* bits) currently held, sent whenever they change or before each
   pointer event. Shift gives fine control while dragging knobs and faders, and shift-click stops a sampler slot. */
void    dh_app_modifiers(dh_app* app, uint32_t mods);
void    dh_app_pointer(dh_app* app, dh_ptr_kind kind, uint32_t id, float x_px, float y_px, int clicks);
void    dh_app_scroll(dh_app* app, float x_px, float y_px, float dx_px, float dy_px);
/* Trackpad pinch: the change in magnification since the last call (> 0 spreads). Zooms the waveform under the
   pointer. Shells whose system reports a pinch as Ctrl + wheel need not call it. */
void    dh_app_zoom(dh_app* app, float x_px, float y_px, float magnification);
void    dh_app_key(dh_app* app, dh_key key, uint32_t mods);
void    dh_app_text(dh_app* app, const char* utf8);
/* A file was dropped or opened. x/y are where it was dropped (negative if not applicable). */
void    dh_app_open_file(dh_app* app, const char* path, float x_px, float y_px);
/* The audio route changed (device plugged, session interruption ended): reopen the output. */
void    dh_app_audio_route(dh_app* app);

/* Build the next frame. The returned pointers stay valid until the next call. */
const dh_frame* dh_app_frame(dh_app* app, double dt_seconds);


/* ======================================================================================================================
   Free-form and multi-window skins (optional, additive; see docs/SHELL-WINDOWS.md).

   A shell that can show several native windows, transparent and shaped, announces it once after dh_app_create():
       dh_app_set_capabilities(app, DH_CAP_MULTI_WINDOW | DH_CAP_TRANSPARENT | DH_CAP_CLICK_THROUGH);
   Without that call (or with the bits clear) nothing below is used: dh_app_frame() draws everything into the one
   rectangular window as before, modules docked in a default arrangement over a solid backdrop.

   Then, once per display tick (60 Hz):
       dh_app_begin_frame(app, dt);                        // engine snapshot, window set, arrangement
       n = dh_app_windows(app, infos, cap);               // windows the core wants (also hidden modules, for the menu)
       for each visible, non-occluded window w:
           dh_app_window_display(app, w.id, px_w, px_h, backing_scale);   // when its pixel size or scale changed
           f = dh_app_window_frame(app, w.id);            // draw list of that window (no uploads)
       u = dh_app_end_frame(app);                         // textures: apply u->uploads / u->releases BEFORE drawing any f
       render every f with its clear colour (alpha 0 for transparent windows)

   Textures share ONE namespace (glyph atlas id 1, skin images from 2): upload each once, draw from any window. Vertices
   are straight-alpha RGBA as always; the shell's blend (src.a, 1-src.a for colour, 1, 1-src.a for alpha) leaves the
   framebuffer premultiplied, which is what a non-opaque CAMetalLayer / DirectComposition surface expects. The core
   bleeds colour into transparent texels of skin images, so bilinear sampling leaves no dark fringes.

   Coordinates: window pixels for pointer events and hit tests (origin top-left of the window's content), desktop points
   for window positions (origin top-left of the primary display, y down; the shell converts from its native space).
   ====================================================================================================================== */
enum { DH_CAP_MULTI_WINDOW = 1, DH_CAP_TRANSPARENT = 2, DH_CAP_CLICK_THROUGH = 4 };
void dh_app_set_capabilities(dh_app* app, uint32_t caps);

enum {
  DH_WIN_VISIBLE = 1,       /* shown (a hidden module is listed so the shell can offer it in its Window menu) */
  DH_WIN_MAIN = 2,          /* the main window: in the Dock / window list, closing it quits */
  DH_WIN_BORDERLESS = 4,    /* no system title bar or frame: the skin draws everything (chrome=none) */
  DH_WIN_TRANSPARENT = 8,   /* non-opaque, clear background: draw with clear alpha 0 and no backdrop */
  DH_WIN_SHAPED = 16,       /* clicks on transparent parts must fall through: use dh_app_hit_test */
  DH_WIN_FLOATING = 32,     /* stays above other windows (always on top) */
  DH_WIN_RESIZABLE = 64,
  DH_WIN_NO_SHADOW = 128,   /* no window shadow (shadow=none); otherwise the system shadow may follow the alpha shape */
  DH_WIN_ASPECT_LOCK = 256, /* keep width:height while resizing */
  DH_WIN_POPUP = 512,       /* a pop-up anchored to the control that opened it: closes when it loses focus */
  DH_WIN_POS_VALID = 1024,  /* x, y are meaningful (restored or computed); otherwise the shell picks a position */
  DH_WIN_PANEL = 2048       /* an application panel (settings ...), not a skin window */
};
typedef struct {
  uint32_t id;              /* stable for the lifetime of the window */
  char name[32];            /* skin window id ("main", "browser") or "panel" */
  char role[24];            /* main, browser, sampler, effects, mixer, settings, deck3_4, a free name, or the panel kind */
  char title[128];          /* UTF-8, already translated */
  uint32_t flags;          /* DH_WIN_* */
  float width, height;     /* content size in points */
  float x, y;              /* desktop points of the content's top-left (valid with DH_WIN_POS_VALID) */
  uint32_t move_seq;       /* changes whenever the core moved or resized the window: apply x, y, width, height again */
  float min_w, min_h, max_w, max_h; /* points; 0 = none */
  float user_scale;        /* scale=fit windows: width = base * user_scale */
  uint32_t z;              /* stacking order hint: higher is in front of lower */
} dh_window_info;

/* Fills up to `cap` entries and returns how many windows the core has (which may exceed `cap`). */
uint32_t dh_app_windows(dh_app* app, dh_window_info* out, uint32_t cap);
void     dh_app_begin_frame(dh_app* app, double dt_seconds);
void     dh_app_window_display(dh_app* app, uint32_t window, float w_px, float h_px, float backing_scale);
const dh_frame* dh_app_window_frame(dh_app* app, uint32_t window);  /* NULL when unknown / hidden; no uploads in it */
const dh_frame* dh_app_end_frame(dh_app* app);                       /* only uploads / releases are set */

void dh_app_window_pointer(dh_app* app, uint32_t window, dh_ptr_kind kind, uint32_t id, float x_px, float y_px, int clicks);
void dh_app_window_scroll(dh_app* app, uint32_t window, float x_px, float y_px, float dx_px, float dy_px);
void dh_app_window_zoom(dh_app* app, uint32_t window, float x_px, float y_px, float magnification);
void dh_app_window_open_file(dh_app* app, uint32_t window, const char* path, float x_px, float y_px);

typedef enum { DH_HIT_NONE = 0, DH_HIT_DRAG = 1, DH_HIT_CONTROL = 2, DH_HIT_RESIZE = 3, DH_HIT_SOLID = 4 } dh_hit;
/* What a click at (x_px, y_px) of the window would do: NONE = transparent, let it fall through to what is behind;
   DRAG = move the window (dh_app_window_drag); CONTROL = deliver it as a pointer event; RESIZE = a resize grip
   (dh_app_window_drag with kind RESIZE); SOLID = opaque but inert: swallow it. */
dh_hit dh_app_hit_test(dh_app* app, uint32_t window, float x_px, float y_px);

typedef enum { DH_DRAG_MOVE = 0, DH_DRAG_RESIZE = 1 } dh_drag_kind;
typedef enum { DH_DRAG_BEGIN = 0, DH_DRAG_UPDATE = 1, DH_DRAG_END = 2 } dh_drag_phase;
/* Core-driven window dragging (so snapping and group moves work): on a DRAG / RESIZE hit send BEGIN with the pointer's
   desktop position in points, UPDATE on every mouse move, END on release; apply the windows' new frames from the next
   dh_app_windows(). mods = DH_MOD_* (shift = move this window alone, no snapping). */
void dh_app_window_drag(dh_app* app, uint32_t window, dh_drag_kind kind, dh_drag_phase phase, float desk_x, float desk_y, uint32_t mods);

typedef enum {
  DH_WE_MOVED = 1,     /* a = x, b = y (desktop points of the content's top-left) */
  DH_WE_RESIZED = 2,   /* a = width, b = height (points) */
  DH_WE_FOCUS = 3,     /* a = 1 key window gained focus, 0 lost it */
  DH_WE_CLOSE = 4,     /* the user closed the window (a module is hidden; a panel is dismissed; main quits via the shell) */
  DH_WE_MINIMIZED = 5  /* a = 1 minimised, 0 restored */
} dh_window_event;
void dh_app_window_event(dh_app* app, uint32_t window, dh_window_event kind, float a, float b);
/* Show or hide a module from the shell's Window menu. */
void dh_app_window_set_visible(dh_app* app, uint32_t window, bool visible);
/* "Reset window arrangement" menu item. */
void dh_app_windows_reset(dh_app* app);

/* Keyboard shortcuts (customisable: resources/keymaps/default.dhkeys + <data>/keymap.dhkeys, docs/SHORTCUTS.md).
   One key event. `physical` is the USB HID keyboard usage (page 0x07: A = 4 .. Z = 29, 1 = 30 .. 0 = 39, Enter 40,
   Escape 41, Backspace 42, Tab 43, Space 44, F1 = 58 ..., arrows 79..82; SDL3 scancodes are these values) or 0 if the
   shell only knows the character. `character` is the Unicode code point the key types without modifiers (0 = none).
   mods = DH_MOD_*. Send key-up events too (is_down = false): keys bound as "hold" (cue, hot cues) need them.
   Returns true when a shortcut took the key: do not then send the key's text (dh_app_text) or dh_app_key for it.
   Returns false for keys that are not bound or while a text field has the keyboard; then continue as before with
   dh_app_key / dh_app_text. Existing shells keep working unchanged without calling this. */
bool dh_app_key_event(dh_app* app, uint32_t physical, uint32_t character, uint32_t mods, bool is_down, bool is_repeat);
/* The window lost focus or the keyboard moved elsewhere: release every key still held (ends held cues). */
void dh_app_key_release_all(dh_app* app);

#ifdef __cplusplus
}
#endif
#endif
