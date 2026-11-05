#include "ui.h"
#include "font.h"
#include "input.h"
#include "sfx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/net/netctl.h>

#ifndef M_PI
#define M_PI 3.14159265358979f
#endif

/* ---------------------------------------------------------------- */
/* Themes — "Green" is sampled from the Settings app itself.          */
/* ---------------------------------------------------------------- */

const ui_theme g_themes[NUM_THEMES] = {
  { "Green",    C_RGBA(0x0b, 0x84, 0x00, 255), C_RGBA(0x1a, 0x62, 0x00, 255), C_RGBA(0x14, 0xa0, 0x08, 255), C_RGBA(0x24, 0xf2, 0x20, 255), C_RGBA(0x9a, 0xff, 0x6a, 255) },
  { "Blue",     C_RGBA(0x00, 0x5e, 0xc4, 255), C_RGBA(0x00, 0x36, 0x86, 255), C_RGBA(0x04, 0x56, 0xb8, 255), C_RGBA(0x2a, 0xb4, 0xff, 255), C_RGBA(0x8a, 0xd6, 0xff, 255) },
  { "Purple",   C_RGBA(0x6a, 0x22, 0xb0, 255), C_RGBA(0x40, 0x10, 0x78, 255), C_RGBA(0x5c, 0x1c, 0xa0, 255), C_RGBA(0xc0, 0x6a, 0xff, 255), C_RGBA(0xe2, 0xb0, 0xff, 255) },
  { "Orange",   C_RGBA(0xc8, 0x5a, 0x00, 255), C_RGBA(0x88, 0x34, 0x00, 255), C_RGBA(0xb0, 0x4c, 0x00, 255), C_RGBA(0xff, 0xae, 0x2a, 255), C_RGBA(0xff, 0xd2, 0x8a, 255) },
  { "Graphite", C_RGBA(0x3c, 0x3e, 0x46, 255), C_RGBA(0x1c, 0x1d, 0x22, 255), C_RGBA(0x2a, 0x2b, 0x31, 255), C_RGBA(0x74, 0x78, 0x86, 255), C_RGBA(0x6c, 0xc4, 0xff, 255) },
};
const ui_theme *T = &g_themes[0];
void ui_set_theme(int i) { T = &g_themes[(i % NUM_THEMES + NUM_THEMES) % NUM_THEMES]; }

static unsigned g_frames;
static uint64_t g_t0, g_tlast;
static float g_time, g_dt = 1 / 60.0f;
int g_focus_visible = 0;
int g_ui_input = 1;

void ui_init(void) {
  font_init();
  g_t0 = g_tlast = sceKernelGetProcessTimeWide();
}

void ui_shutdown(void) { font_shutdown(); }

void ui_begin_frame(void) {
  g_frames++;
  uint64_t now = sceKernelGetProcessTimeWide();
  g_dt = (now - g_tlast) / 1e6f;
  if (g_dt > 0.1f) g_dt = 0.1f;
  if (g_dt <= 0) g_dt = 1 / 60.0f;
  g_tlast = now;
  g_time = (now - g_t0) / 1e6f;
  if (g_in.pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) g_focus_visible = 1;
  if (g_in.touch_down) g_focus_visible = 0;
}

unsigned ui_frame(void) { return g_frames; }
float ui_time(void) { return g_time; }
float ui_dt(void) { return g_dt; }

float ease_out(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; float u = 1 - t; return 1 - u * u * u; }
float ease_in_out(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }
float approach(float cur, float target, float rate) { return cur + (target - cur) * (1 - expf(-rate * g_dt)); }

uint32_t col_alpha(uint32_t c, float a) {
  if (a < 0) a = 0;
  if (a > 1) a = 1;
  uint32_t al = (uint32_t)(((c >> 24) & 0xFF) * a + 0.5f);
  return (c & 0x00FFFFFF) | (al << 24);
}

uint32_t col_lerp(uint32_t a, uint32_t b, float t) {
  if (t <= 0) return a;
  if (t >= 1) return b;
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    float ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
    out |= ((uint32_t)(ca + (cb - ca) * t + 0.5f) & 0xFF) << sh;
  }
  return out;
}

/* ---------------------------------------------------------------- */
/* Transform stack                                                   */
/* ---------------------------------------------------------------- */

#define PERSP_D 760.0f
#define XF_MAX 8
static ui_xform g_xs[XF_MAX];
static int g_xn;
static ui_xform X = { 0, 0, 1, 0, 0, 0, 1 };
static float X_cos = 1, X_sin = 0;
static int X_3d = 0, X_flat_int = 1;

ui_xform ui_xform_identity(void) { ui_xform x = { 0, 0, 1, 0, 0, 0, 1 }; return x; }

static void xf_update(void) {
  X_cos = cosf(X.theta);
  X_sin = sinf(X.theta);
  X_3d = fabsf(X.theta) > 0.0005f || fabsf(X.depth) > 0.01f || fabsf(X.scale - 1) > 0.0005f;
  X_flat_int = !X_3d && fabsf(X.tx - roundf(X.tx)) < 0.01f && fabsf(X.ty - roundf(X.ty)) < 0.01f;
}

void ui_push_xform(ui_xform n) {
  if (g_xn >= XF_MAX) return;
  g_xs[g_xn++] = X;
  /* translations add, alpha multiplies; 3D params come from whichever level sets them */
  ui_xform c = X;
  c.tx += n.tx;
  c.ty += n.ty;
  c.alpha *= n.alpha;
  if (n.theta != 0 || n.depth != 0) { c.theta = n.theta; c.axis_x = n.axis_x; c.depth = n.depth; }
  c.scale *= n.scale;
  X = c;
  xf_update();
}

void ui_pop_xform(void) {
  if (g_xn > 0) X = g_xs[--g_xn];
  xf_update();
}

static inline void xf(float x, float y, float *ox, float *oy) {
  x += X.tx;
  y += X.ty;
  if (!X_3d) { *ox = x; *oy = y; return; }
  float dx = x - X.axis_x;
  float wx = X.axis_x + dx * X_cos;
  float z = dx * X_sin + X.depth;
  float s = PERSP_D / (PERSP_D + z) * X.scale;
  *ox = SCREEN_W / 2.0f + (wx - SCREEN_W / 2.0f) * s;
  *oy = SCREEN_H / 2.0f + (y - SCREEN_H / 2.0f) * s;
}

static inline uint32_t xc(uint32_t c) { return X.alpha >= 0.999f ? c : col_alpha(c, X.alpha); }

/* ---------------------------------------------------------------- */
/* Vertex-colour geometry                                            */
/* ---------------------------------------------------------------- */

