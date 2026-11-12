/* STUB of the shell<->core contract (real one: src/app/include/deckhand/app/capi.h).
   Delete this directory when the real header lands. MIT. */
#ifndef DECKHAND_CAPI_STUB_H
#define DECKHAND_CAPI_STUB_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct dh_app dh_app;

typedef struct { float x, y, u, v; uint32_t rgba; } dh_vertex;      /* pixels, RGBA8 */
typedef struct {
  uint32_t texture_id; float clip[4]; /* x0,y0,x1,y1 px */
  uint32_t idx_offset, idx_count, vtx_offset;
} dh_draw_cmd;
typedef enum { DH_TEX_A8 = 1, DH_TEX_RGBA8 = 2 } dh_tex_format;
typedef struct { uint32_t id; uint32_t w, h; dh_tex_format fmt; const uint8_t* pixels; } dh_tex_upload;

typedef struct {
  const dh_vertex* vtx; uint32_t vtx_count;
  const uint32_t* idx; uint32_t idx_count;
  const dh_draw_cmd* cmds; uint32_t cmd_count;
  const dh_tex_upload* uploads; uint32_t upload_count;   /* apply before drawing */
  const uint32_t* releases; uint32_t release_count;      /* textures to destroy */
  float clear_rgba[4];
  bool wants_text_input;                                  /* show keyboard */
} dh_frame;

typedef struct { uint32_t w, h; float ascent; uint8_t* alpha; } dh_bitmap; /* alpha: malloc'd, core frees */
typedef struct {
  void* user;
  bool (*rasterise_text)(void* user, const char* utf8, float px_size, dh_bitmap* out);
} dh_platform;

typedef enum { DH_PTR_DOWN, DH_PTR_MOVE, DH_PTR_UP, DH_PTR_CANCEL } dh_ptr_kind;

dh_app* dh_app_create(const dh_platform* p, const char* data_dir);
void    dh_app_destroy(dh_app*);
void    dh_app_set_display(dh_app*, float w_px, float h_px, float scale,
                           float inset_l, float inset_t, float inset_r, float inset_b);
void    dh_app_pointer(dh_app*, dh_ptr_kind, uint32_t id, float x_px, float y_px);
void    dh_app_text(dh_app*, const char* utf8);
void    dh_app_backspace(dh_app*);
void    dh_app_midi(dh_app*, const uint8_t* bytes, uint32_t len);
void    dh_app_audio_route(dh_app*, uint32_t out_channels, int master_pair, int cue_pair); /* -1 none */
void    dh_app_open_file(dh_app*, const char* path);
const dh_frame* dh_app_frame(dh_app*, double dt);

#ifdef __cplusplus
}
#endif
#endif
