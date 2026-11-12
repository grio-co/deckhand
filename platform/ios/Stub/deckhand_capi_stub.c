/* Stub core: test pattern, text via platform rasteriser, touch widgets. */
#include "deckhand_capi_stub.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAXV 4096
#define MAXC 128
#define MAXT 512
typedef struct { char s[96]; float px; uint32_t id, w, h; float asc; uint8_t* a; bool used; } txt;
struct dh_app {
  dh_platform p; float W, H, sc, il, it, ir, ib;
  dh_vertex v[MAXV]; uint32_t nv; uint32_t idx[MAXV * 3]; uint32_t ni;
  dh_draw_cmd c[MAXC]; uint32_t nc;
  dh_tex_upload up[MAXT + 1]; uint32_t nup;
  txt t[MAXT]; uint32_t next_id; uint8_t white[4]; bool white_sent;
  dh_frame f;
  float touch[10][3]; /* active,x,y */
  bool playing; float xfade; bool search_focus; char query[64]; int midi_count; char file[96];
  uint32_t ch; int mp, cp;
};

dh_app* dh_app_create(const dh_platform* p, const char* d) {
  (void)d; dh_app* a = calloc(1, sizeof *a); a->p = *p; a->next_id = 2; a->xfade = .5f;
  memset(a->white, 255, 4); return a;
}
void dh_app_destroy(dh_app* a) { for (int i = 0; i < MAXT; i++) free(a->t[i].a); free(a); }
void dh_app_set_display(dh_app* a, float w, float h, float s, float l, float t, float r, float b) {
  a->W = w; a->H = h; a->sc = s; a->il = l; a->it = t; a->ir = r; a->ib = b;
}
static bool in(float x, float y, float rx, float ry, float rw, float rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}
static void layout(dh_app* a, float* px, float* py, float* pw, float* ph, float* sx, float* sy, float* sw, float* sh,
                   float* fx, float* fy, float* fw, float* fh) {
  float u = a->sc, x = a->il + 16 * u, w = a->W - a->il - a->ir - 32 * u, y = a->it + 90 * u;
  *px = x; *py = y; *pw = 140 * u; *ph = 56 * u;
  *sx = x; *sy = y + 80 * u; *sw = w; *sh = 48 * u;           /* search field */
  *fx = x; *fy = y + 160 * u; *fw = w; *fh = 56 * u;          /* crossfader */
}
void dh_app_pointer(dh_app* a, dh_ptr_kind k, uint32_t id, float x, float y) {
  if (id < 10) { a->touch[id][0] = (k == DH_PTR_DOWN || k == DH_PTR_MOVE); a->touch[id][1] = x; a->touch[id][2] = y; }
  float px, py, pw, ph, sx, sy, sw, sh, fx, fy, fw, fh; layout(a, &px, &py, &pw, &ph, &sx, &sy, &sw, &sh, &fx, &fy, &fw, &fh);
  if (k == DH_PTR_DOWN) {
    if (in(x, y, px, py, pw, ph)) a->playing = !a->playing;
    a->search_focus = in(x, y, sx, sy, sw, sh);
  }
  if ((k == DH_PTR_DOWN || k == DH_PTR_MOVE) && in(x, y, fx - 20, fy - 20, fw + 40, fh + 40)) {
    float v = (x - fx) / fw; a->xfade = v < 0 ? 0 : v > 1 ? 1 : v;
  }
}
void dh_app_text(dh_app* a, const char* s) { size_t n = strlen(a->query); if (n + strlen(s) < sizeof a->query) strcat(a->query, s); }
void dh_app_backspace(dh_app* a) {
  size_t n = strlen(a->query); if (!n) return; n--;
  while (n && (a->query[n] & 0xC0) == 0x80) n--;
  a->query[n] = 0;
}
void dh_app_midi(dh_app* a, const uint8_t* b, uint32_t n) { (void)b; (void)n; a->midi_count++; }
void dh_app_audio_route(dh_app* a, uint32_t c, int m, int cu) { a->ch = c; a->mp = m; a->cp = cu; }
void dh_app_open_file(dh_app* a, const char* p) { snprintf(a->file, sizeof a->file, "%s", p); }