static vita2d_color_vertex *verts(int n) {
  return vita2d_pool_memalign(n * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
}

static inline void vset(vita2d_color_vertex *v, float x, float y, uint32_t c) {
  xf(x, y, &v->x, &v->y);
  v->z = 0.5f;
  v->color = xc(c);
}

static void quad4(float x, float y, float w, float h, uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11) {
  if (w <= 0 || h <= 0) return;
  vita2d_color_vertex *v = verts(4);
  if (!v) return;
  vset(&v[0], x, y, c00);
  vset(&v[1], x + w, y, c10);
  vset(&v[2], x, y + h, c01);
  vset(&v[3], x + w, y + h, c11);
  vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void ui_rect(float x, float y, float w, float h, uint32_t c) { quad4(x, y, w, h, c, c, c, c); }
void ui_vgrad(float x, float y, float w, float h, uint32_t t, uint32_t b) { quad4(x, y, w, h, t, t, b, b); }
void ui_hgrad(float x, float y, float w, float h, uint32_t l, uint32_t r) { quad4(x, y, w, h, l, r, l, r); }

#define CORNER_SEG 8

static int rrect_points(float x, float y, float w, float h, float r, vec2 *out) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r < 0) r = 0;
  const float cx[4] = { x + r, x + w - r, x + w - r, x + r };
  const float cy[4] = { y + r, y + r, y + h - r, y + h - r };
  int n = 0;
  for (int c = 0; c < 4; c++) {
    float a0 = M_PI + c * (M_PI / 2);
    for (int i = 0; i <= CORNER_SEG; i++) {
      float a = a0 + (M_PI / 2) * i / CORNER_SEG;
      out[n].x = cx[c] + cosf(a) * r;
      out[n].y = cy[c] + sinf(a) * r;
      n++;
    }
  }
  return n;
}

void ui_rrect_vgrad(float x, float y, float w, float h, float r, uint32_t top, uint32_t bot) {
  if (w <= 0 || h <= 0) return;
  if (r < 0.5f) { ui_vgrad(x, y, w, h, top, bot); return; }
  vec2 p[4 * (CORNER_SEG + 1)];
  int n = rrect_points(x, y, w, h, r, p);
  vita2d_color_vertex *v = verts(n + 2);
  if (!v) return;
  vset(&v[0], x + w / 2, y + h / 2, col_lerp(top, bot, 0.5f));
  for (int i = 0; i < n; i++) vset(&v[i + 1], p[i].x, p[i].y, col_lerp(top, bot, (p[i].y - y) / h));
  v[n + 1] = v[1];
  vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 2);
}

void ui_rrect(float x, float y, float w, float h, float r, uint32_t c) { ui_rrect_vgrad(x, y, w, h, r, c, c); }

static void band(const vec2 *outer, const vec2 *inner, int n, uint32_t co, uint32_t ci, int closed) {
  int m = closed ? n + 1 : n;
  vita2d_color_vertex *v = verts(m * 2);
  if (!v) return;
  for (int i = 0; i < m; i++) {
    int k = i % n;
    vset(&v[i * 2], outer[k].x, outer[k].y, co);
    vset(&v[i * 2 + 1], inner[k].x, inner[k].y, ci);
  }
  vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, m * 2);
}

void ui_rrect_outline(float x, float y, float w, float h, float r, float t, uint32_t c) {
  vec2 o[4 * (CORNER_SEG + 1)], in[4 * (CORNER_SEG + 1)];
  int n = rrect_points(x, y, w, h, r, o);
  rrect_points(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, in);
  band(o, in, n, c, c, 1);
}

void ui_glow(float x, float y, float w, float h, float r, float spread, uint32_t c) {
  vec2 o[4 * (CORNER_SEG + 1)], in[4 * (CORNER_SEG + 1)];
  int n = rrect_points(x - spread, y - spread, w + 2 * spread, h + 2 * spread, r + spread, o);
  rrect_points(x, y, w, h, r, in);
  band(o, in, n, col_alpha(c, 0), c, 1);
}

void ui_shadow(float x, float y, float w, float h, float r, float spread, float alpha) {
  ui_glow(x, y + spread * 0.35f, w, h, r, spread, C_RGBA(0, 0, 0, (int)(alpha * 255)));
}

static int circle_points(float cx, float cy, float r, float a0, float a1, int seg, vec2 *out) {
  for (int i = 0; i <= seg; i++) {
    float a = a0 + (a1 - a0) * i / seg;
    out[i].x = cx + cosf(a) * r;
    out[i].y = cy + sinf(a) * r;
  }
  return seg + 1;
}

static int seg_for(float r) {
  int s = (int)(r * 1.5f);
  if (s < 20) s = 20;
  if (s > 96) s = 96;
  return s;
}

void ui_circle_vgrad(float x, float y, float r, uint32_t top, uint32_t bot) {
  int seg = seg_for(r);
  vec2 p[100];
  int n = circle_points(x, y, r, 0, 2 * M_PI, seg, p);
  vita2d_color_vertex *v = verts(n + 1);
  if (!v) return;
  vset(&v[0], x, y, col_lerp(top, bot, 0.5f));
  for (int i = 0; i < n; i++) vset(&v[i + 1], p[i].x, p[i].y, col_lerp(top, bot, (p[i].y - (y - r)) / (2 * r)));
  vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 1);
}

void ui_circle(float x, float y, float r, uint32_t c) { ui_circle_vgrad(x, y, r, c, c); }

void ui_circle_radial(float x, float y, float r, uint32_t inner, uint32_t outer) {
  int seg = seg_for(r);
  vec2 p[100];
  int n = circle_points(x, y, r, 0, 2 * M_PI, seg, p);
  vita2d_color_vertex *v = verts(n + 1);
  if (!v) return;
  vset(&v[0], x, y, inner);
  for (int i = 0; i < n; i++) vset(&v[i + 1], p[i].x, p[i].y, outer);
  vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 1);
}

void ui_arc(float x, float y, float r, float t, float a0, float a1, uint32_t c) {
  int seg = (int)(seg_for(r) * fabsf(a1 - a0) / (2 * M_PI)) + 2;
  if (seg > 99) seg = 99;
  vec2 o[100], in[100];
  int n = circle_points(x, y, r, a0, a1, seg, o);
  circle_points(x, y, r - t, a0, a1, seg, in);
  band(o, in, n, c, c, 0);
}

void ui_ring(float x, float y, float r, float t, uint32_t c) { ui_arc(x, y, r, t, 0, 2 * M_PI, c); }

