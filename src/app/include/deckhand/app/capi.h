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
void    dh_app_key(dh_app* app, dh_key key, uint32_t mods);
void    dh_app_text(dh_app* app, const char* utf8);
/* A file was dropped or opened. x/y are where it was dropped (negative if not applicable). */
void    dh_app_open_file(dh_app* app, const char* path, float x_px, float y_px);
/* The audio route changed (device plugged, session interruption ended): reopen the output. */
void    dh_app_audio_route(dh_app* app);

/* Build the next frame. The returned pointers stay valid until the next call. */
const dh_frame* dh_app_frame(dh_app* app, double dt_seconds);

#ifdef __cplusplus
}
#endif
#endif