static void cmd(dh_app* a, uint32_t tex, uint32_t i0, const float* clip) {
  if (a->nc >= MAXC) return;
  dh_draw_cmd* c = &a->c[a->nc++]; c->texture_id = tex; c->idx_offset = i0; c->idx_count = a->ni - i0; c->vtx_offset = 0;
  memcpy(c->clip, clip, 16);
}
static void quad(dh_app* a, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t col) {
  if (a->nv + 4 > MAXV) return; uint32_t b = a->nv;
  a->v[a->nv++] = (dh_vertex){x0, y0, u0, v0, col}; a->v[a->nv++] = (dh_vertex){x1, y0, u1, v0, col};
  a->v[a->nv++] = (dh_vertex){x1, y1, u1, v1, col}; a->v[a->nv++] = (dh_vertex){x0, y1, u0, v1, col};
  uint32_t q[6] = {0, 1, 2, 0, 2, 3}; for (int i = 0; i < 6; i++) a->idx[a->ni++] = b + q[i];
}
static void rect(dh_app* a, float x, float y, float w, float h, uint32_t col, const float* clip) {
  uint32_t i0 = a->ni; quad(a, x, y, x + w, y + h, 0, 0, 1, 1, col); cmd(a, 1, i0, clip);
}
static void text(dh_app* a, const char* s, float x, float y, float pt, uint32_t col, const float* clip) {
  float px = pt * a->sc; txt* t = NULL;
  for (int i = 0; i < MAXT; i++) if (a->t[i].used && a->t[i].px == px && !strcmp(a->t[i].s, s)) { t = &a->t[i]; break; }
  if (!t) {
    for (int i = 0; i < MAXT; i++) if (!a->t[i].used) { t = &a->t[i]; break; }
    if (!t) return;
    dh_bitmap bm = {0};
    if (!a->p.rasterise_text(a->p.user, s, px, &bm) || !bm.alpha) return;
    snprintf(t->s, sizeof t->s, "%s", s); t->px = px; t->w = bm.w; t->h = bm.h; t->asc = bm.ascent; t->a = bm.alpha;
    t->id = a->next_id++; t->used = true;
    a->up[a->nup++] = (dh_tex_upload){t->id, t->w, t->h, DH_TEX_A8, t->a};
  }
  uint32_t i0 = a->ni; quad(a, x, y, x + t->w, y + t->h, 0, 0, 1, 1, col); cmd(a, t->id, i0, clip);
}
#define C(r, g, b) (0xFF000000u | ((b) << 16) | ((g) << 8) | (r))
const dh_frame* dh_app_frame(dh_app* a, double dt) {
  (void)dt; a->nv = a->ni = a->nc = a->nup = 0;
  if (!a->white_sent) { a->up[a->nup++] = (dh_tex_upload){1, 1, 1, DH_TEX_RGBA8, a->white}; a->white_sent = true; }
  float u = a->sc, full[4] = {0, 0, a->W, a->H};
  float px, py, pw, ph, sx, sy, sw, sh, fx, fy, fw, fh; layout(a, &px, &py, &pw, &ph, &sx, &sy, &sw, &sh, &fx, &fy, &fw, &fh);
  /* test gradient bar */
  uint32_t i0 = a->ni; float bx = a->il, bw = (a->W - a->il - a->ir) / 8;
  for (int i = 0; i < 8; i++) { uint32_t col = C(32 * i + 20, 200 - 20 * i, 255 - 25 * i); quad(a, bx + i * bw, 0, bx + (i + 1) * bw, a->it + 8 * u, 0, 0, 1, 1, col); }
  cmd(a, 1, i0, full);
  text(a, "DeckHand (iOS stub core)", a->il + 16 * u, a->it + 12 * u, 22, C(235, 235, 240), full);
  text(a, "مرحبا — こんにちは — Привет — 你好", a->il + 16 * u, a->it + 44 * u, 16, C(150, 200, 255), full);
  rect(a, px, py, pw, ph, a->playing ? C(40, 180, 90) : C(70, 70, 90), full);
  text(a, a->playing ? "Pause" : "Play", px + 24 * u, py + 14 * u, 20, C(255, 255, 255), full);
  rect(a, sx, sy, sw, sh, a->search_focus ? C(50, 60, 90) : C(35, 38, 50), full);
  float cl[4] = {sx, sy, sx + sw, sy + sh};
  text(a, a->query[0] ? a->query : "Search…", sx + 12 * u, sy + 12 * u, 18, a->query[0] ? C(255, 255, 255) : C(120, 120, 140), cl);
  rect(a, fx, fy + 24 * u, fw, 8 * u, C(60, 60, 80), full);
  rect(a, fx + a->xfade * fw - 14 * u, fy, 28 * u, 56 * u, C(240, 160, 40), full);
  char buf[96];
  snprintf(buf, sizeof buf, "xfade %.2f  midi msgs %d  out ch %u", a->xfade, a->midi_count, a->ch);
  text(a, buf, fx, fy + 80 * u, 14, C(180, 180, 190), full);
  snprintf(buf, sizeof buf, "master pair %d  cue pair %d", a->mp, a->cp);
  text(a, buf, fx, fy + 104 * u, 14, C(180, 180, 190), full);
  if (a->file[0]) text(a, a->file, fx, fy + 128 * u, 12, C(180, 220, 180), full);
  for (int i = 0; i < 10; i++) if (a->touch[i][0]) rect(a, a->touch[i][1] - 24 * u, a->touch[i][2] - 24 * u, 48 * u, 48 * u, 0x80FFFF40u, full);
  a->f = (dh_frame){a->v, a->nv, a->idx, a->ni, a->c, a->nc, a->up, a->nup, NULL, 0, {.06f, .06f, .09f, 1}, a->search_focus};
  return &a->f;
}