void ui_line(float x1, float y1, float x2, float y2, float w, uint32_t c) {
  float dx = x2 - x1, dy = y2 - y1, l = sqrtf(dx * dx + dy * dy);
  if (l < 0.001f) return;
  float nx = -dy / l * w / 2, ny = dx / l * w / 2;
  vita2d_color_vertex *v = verts(4);
  if (!v) return;
  vset(&v[0], x1 + nx, y1 + ny, c);
  vset(&v[1], x2 + nx, y2 + ny, c);
  vset(&v[2], x1 - nx, y1 - ny, c);
  vset(&v[3], x2 - nx, y2 - ny, c);
  vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void ui_tri(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c) {
  vita2d_color_vertex *v = verts(3);
  if (!v) return;
  vset(&v[0], x1, y1, c);
  vset(&v[1], x2, y2, c);
  vset(&v[2], x3, y3, c);
  vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
}

static int g_clip_on;
void ui_clip(int x, int y, int w, int h) {
  if (X_3d) return;  /* swung-back pages aren't clipped */
  int cx = (int)(x + X.tx), cy = (int)(y + X.ty);
  vita2d_enable_clipping();
  vita2d_set_clip_rectangle(cx < 0 ? 0 : cx, cy < 0 ? 0 : cy, cx + w, cy + h);
  g_clip_on = 1;
}

void ui_unclip(void) {
  if (g_clip_on) vita2d_disable_clipping();
  g_clip_on = 0;
}

/* ---------------------------------------------------------------- */
/* Textures                                                          */
/* ---------------------------------------------------------------- */

void ui_tex(vita2d_texture *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t tint) {
  vita2d_texture_vertex *v = vita2d_pool_memalign(4 * sizeof(vita2d_texture_vertex), sizeof(vita2d_texture_vertex));
  if (!v) return;
  float px[4] = { x, x + w, x, x + w }, py[4] = { y, y, y + h, y + h };
  float pu[4] = { u0, u1, u0, u1 }, pv[4] = { v0, v0, v1, v1 };
  for (int i = 0; i < 4; i++) {
    xf(px[i], py[i], &v[i].x, &v[i].y);
    v[i].z = 0.5f;
    v[i].u = pu[i];
    v[i].v = pv[i];
  }
  vita2d_draw_array_textured(t, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4, xc(tint));
}

void ui_tex_cover(vita2d_texture *t, float x, float y, float w, float h, float alpha) {
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  float s = fmaxf(w / tw, h / th);
  float cw = w / s, ch = h / s;
  float u0 = (tw - cw) / 2 / tw, v0 = (th - ch) / 2 / th;
  ui_tex(t, x, y, w, h, u0, v0, u0 + cw / tw, v0 + ch / th, C_RGBA(255, 255, 255, (int)(alpha * 255)));
}

void ui_tex_fit(vita2d_texture *t, float x, float y, float w, float h, float alpha) {
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  float s = fminf(w / tw, h / th);
  ui_tex(t, x + (w - tw * s) / 2, y + (h - th * s) / 2, tw * s, th * s, 0, 0, 1, 1, C_RGBA(255, 255, 255, (int)(alpha * 255)));
}

/* ---------------------------------------------------------------- */
/* Text                                                              */
/* ---------------------------------------------------------------- */

typedef struct { uint32_t c; } text_ctx;

static void glyph_cb(void *ctx, float x, float y, float w, float h, float u0, float v0, float u1, float v1) {
  ui_tex(font_atlas(), x, y, w, h, u0, v0, u1, v1, ((text_ctx *)ctx)->c);
}

static void draw_baseline(float x, float baseline, int px, uint32_t c, const char *s) {
  text_ctx t = { c };
  /* On a flat page snap to whole pixels so glyphs map 1:1 onto the atlas. */
  if (X_flat_int) { x = roundf(x); baseline = roundf(baseline); }
  if (!font_native()) {
    /* vita2d draws this itself, so only translation/alpha of the transform apply */
    font_fallback_draw(x + X.tx, baseline + X.ty, px, xc(c), s);
    return;
  }
  font_layout(px, x, baseline, s, glyph_cb, &t);
}

void ui_text(float x, float y, int px, uint32_t c, const char *s) {
  float cap = font_ascent(px);
  draw_baseline(x, y + (px + cap) / 2.0f, px, c, s);
}

void ui_text_vc(float x, float cy, int px, uint32_t c, const char *s) {
  draw_baseline(x, cy + font_ascent(px) / 2.0f, px, c, s);
}

void ui_text_shadow_vc(float x, float cy, int px, uint32_t c, const char *s) {
  uint32_t sh = C_RGBA(0, 0, 0, (int)(((c >> 24) & 0xFF) * 0.35f));
  ui_text_vc(x + 1, cy + 2, px, sh, s);
  ui_text_vc(x, cy, px, c, s);
}

int ui_text_w(int px, const char *s) { return (int)ceilf(font_width(px, s)); }

static int utf8_prev(const char *s, int i) {
  do { i--; } while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80);
  return i;
}

static const char *clip_str(int px, const char *s, float max_w, char *buf, int cap) {
  if (ui_text_w(px, s) <= max_w) return s;
  snprintf(buf, cap - 4, "%s", s);
  int n = strlen(buf);
  while (n > 0) {
    n = utf8_prev(buf, n);
    while (n > 0 && buf[n - 1] == ' ') n--;
    strcpy(buf + n, "…");
    if (ui_text_w(px, buf) <= max_w) break;
  }
  return buf;
}

void ui_text_clip(float x, float y, int px, uint32_t c, const char *s, float max_w) {
  char buf[512];
  ui_text(x, y, px, c, clip_str(px, s, max_w, buf, sizeof(buf)));
}

void ui_text_clip_vc(float x, float cy, int px, uint32_t c, const char *s, float max_w) {
  char buf[512];
  ui_text_vc(x, cy, px, c, clip_str(px, s, max_w, buf, sizeof(buf)));
}

void ui_text_center(float cx, float cy, int px, uint32_t c, const char *s) {
  ui_text_vc(cx - ui_text_w(px, s) / 2.0f, cy, px, c, s);
}

void ui_text_right(float rx, float cy, int px, uint32_t c, const char *s) {
  ui_text_vc(rx - ui_text_w(px, s), cy, px, c, s);
}

int ui_text_wrap(float x, float y, int px, uint32_t c, const char *s, float max_w, float line_h, int max_lines, int draw) {
  char line[1024];
  int lines = 0;
  const char *p = s;
  while (*p && lines < max_lines) {
    if (*p == '\n') { p++; continue; }
    int len = 0;
    const char *q = p;
    /* grow word by word */
    while (*q && *q != '\n') {
      int wl = 0;
      while (q[wl] && q[wl] != ' ' && q[wl] != '\n') wl++;
      if (len + wl + 2 >= (int)sizeof(line)) break;
      memcpy(line + len, q, wl);
      line[len + wl] = 0;
      if (len > 0 && ui_text_w(px, line) > max_w) { line[len] = 0; break; }
      len += wl;
      q += wl;
      if (*q == ' ') { line[len++] = ' '; line[len] = 0; q++; }
    }
    while (len > 0 && line[len - 1] == ' ') len--;
    line[len] = 0;
    if (len == 0) {
      if (draw) ui_text_clip(x, y + lines * line_h, px, c, p, max_w);
      while (*p && *p != ' ' && *p != '\n') p++;
      lines++;
      continue;
    }
    const char *next = q;
    while (*next == ' ') next++;
    if (draw) {
      if (lines == max_lines - 1 && *next) {
        char buf[1100], tmp[1100];
        snprintf(buf, sizeof(buf), "%s %s", line, next);
        ui_text(x, y + lines * line_h, px, c, clip_str(px, buf, max_w, tmp, sizeof(tmp)));
      } else {
        ui_text(x, y + lines * line_h, px, c, line);
      }
    }
    lines++;
    p = next;
  }
  return lines;
}

/* ---------------------------------------------------------------- */
/* Icons                                                             */
/* ---------------------------------------------------------------- */

void ui_icon(icon_id id, float cx, float cy, float s, uint32_t c) {
  float h = s / 2, t = fmaxf(2, s / 11);
  switch (id) {
    case ICON_FOLDER:
      ui_rrect(cx - h, cy - h * 0.62f, h * 0.9f, h * 0.4f, t, c);
      ui_rrect(cx - h, cy - h * 0.42f, s, h * 1.25f, t, c);
      break;
    case ICON_COLLECTION:
      ui_rrect(cx - h * 0.7f, cy - h * 0.9f, h * 1.4f, h * 0.2f, 1, col_alpha(c, 0.6f));
      ui_rrect(cx - h * 0.85f, cy - h * 0.6f, h * 1.7f, h * 0.2f, 1, col_alpha(c, 0.8f));
      ui_rrect(cx - h, cy - h * 0.3f, s, h * 1.2f, t, c);
      break;
    case ICON_FILM: {
      ui_rrect_outline(cx - h, cy - h * 0.75f, s, h * 1.5f, t * 0.8f, t, c);
      float hw = s * 0.08f;
      for (int i = 0; i < 4; i++) {
        float hx = cx - h + s * 0.14f + i * s * 0.24f;
        ui_rect(hx, cy - h * 0.62f, hw, hw, c);
        ui_rect(hx, cy + h * 0.62f - hw, hw, hw, c);
      }
      ui_tri(cx - h * 0.25f, cy - h * 0.3f, cx + h * 0.35f, cy, cx - h * 0.25f, cy + h * 0.3f, c);
      break;
    }
    case ICON_TV:
      ui_rrect_outline(cx - h, cy - h * 0.72f, s, h * 1.3f, t * 1.2f, t, c);
      ui_line(cx - h * 0.4f, cy + h * 0.85f, cx + h * 0.4f, cy + h * 0.85f, t, c);
      break;
    case ICON_MUSIC:
      ui_circle(cx - h * 0.35f, cy + h * 0.55f, h * 0.32f, c);
      ui_line(cx - h * 0.06f, cy + h * 0.55f, cx - h * 0.06f, cy - h * 0.85f, t, c);
      ui_line(cx - h * 0.06f, cy - h * 0.8f, cx + h * 0.6f, cy - h * 0.45f, t * 1.4f, c);
      break;
    case ICON_VIDEO:
      ui_rrect(cx - h, cy - h * 0.6f, s * 0.7f, h * 1.2f, t, c);
      ui_tri(cx + h * 0.45f, cy - h * 0.1f, cx + h, cy - h * 0.55f, cx + h, cy + h * 0.55f, c);
      ui_tri(cx + h * 0.45f, cy + h * 0.1f, cx + h * 0.45f, cy - h * 0.1f, cx + h, cy + h * 0.55f, c);
      break;
    case ICON_PLAY:
      ui_tri(cx - h * 0.55f, cy - h * 0.75f, cx + h * 0.8f, cy, cx - h * 0.55f, cy + h * 0.75f, c);
      break;
    case ICON_PAUSE:
      ui_rrect(cx - h * 0.6f, cy - h * 0.7f, h * 0.42f, h * 1.4f, 1.5f, c);
      ui_rrect(cx + h * 0.18f, cy - h * 0.7f, h * 0.42f, h * 1.4f, 1.5f, c);
      break;
    case ICON_RESUME:
      ui_arc(cx, cy, h, t, -M_PI * 0.5f, M_PI * 1.2f, c);
      ui_tri(cx - h * 0.3f, cy - h * 0.45f, cx + h * 0.5f, cy, cx - h * 0.3f, cy + h * 0.45f, c);
      break;
    case ICON_NEXT:
      ui_tri(cx - h * 0.8f, cy - h * 0.7f, cx + h * 0.3f, cy, cx - h * 0.8f, cy + h * 0.7f, c);
      ui_rect(cx + h * 0.4f, cy - h * 0.7f, h * 0.3f, h * 1.4f, c);
      break;
    case ICON_SERVER:
      for (int i = 0; i < 3; i++) {
        float y0 = cy - h * 0.85f + i * h * 0.6f;
        ui_rrect_outline(cx - h * 0.85f, y0, s * 0.85f, h * 0.48f, t * 0.8f, t * 0.8f, c);
        ui_circle(cx + h * 0.5f, y0 + h * 0.24f, t * 0.7f, c);
      }
      break;
    case ICON_USER:
      ui_circle(cx, cy - h * 0.35f, h * 0.38f, c);
      ui_arc(cx, cy + h * 0.9f, h * 0.8f, h * 0.8f, M_PI, 2 * M_PI, c);
      break;
    case ICON_KEY:
      ui_ring(cx - h * 0.45f, cy, h * 0.42f, t * 1.2f, c);
      ui_line(cx - h * 0.05f, cy, cx + h * 0.95f, cy, t * 1.2f, c);
      ui_line(cx + h * 0.55f, cy, cx + h * 0.55f, cy + h * 0.35f, t * 1.2f, c);
      ui_line(cx + h * 0.85f, cy, cx + h * 0.85f, cy + h * 0.3f, t * 1.2f, c);
      break;
    case ICON_SIGNOUT:
      ui_arc(cx, cy, h * 0.8f, t, -M_PI * 0.32f, M_PI * 1.32f, c);
      ui_line(cx, cy - h, cx, cy - h * 0.1f, t, c);
      break;
    case ICON_CHECK:
      ui_line(cx - h * 0.6f, cy, cx - h * 0.15f, cy + h * 0.45f, t * 1.3f, c);
      ui_line(cx - h * 0.2f, cy + h * 0.45f, cx + h * 0.65f, cy - h * 0.5f, t * 1.3f, c);
      break;
    case ICON_BACK: {
      /* The Vita return arrow: thick U-turn opening left with a bold arrowhead. */
      float tk = s * 0.16f, r = h * 0.42f;
      float acx = cx + h * 0.12f, acy = cy + h * 0.16f;
      ui_arc(acx, acy, r + tk / 2, tk, -M_PI / 2, M_PI / 2, c);
      ui_line(cx - h * 0.18f, acy - r, acx + 0.5f, acy - r, tk, c);
      ui_line(cx - h * 0.05f, acy + r, acx + 0.5f, acy + r, tk, c);
      ui_circle(cx - h * 0.05f, acy + r, tk / 2, c);
      float ax = cx - h * 0.62f, ay = acy - r;
      ui_tri(ax, ay, ax + h * 0.5f, ay - h * 0.42f, ax + h * 0.5f, ay + h * 0.42f, c);
      break;
    }
    case ICON_DOTS:
      for (int i = -1; i <= 1; i++) ui_circle(cx + i * h * 0.55f, cy, s * 0.085f, c);
      break;
    case ICON_SETTINGS:
      ui_ring(cx, cy, h * 0.62f, h * 0.3f, c);
      for (int i = 0; i < 8; i++) {
        float a = i * M_PI / 4;
        ui_line(cx + cosf(a) * h * 0.5f, cy + sinf(a) * h * 0.5f, cx + cosf(a) * h * 0.92f, cy + sinf(a) * h * 0.92f, h * 0.28f, c);
      }
      break;
    case ICON_HOME:
      ui_tri(cx - h, cy - h * 0.05f, cx, cy - h * 0.9f, cx + h, cy - h * 0.05f, c);
      ui_rect(cx - h * 0.68f, cy - h * 0.1f, h * 1.36f, h * 0.95f, c);
      break;
    case ICON_CHEVRON:
      ui_tri(cx - h * 0.22f, cy - h * 0.42f, cx + h * 0.34f, cy, cx - h * 0.22f, cy + h * 0.42f, c);
      break;
    default:
      break;
  }
}

void ui_star(float cx, float cy, float r, uint32_t c) {
  vec2 p[10];
  for (int i = 0; i < 10; i++) {
    float a = -M_PI / 2 + i * M_PI / 5, rr = i % 2 ? r * 0.45f : r;
    p[i].x = cx + cosf(a) * rr;
    p[i].y = cy + sinf(a) * rr;
  }
  for (int i = 0; i < 10; i++) ui_tri(cx, cy, p[i].x, p[i].y, p[(i + 1) % 10].x, p[(i + 1) % 10].y, c);
}

icon_id ui_icon_for_item(const char *type, const char *ct) {
  if (ct && *ct) {
    if (!strcmp(ct, "movies")) return ICON_FILM;
    if (!strcmp(ct, "tvshows")) return ICON_TV;
    if (!strcmp(ct, "music")) return ICON_MUSIC;
    if (!strcmp(ct, "homevideos") || !strcmp(ct, "musicvideos")) return ICON_VIDEO;
    if (!strcmp(ct, "boxsets") || !strcmp(ct, "playlists")) return ICON_COLLECTION;
  }
  if (!strcmp(type, "Movie")) return ICON_FILM;
  if (!strcmp(type, "Series") || !strcmp(type, "Season") || !strcmp(type, "Episode")) return ICON_TV;
  if (!strcmp(type, "Audio") || !strcmp(type, "MusicAlbum") || !strcmp(type, "MusicArtist")) return ICON_MUSIC;
  if (!strcmp(type, "Video") || !strcmp(type, "MusicVideo")) return ICON_VIDEO;
  if (!strcmp(type, "BoxSet")) return ICON_COLLECTION;
  return ICON_FOLDER;
}

/* ---------------------------------------------------------------- */
/* Chrome                                                            */
/* ---------------------------------------------------------------- */

void ui_background(void) {
  /* Settings: dark at the top, deepest below the middle, lime glow at the bottom,
   * and soft light falling on the centre. */
  ui_vgrad(0, 0, SCREEN_W, 300, T->bg_top, T->bg_mid);
  ui_vgrad(0, 300, SCREEN_W, 130, T->bg_mid, T->bg_bot);
  ui_vgrad(0, 430, SCREEN_W, SCREEN_H - 430, T->bg_bot, T->glow);
  uint32_t lit = C_RGBA(255, 255, 255, 22), none = C_RGBA(255, 255, 255, 0);
  ui_hgrad(0, 0, SCREEN_W / 2, SCREEN_H, none, lit);
  ui_hgrad(SCREEN_W / 2, 0, SCREEN_W / 2, SCREEN_H, lit, none);
  ui_tri(-40, 544, 380, 120, 520, 120, C_RGBA(255, 255, 255, 7));
  ui_tri(-40, 544, 520, 120, 140, 544, C_RGBA(255, 255, 255, 7));
  ui_tri(560, 544, 980, 260, 980, 360, C_RGBA(255, 255, 255, 6));
}

void ui_watermark(icon_id icon) {
  ui_icon(icon, 770, 340, 300, C_RGBA(255, 255, 255, 11));
}

static void draw_battery(float x, float cy, int pct, int charging) {
  float w = 30, h = 15, y = cy - h / 2;
  ui_rrect_outline(x, y, w, h, 2.5f, 1.5f, C_RGBA(0xE8, 0xE8, 0xE8, 255));
  ui_rect(x + w, cy - 3.5f, 2.5f, 7, C_RGBA(0xE8, 0xE8, 0xE8, 255));
  pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  float fw = (w - 5) * pct / 100.0f;
  uint32_t top = pct <= 15 && !charging ? C_RGBA(0xFF, 0x6A, 0x50, 255) : C_RGBA(0xB4, 0xF5, 0x6A, 255);
  uint32_t bot = pct <= 15 && !charging ? C_RGBA(0xD0, 0x20, 0x10, 255) : C_RGBA(0x3C, 0xB4, 0x1C, 255);
  ui_vgrad(x + 2.5f, y + 2.5f, fw, h - 5, top, bot);
  if (charging) {
    float lx = x + w / 2;
    ui_tri(lx + 2, y + 1, lx - 5, cy + 1, lx, cy + 1, COL_TEXT);
    ui_tri(lx, cy - 1, lx + 5, cy - 1, lx - 2, y + h - 1, COL_TEXT);
  }
}

static void draw_wifi(float cx, float by, int bars, int on) {
  for (int i = 0; i < 3; i++) {
    float r = 5 + i * 4.5f;
    ui_arc(cx, by, r, 2.2f, -M_PI * 0.75f, -M_PI * 0.25f, on && i < bars ? COL_TEXT : C_RGBA(255, 255, 255, 60));
  }
  ui_circle(cx, by - 1, 2, on ? COL_TEXT : C_RGBA(255, 255, 255, 60));
}

void ui_status_bar(void) {
  static int pct = 100, chg = 0, inet = 0, rssi = 0;
  if (g_frames % 120 == 1) {
    pct = scePowerGetBatteryLifePercent();
    chg = scePowerIsBatteryCharging();
    int st = 0;
    sceNetCtlInetGetState(&st);
    inet = st == SCE_NETCTL_STATE_CONNECTED;
    SceNetCtlInfo info;
    if (inet && sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info) >= 0) rssi = info.rssi_percentage;
  }
  ui_vgrad(0, 0, SCREEN_W, STATUS_H, C_RGBA(0, 0, 0, 255), C_RGBA(0x1e, 0x1e, 0x1e, 255));
  ui_rect(0, STATUS_H - 1, SCREEN_W, 1, C_RGBA(0x2d, 0x2d, 0x2d, 255));

  SceDateTime t;
  sceRtcGetCurrentClockLocalTime(&t);
  char hm[12];
  int hr = t.hour % 12;
  snprintf(hm, sizeof(hm), "%d:%02d", hr ? hr : 12, t.minute);
  const char *ap = t.hour < 12 ? "AM" : "PM";
  float cy = STATUS_H / 2.0f;
  float x = SCREEN_W - 12 - 32 - 10;
  draw_battery(x, cy, pct, chg);
  float apw = ui_text_w(15, ap);
  ui_text_vc(x - 8 - apw, cy + 1, 15, COL_TEXT, ap);
  float hmw = ui_text_w(23, hm);
  ui_text_vc(x - 8 - apw - 5 - hmw, cy, 23, COL_TEXT, hm);
  int bars = rssi > 66 ? 3 : rssi > 33 ? 2 : 1;
  draw_wifi(x - 8 - apw - 5 - hmw - 26, cy + 8, bars, inet);
}

void ui_page_title(const char *title) {
  char buf[512];
  const char *s = clip_str(32, title, 760, buf, sizeof(buf));
  float w = ui_text_w(32, s);
  ui_text_shadow_vc((SCREEN_W - w) / 2, STATUS_H + 35, 32, COL_TEXT, s);
}

/* The big round corner buttons: partly off-screen, translucent, glossy. */
static float g_corner_press[2];

/* A widget claims a touch when the finger lands on it, and gets the tap on release
 * only if it still holds the claim (sliding off cancels, like the system UI). */
static int touch_widget(unsigned id, float x, float y, float w, float h, int *down) {
  if (g_in.touch_down && !g_in.consumed && point_in(g_in.sx, g_in.sy, x, y, w, h)) {
    g_in.consumed = 1;
    g_in.claim = id;
  }
  int mine = g_in.claim == id;
  int inside = point_in(g_in.tx, g_in.ty, x - 24, y - 24, w + 48, h + 48);
  if (down) *down = mine && g_in.touching && inside;
  return mine && g_in.tap && inside;
}

static unsigned rect_id(float x, float y, float w, float h) {
  unsigned a = (unsigned)(int)x * 73856093u ^ (unsigned)(int)y * 19349663u ^ (unsigned)(int)w * 83492791u ^ (unsigned)(int)h;
  return a | 0x100u;  /* never 0 or a corner id */
}

static int corner_button(int right, int visible, icon_id icon) {
  float *press = &g_corner_press[right];
  if (!visible) { *press = 0; return 0; }
  const float r = 50;
  float cx = right ? SCREEN_W - 24 : 24, cy = SCREEN_H - 26;
  float hx = right ? SCREEN_W - 110 : 0, hy = SCREEN_H - 100;
  int down = 0, tapped = 0;
  if (g_ui_input) tapped = touch_widget(right ? 2 : 1, hx, hy, 110, 100, &down);
  else down = g_in.touching && point_in(g_in.sx, g_in.sy, hx, hy, 110, 100);
  *press = approach(*press, down ? 1.0f : 0.0f, 22);

  float k = 1 - 0.06f * *press;
  float rr = r * k;
  ui_circle_radial(cx, cy, rr + 8, C_RGBA(0, 0, 0, 40), C_RGBA(0, 0, 0, 0));
  uint32_t top = col_lerp(C_RGBA(255, 255, 255, 80), C_RGBA(255, 255, 255, 140), *press);
  uint32_t bot = col_lerp(C_RGBA(255, 255, 255, 34), C_RGBA(255, 255, 255, 80), *press);
  ui_circle_vgrad(cx, cy, rr, top, bot);
  ui_arc(cx, cy, rr - 1, rr * 0.45f, M_PI * 1.08f, M_PI * 1.92f, C_RGBA(255, 255, 255, 26));
  ui_ring(cx, cy, rr, 1.5f, C_RGBA(255, 255, 255, 90));
  float ix = right ? cx - 14 : cx + 14, iy = cy - 10;
  uint32_t ic = col_lerp(C_RGBA(0xE6, 0xFF, 0xE6, 255), C_RGBA(255, 255, 255, 255), *press);
  ui_icon(icon, ix + 1, iy + 2, 40 * k, C_RGBA(0, 0, 0, 50));
  ui_icon(icon, ix, iy, 40 * k, ic);
  return tapped;
}

int ui_back_button(int visible) { return corner_button(0, visible, ICON_BACK); }
int ui_options_button(int visible) { return corner_button(1, visible, ICON_DOTS); }

void ui_spinner(float cx, float cy, float r, float alpha) {
  float head = fmodf(ui_time() * 12, 12);
  for (int i = 0; i < 12; i++) {
    float age = fmodf(head - i + 12, 12);
    float a = i * (M_PI / 6) - M_PI / 2;
    float k = 1.0f - age / 12.0f;
    uint32_t c = C_RGBA(255, 255, 255, (int)((50 + 205 * k * k) * alpha));
    ui_line(cx + cosf(a) * r * 0.5f, cy + sinf(a) * r * 0.5f, cx + cosf(a) * r, cy + sinf(a) * r, r * 0.2f, c);
  }
}

void ui_row_separator(float x, float y, float w) {
  /* etched: a dark line with a light line beneath, faded at the ends */
  float f = 40;
  uint32_t d = C_RGBA(0, 0, 0, 64), l = C_RGBA(255, 255, 255, 34), z = C_RGBA(0, 0, 0, 0), zl = C_RGBA(255, 255, 255, 0);
  ui_hgrad(x, y, f, 1, z, d);
  ui_rect(x + f, y, w - 2 * f, 1, d);
  ui_hgrad(x + w - f, y, f, 1, d, z);
  ui_hgrad(x, y + 1, f, 1, zl, l);
  ui_rect(x + f, y + 1, w - 2 * f, 1, l);
  ui_hgrad(x + w - f, y + 1, f, 1, l, zl);
}

void ui_row_highlight(float x, float y, float w, float h, float a) {
  if (a <= 0.01f) return;
  /* glossy white bar fading out at both ends, as the system list highlight */
  uint32_t mid = C_RGBA(255, 255, 255, (int)(95 * a)), lo = C_RGBA(255, 255, 255, (int)(40 * a)), z = C_RGBA(255, 255, 255, 0);
  float e = fminf(140, w * 0.25f);
  quad4(x, y, e, h, z, lo, z, lo);
  quad4(x + e, y, w - 2 * e, h, lo, lo, lo, lo);
  quad4(x + w - e, y, e, h, lo, z, lo, z);
  quad4(x + e, y, w - 2 * e, h * 0.5f, mid, mid, lo, lo);
  quad4(x, y, e, h * 0.5f, z, mid, z, lo);
  quad4(x + w - e, y, e, h * 0.5f, mid, z, lo, z);
  uint32_t line = C_RGBA(255, 255, 255, (int)(150 * a)), lz = C_RGBA(255, 255, 255, 0);
  ui_hgrad(x, y, w / 2, 1, lz, line);
  ui_hgrad(x + w / 2, y, w / 2, 1, line, lz);
}

void ui_focus_bar(float x, float y, float w, float h) {
  float pulse = 0.75f + 0.25f * sinf(ui_time() * 4.2f);
  ui_row_highlight(x, y, w, h, 0.85f * pulse);
  uint32_t g = C_RGBA(255, 255, 255, (int)(110 * pulse)), z = C_RGBA(255, 255, 255, 0);
  ui_hgrad(x, y + h - 1, w / 2, 1, z, g);
  ui_hgrad(x + w / 2, y + h - 1, w / 2, 1, g, z);
}

void ui_focus_ring(float x, float y, float w, float h, float r) {
  float pulse = 0.65f + 0.35f * sinf(ui_time() * 4.2f);
  ui_glow(x, y, w, h, r, 9, C_RGBA(255, 255, 255, (int)(110 * pulse)));
  ui_rrect_outline(x, y, w, h, r, 2.5f, C_RGBA(255, 255, 255, (int)(200 + 55 * pulse)));
}

int ui_button(const char *label, icon_id icon, float x, float y, float w, float h, int focused) {
  int down = 0, tapped = 0;
  if (g_ui_input) tapped = touch_widget(rect_id(x, y, w, h), x, y, w, h, &down);
  if (!g_ui_input) focused = 0;
  float r = 8;
  ui_shadow(x, y, w, h, r, 6, 0.35f);
  if (down) ui_rrect_vgrad(x, y, w, h, r, C_RGBA(0xC8, 0xC8, 0xCC, 255), C_RGBA(0xE8, 0xE8, 0xEC, 255));
  else ui_rrect_vgrad(x, y, w, h, r, C_RGBA(0xFF, 0xFF, 0xFF, 255), C_RGBA(0xCF, 0xD0, 0xD4, 255));
  ui_rrect_vgrad(x + 2, y + 1, w - 4, h * 0.46f, r - 1, C_RGBA(255, 255, 255, down ? 20 : 120), C_RGBA(255, 255, 255, 0));
  ui_rrect_outline(x, y, w, h, r, 1, C_RGBA(0, 0, 0, 90));
  float tw = ui_text_w(22, label), iw = icon != ICON_NONE ? 32 : 0;
  float sx = x + (w - tw - iw) / 2;
  if (iw) ui_icon(icon, sx + 11, y + h / 2, 20, COL_DARK);
  ui_text_vc(sx + iw, y + h / 2, 22, COL_DARK, label);
  if (focused && g_focus_visible) ui_focus_ring(x - 3, y - 3, w + 6, h + 6, r + 3);
  if (tapped) sfx_play(SFX_SELECT);
  return tapped;
}

void ui_progress_bar(float x, float y, float w, float h, float frac) {
  frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
  ui_rrect(x, y, w, h, h / 2, C_RGBA(0, 0, 0, 140));
  if (frac > 0) ui_rrect_vgrad(x, y, fmaxf(h, w * frac), h, h / 2, C_RGBA(255, 255, 255, 255), T->accent);
}

void ui_badge_check(float cx, float cy) {
  ui_circle(cx, cy + 1, 13, C_RGBA(0, 0, 0, 90));
  ui_circle_vgrad(cx, cy, 12, C_RGBA(255, 255, 255, 255), C_RGBA(0xD8, 0xD8, 0xDC, 255));
  ui_icon(ICON_CHECK, cx, cy + 1, 16, col_lerp(T->bg_top, C_RGBA(0, 0, 0, 255), 0.2f));
}

/* ---------------------------------------------------------------- */
/* Scroller                                                          */
/* ---------------------------------------------------------------- */

void scroller_init(scroller *s, int cols, float x, float y, float w, float h, float cell_w, float cell_h, float gx, float gy) {
  memset(s, 0, sizeof(*s));
  s->cols = cols < 1 ? 1 : cols;
  s->x = x; s->y = y; s->w = w; s->h = h;
  s->cell_w = cell_w; s->cell_h = cell_h; s->gap_x = gx; s->gap_y = gy;
  s->pad_top = 8; s->pad_bottom = 110;
  s->pressed = -1;
  s->hl_idx = -1;
}

void scroller_restart_appear(scroller *s) { s->appear = 0; }

void scroller_set_count(scroller *s, int count) {
  if (s->last_count == 0 && count > 0) s->appear = 0;
  s->last_count = count;
  s->count = count;
  if (s->sel >= count) s->sel = count ? count - 1 : 0;
}

float scroller_content_h(scroller *s) {
  int rows = (s->count + s->cols - 1) / s->cols;
  return s->pad_top + rows * s->cell_h + (rows > 0 ? (rows - 1) * s->gap_y : 0) + s->pad_bottom;
}

static float max_scroll(scroller *s) {
  float m = scroller_content_h(s) - s->h;
  return m > 0 ? m : 0;
}

void scroller_cell_rect(scroller *s, int idx, float *x, float *y) {
  int row = idx / s->cols, col = idx % s->cols;
  float total_w = s->cols * s->cell_w + (s->cols - 1) * s->gap_x;
  *x = s->x + (s->w - total_w) / 2 + col * (s->cell_w + s->gap_x);
  *y = s->y + s->pad_top + row * (s->cell_h + s->gap_y) - s->scroll;
}

static void ensure_visible(scroller *s) {
  int row = s->sel / s->cols;
  float top = s->pad_top + row * (s->cell_h + s->gap_y);
  float margin = s->cell_h * 0.4f;
  if (top - margin < s->target) s->target = top - margin;
  if (top + s->cell_h + margin + 70 > s->target + s->h) s->target = top + s->cell_h + margin + 70 - s->h;
  if (s->target < 0) s->target = 0;
  if (s->target > max_scroll(s)) s->target = max_scroll(s);
}

static int hit_index(scroller *s, int px, int py) {
  if (!point_in(px, py, s->x, s->y, s->w, s->h)) return -1;
  for (int i = 0; i < s->count; i++) {
    float cx, cy;
    scroller_cell_rect(s, i, &cx, &cy);
    if (cy > s->y + s->h) break;
    if (point_in(px, py, cx, cy, s->cell_w, s->cell_h)) return i;
  }
  return -1;
}

int scroller_update(scroller *s, int input) {
  int activated = -1;
  float maxs = max_scroll(s);
  s->appear += ui_dt();

  if (input && s->count) {
    int old = s->sel;
    if (BTN_REPEAT(SCE_CTRL_DOWN)) {
      if (s->sel + s->cols < s->count) s->sel += s->cols;
      else if (s->sel / s->cols < (s->count - 1) / s->cols) s->sel = s->count - 1;
    }
    if (BTN_REPEAT(SCE_CTRL_UP) && s->sel - s->cols >= 0) s->sel -= s->cols;
    if (s->cols > 1) {
      if (BTN_REPEAT(SCE_CTRL_RIGHT) && s->sel + 1 < s->count && (s->sel + 1) % s->cols) s->sel++;
      if (BTN_REPEAT(SCE_CTRL_LEFT) && s->sel % s->cols) s->sel--;
    }
    int page = s->cols * (int)(s->h / (s->cell_h + s->gap_y));
    if (BTN_REPEAT(SCE_CTRL_RTRIGGER)) { s->sel += page; if (s->sel >= s->count) s->sel = s->count - 1; }
    if (BTN_REPEAT(SCE_CTRL_LTRIGGER)) { s->sel -= page; if (s->sel < 0) s->sel = 0; }
    if (old != s->sel) { g_focus_visible = 1; ensure_visible(s); sfx_play(SFX_MOVE); }
    if (BTN_PRESSED(g_in.enter)) {
      if (g_focus_visible) { activated = s->sel; s->hl_idx = s->sel; s->hl_a = 1; sfx_play(SFX_SELECT); }
      else { g_focus_visible = 1; ensure_visible(s); }
    }

    int in_corner = g_in.sy > SCREEN_H - 100 && (g_in.sx < 110 || g_in.sx > SCREEN_W - 110);
    if (g_in.touch_down && !g_in.consumed && !in_corner && point_in(g_in.sx, g_in.sy, s->x, s->y, s->w, s->h)) {
      s->dragging = 1;
      s->vel = 0;
      s->target = s->scroll;
      s->press_frames = 0;
    }
  }

  if (s->dragging) {
    if (g_in.touching) {
      g_in.consumed = 1;
      s->press_frames++;
      if (g_in.dragging) {
        float d = -g_in.dy;
        if (s->scroll < 0 || s->scroll > maxs) d *= 0.4f;
        s->scroll += d;
        s->target = s->scroll;
        s->vel = s->vel * 0.4f + d * 0.6f;
        s->pressed = -1;
      } else {
        /* highlight only after a short hold, so flicks don't flash rows */
        s->pressed = s->press_frames > 4 ? hit_index(s, g_in.tx, g_in.ty) : -1;
      }
    } else {
      s->dragging = 0;
      if (g_in.tap) {
        int i = hit_index(s, g_in.tx, g_in.ty);
        if (i >= 0) { s->sel = i; activated = i; s->hl_idx = i; s->hl_a = 1; sfx_play(SFX_SELECT); }
      }
      s->pressed = -1;
    }
  } else {
    s->pressed = -1;
  }

  if (s->pressed >= 0) { s->hl_idx = s->pressed; s->hl_a = approach(s->hl_a, 1, 30); }
  else s->hl_a = approach(s->hl_a, 0, 9);

  if (!s->dragging) {
    if (fabsf(s->vel) > 0.3f) {
      s->target += s->vel;
      s->vel *= powf(0.95f, ui_dt() * 60);
      if (s->target < 0 || s->target > maxs) s->vel *= 0.55f;
    } else {
      s->vel = 0;
    }
    if (s->target < 0) s->target = approach(s->target, 0, 14);
    if (s->target > maxs) s->target = approach(s->target, maxs, 14);
    s->scroll = approach(s->scroll, s->target, 18);
    if (fabsf(s->target - s->scroll) < 0.25f) s->scroll = s->target;
  }
  return activated;
}

void scroller_draw(scroller *s, cell_draw_fn fn, void *ctx) {
  ui_clip(s->x, s->y, s->w, s->h);
  int first = (int)((s->scroll - s->pad_top) / (s->cell_h + s->gap_y)) - 1;
  if (first < 0) first = 0;

  int vis = 0;
  for (int i = first * s->cols; i < s->count; i++) {
    float cx, cy;
    scroller_cell_rect(s, i, &cx, &cy);
    if (cy > s->y + s->h) break;
    if (cy + s->cell_h < s->y - 20) continue;
    /* entry cascade: rows drift up and fade in one after another */
    float t = ease_out((s->appear - vis * 0.035f) / 0.32f);
    vis++;
    if (t <= 0.001f) continue;
    ui_xform xf = ui_xform_identity();
    xf.ty = (1 - t) * 26;
    xf.alpha = t;
    ui_push_xform(xf);
    int focused = i == s->sel && g_focus_visible;
    float pa = i == s->hl_idx ? s->hl_a : 0;
    fn(ctx, i, cx, cy, s->cell_w, s->cell_h, focused, pa);
    ui_pop_xform();
  }
  ui_unclip();

  float maxs = max_scroll(s);
  if (maxs > 0 && (s->dragging || fabsf(s->vel) > 0.3f || fabsf(s->target - s->scroll) > 1)) {
    float total = scroller_content_h(s);
    float bh = fmaxf(30, s->h * s->h / total);
    float by = s->y + (s->h - bh) * fminf(1, fmaxf(0, s->scroll / maxs));
    ui_rrect(SCREEN_W - 10, by, 4, bh, 2, C_RGBA(255, 255, 255, 120));
  }
}

/* ---------------------------------------------------------------- */
/* Options menu — dark translucent panel rising from the "…" button  */
/* ---------------------------------------------------------------- */

#define OPT_W 360
#define OPT_ROW 60

void options_clear(options_menu *m) { m->count = 0; }

void options_add(options_menu *m, const char *label, int checked) {
  if (m->count >= OPT_MAX) return;
  m->labels[m->count] = label;
  m->checked[m->count] = checked;
  m->count++;
}

void options_open(options_menu *m) {
  m->open = 1;
  m->first = 0;
  m->just_opened = 1;
  sfx_play(SFX_MENU);
  m->sel = 0;
  m->pressed = -1;
}

static int opt_vis(options_menu *m) { return m->count < OPT_VISIBLE ? m->count : OPT_VISIBLE; }

static void opt_rect(options_menu *m, float *x, float *y, float *w, float *h) {
  *w = OPT_W;
  *h = opt_vis(m) * OPT_ROW + 16;
  *x = SCREEN_W - OPT_W - 28;
  *y = SCREEN_H - 92 - *h + (1 - ease_out(m->anim)) * 30;
}

static void opt_keep_visible(options_menu *m) {
  int v = opt_vis(m);
  if (m->sel < m->first) m->first = m->sel;
  if (m->sel >= m->first + v) m->first = m->sel - v + 1;
  if (m->first > m->count - v) m->first = m->count - v;
  if (m->first < 0) m->first = 0;
}

int options_update(options_menu *m) {
  m->anim = approach(m->anim, m->open ? 1.0f : 0.0f, 16);
  if (!m->open) return -1;
  if (m->just_opened) { m->just_opened = 0; opt_keep_visible(m); return -1; }
  float x, y, w, h;
  opt_rect(m, &x, &y, &w, &h);
  if (BTN_REPEAT(SCE_CTRL_DOWN)) { m->sel = (m->sel + 1) % m->count; g_focus_visible = 1; sfx_play(SFX_MOVE); opt_keep_visible(m); }
  if (BTN_REPEAT(SCE_CTRL_UP)) { m->sel = (m->sel + m->count - 1) % m->count; g_focus_visible = 1; sfx_play(SFX_MOVE); opt_keep_visible(m); }
  if (BTN_PRESSED(g_in.back) || BTN_PRESSED(SCE_CTRL_TRIANGLE)) { m->open = 0; sfx_play(SFX_BACK); return -2; }
  if (BTN_PRESSED(g_in.enter)) { m->open = 0; sfx_play(SFX_SELECT); return m->sel; }
  m->pressed = -1;
  int inside = point_in(g_in.sx, g_in.sy, x, y + 8, w, h - 16);
  /* drag to scroll long lists */
  if (g_in.touching && inside && g_in.dragging && m->count > OPT_VISIBLE) {
    m->drag_acc -= g_in.dy;
    while (m->drag_acc > OPT_ROW * 0.6f && m->first < m->count - OPT_VISIBLE) { m->first++; m->drag_acc -= OPT_ROW * 0.6f; sfx_play(SFX_MOVE); }
    while (m->drag_acc < -OPT_ROW * 0.6f && m->first > 0) { m->first--; m->drag_acc += OPT_ROW * 0.6f; sfx_play(SFX_MOVE); }
  }
  if (!g_in.touching) m->drag_acc = 0;
  if (g_in.touching && !g_in.dragging && point_in(g_in.tx, g_in.ty, x, y + 8, w, h - 16))
    m->pressed = m->first + (int)((g_in.ty - y - 8) / OPT_ROW);
  if (g_in.tap) {
    g_in.consumed = 1;
    if (inside) {
      int i = m->first + (int)((g_in.ty - y - 8) / OPT_ROW);
      if (i >= 0 && i < m->count) { m->open = 0; m->sel = i; sfx_play(SFX_SELECT); return i; }
      return -1;
    }
    m->open = 0;
    sfx_play(SFX_BACK);
    return -2;
  }
  if (g_in.touching) g_in.consumed = 1;
  return -1;
}

void options_draw(options_menu *m) {
  if (m->anim < 0.01f) return;
  float a = m->anim;
  ui_rect(0, 0, SCREEN_W, SCREEN_H, C_RGBA(0, 0, 0, (int)(90 * a)));
  float x, y, w, h;
  opt_rect(m, &x, &y, &w, &h);
  ui_xform xf = ui_xform_identity();
  xf.alpha = a;
  ui_push_xform(xf);
  ui_shadow(x, y, w, h, 12, 20, 0.6f);
  ui_rrect_vgrad(x, y, w, h, 12, C_RGBA(0x2a, 0x2a, 0x2e, 0xEE), C_RGBA(0x10, 0x10, 0x12, 0xEE));
  ui_rrect_vgrad(x + 1, y + 1, w - 2, 40, 11, C_RGBA(255, 255, 255, 30), C_RGBA(255, 255, 255, 0));
  ui_rrect_outline(x, y, w, h, 12, 1, C_RGBA(255, 255, 255, 60));
  int v = opt_vis(m);
  for (int r = 0; r < v; r++) {
    int i = m->first + r;
    if (i >= m->count) break;
    float ry = y + 8 + r * OPT_ROW;
    if (r > 0) ui_row_separator(x + 12, ry - 1, w - 24);
    if (i == m->pressed) ui_row_highlight(x + 6, ry + 2, w - 12, OPT_ROW - 4, 1);
    else if (i == m->sel && g_focus_visible && m->open) ui_focus_bar(x + 6, ry + 2, w - 12, OPT_ROW - 4);
    ui_text_clip_vc(x + 26, ry + OPT_ROW / 2.0f, 22, COL_TEXT, m->labels[i], w - 84);
    if (m->checked[i]) ui_icon(ICON_CHECK, x + w - 36, ry + OPT_ROW / 2.0f, 24, T->accent);
  }
  if (m->count > v) {
    /* scroll position */
    float th = h - 24, bh = fmaxf(24, th * v / m->count);
    float by = y + 12 + (th - bh) * m->first / (float)(m->count - v);
    ui_rrect(x + w - 8, by, 3, bh, 1.5f, C_RGBA(255, 255, 255, 110));
  }
  ui_pop_xform();
}

/* ---------------------------------------------------------------- */

void fmt_duration(char *buf, int len, int64_t ticks) {
  int mins = (int)(ticks / (10000000LL * 60));
  if (mins >= 60) snprintf(buf, len, "%d h %d min", mins / 60, mins % 60);
  else snprintf(buf, len, "%d min", mins ? mins : 1);
}

void fmt_clock(char *buf, int len, int64_t ms) {
  if (ms < 0) ms = 0;
  int s = (int)(ms / 1000);
  if (s >= 3600) snprintf(buf, len, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(buf, len, "%d:%02d", s / 60, s % 60);
}
