/* Software stand-in for vita2d: rasterises the exact triangles the app submits.
 * 4x4 supersampled to approximate the Vita's MSAA, then box-filtered down. */
#include <vita2d.h>
#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 960
#define H 544
#define SS 3
static float fb[H * SS][W * SS][3];
typedef struct { int w, h, fmt; unsigned char *d; } htex;   /* fmt 0 = RGBA, 1 = U8 alpha */
#define HT(t) ((htex *)(t))
static int clip_on, cx0, cy0, cx1, cy1;
static char *pool; static size_t pool_off;

void host_begin_frame(void) { pool_off = 0; for (int y = 0; y < H * SS; y++) for (int x = 0; x < W * SS; x++) fb[y][x][0] = fb[y][x][1] = fb[y][x][2] = 0; }
void *vita2d_pool_memalign(unsigned int size, unsigned int al) {
  if (!pool) pool = malloc(64 << 20);
  pool_off = (pool_off + al - 1) / al * al;
  void *p = pool + pool_off; pool_off += size;
  return pool_off > (64u << 20) ? NULL : p;
}
void vita2d_enable_clipping(void) { clip_on = 1; }
void vita2d_disable_clipping(void) { clip_on = 0; }
void vita2d_set_clip_rectangle(int a, int b, int c, int d) { cx0 = a; cy0 = b; cx1 = c; cy1 = d; }

typedef struct { float x, y, r, g, b, a, u, v; } hv;

static void sample(htex *t, float u, float v, float *o) {
  if (!t) { o[0] = o[1] = o[2] = o[3] = 1; return; }
  /* bilinear */
  float fx = u * t->w - 0.5f, fy = v * t->h - 0.5f;
  int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float ax = fx - x0, ay = fy - y0;
  float acc[4] = {0};
  for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
    int x = x0 + i, y = y0 + j; if (x < 0) x = 0; if (y < 0) y = 0; if (x >= t->w) x = t->w - 1; if (y >= t->h) y = t->h - 1;
    float wgt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
    if (t->fmt == 1) { float a = t->d[y * ((t->w + 7) & ~7) + x] / 255.f; acc[0] += wgt; acc[1] += wgt; acc[2] += wgt; acc[3] += a * wgt; }
    else { unsigned char *p = t->d + (y * t->w + x) * 4; for (int k = 0; k < 4; k++) acc[k] += p[k] / 255.f * wgt; }
  }
  memcpy(o, acc, sizeof(acc));
}

static void tri(hv a, hv b, hv c, htex *t) {
  float minx = fminf(a.x, fminf(b.x, c.x)) * SS, maxx = fmaxf(a.x, fmaxf(b.x, c.x)) * SS;
  float miny = fminf(a.y, fminf(b.y, c.y)) * SS, maxy = fmaxf(a.y, fmaxf(b.y, c.y)) * SS;
  int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
  int lx0 = 0, ly0 = 0, lx1 = W * SS, ly1 = H * SS;
  if (clip_on) { lx0 = cx0 * SS; ly0 = cy0 * SS; lx1 = cx1 * SS; ly1 = cy1 * SS; }
  if (x0 < lx0) x0 = lx0; if (y0 < ly0) y0 = ly0; if (x1 > lx1) x1 = lx1; if (y1 > ly1) y1 = ly1; if (x1 > W*SS) x1 = W*SS; if (y1 > H*SS) y1 = H*SS; if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
  float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (fabsf(area) < 1e-6f) return;
  for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
    float px = (x + 0.5f) / SS, py = (y + 0.5f) / SS;
    float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
    float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
    float w2 = 1 - w0 - w1;
    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
    float col[4] = { a.r * w0 + b.r * w1 + c.r * w2, a.g * w0 + b.g * w1 + c.g * w2, a.b * w0 + b.b * w1 + c.b * w2, a.a * w0 + b.a * w1 + c.a * w2 };
    if (t) { float s[4]; sample(t, a.u * w0 + b.u * w1 + c.u * w2, a.v * w0 + b.v * w1 + c.v * w2, s); for (int k = 0; k < 4; k++) col[k] *= s[k]; }
    float al = col[3];
    for (int k = 0; k < 3; k++) fb[y][x][k] = fb[y][x][k] * (1 - al) + col[k] * al;
  }
}

static hv mk(float x, float y, unsigned c, float u, float v) {
  hv o = { x, y, (c & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, ((c >> 16) & 0xFF) / 255.f, ((c >> 24) & 0xFF) / 255.f, u, v };
  return o;
}

static void prim(SceGxmPrimitiveType mode, hv *v, size_t n, htex *t) {
  if (mode == SCE_GXM_PRIMITIVE_TRIANGLES) for (size_t i = 0; i + 2 < n + 0 && i + 2 < n; i += 3) tri(v[i], v[i + 1], v[i + 2], t);
  else if (mode == SCE_GXM_PRIMITIVE_TRIANGLE_STRIP) for (size_t i = 0; i + 2 < n; i++) tri(v[i], v[i + 1], v[i + 2], t);
  else if (mode == SCE_GXM_PRIMITIVE_TRIANGLE_FAN) for (size_t i = 1; i + 1 < n; i++) tri(v[0], v[i], v[i + 1], t);
}

void vita2d_draw_array(SceGxmPrimitiveType mode, const vita2d_color_vertex *vs, size_t n) {
  hv *v = malloc(n * sizeof(hv));
  for (size_t i = 0; i < n; i++) v[i] = mk(vs[i].x, vs[i].y, vs[i].color, 0, 0);
  prim(mode, v, n, NULL); free(v);
}
void vita2d_draw_array_textured(const vita2d_texture *t, SceGxmPrimitiveType mode, const vita2d_texture_vertex *vs, size_t n, unsigned int color) {
  hv *v = malloc(n * sizeof(hv));
  for (size_t i = 0; i < n; i++) v[i] = mk(vs[i].x, vs[i].y, color, vs[i].u, vs[i].v);
  prim(mode, v, n, HT(t)); free(v);
}

vita2d_texture *vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat f) {
  htex *t = calloc(1, sizeof(htex)); t->w = w; t->h = h; t->fmt = f == SCE_GXM_TEXTURE_FORMAT_U8_R111;
  t->d = calloc(1, (size_t)((w + 7) & ~7) * h * (t->fmt ? 1 : 4)); return (vita2d_texture *)t;
}
unsigned int vita2d_texture_get_width(const vita2d_texture *t) { return HT(t)->w; }
unsigned int vita2d_texture_get_height(const vita2d_texture *t) { return HT(t)->h; }
unsigned int vita2d_texture_get_stride(const vita2d_texture *t) { return ((HT(t)->w + 7) & ~7) * (HT(t)->fmt ? 1 : 4); }
void *vita2d_texture_get_datap(const vita2d_texture *t) { return HT(t)->d; }
void vita2d_texture_set_filters(vita2d_texture *t, SceGxmTextureFilter a, SceGxmTextureFilter b) {}
void vita2d_free_texture(vita2d_texture *t) { if (t) { free(HT(t)->d); free(t); } }
void vita2d_wait_rendering_done(void) {}

vita2d_texture *vita2d_load_JPEG_buffer(const void *buf, unsigned long size) {
  struct jpeg_decompress_struct c; struct jpeg_error_mgr e;
  c.err = jpeg_std_error(&e); jpeg_create_decompress(&c);
  jpeg_mem_src(&c, buf, size); jpeg_read_header(&c, 1); c.out_color_space = JCS_RGB; jpeg_start_decompress(&c);
  htex *t = (htex *)vita2d_create_empty_texture_format(c.output_width, c.output_height, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
  unsigned char *row = malloc(c.output_width * 3);
  while (c.output_scanline < c.output_height) {
    int y = c.output_scanline; jpeg_read_scanlines(&c, &row, 1);
    for (unsigned x = 0; x < c.output_width; x++) { unsigned char *p = t->d + (y * t->w + x) * 4; p[0] = row[x*3]; p[1] = row[x*3+1]; p[2] = row[x*3+2]; p[3] = 255; }
  }
  free(row); jpeg_finish_decompress(&c); jpeg_destroy_decompress(&c);
  return (vita2d_texture *)t;
}
vita2d_texture *vita2d_load_PNG_buffer(const void *b) { return NULL; }
vita2d_texture *vita2d_load_BMP_buffer(const void *b) { return NULL; }

void host_save(const char *path) {
  FILE *f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int k = 0; k < 3; k++) {
    float s = 0; for (int j = 0; j < SS; j++) for (int i = 0; i < SS; i++) s += fb[y * SS + j][x * SS + i][k];
    s /= SS * SS; int v = (int)(powf(s, 1.0f) * 255 + 0.5f); fputc(v < 0 ? 0 : v > 255 ? 255 : v, f);
  }
  fclose(f);
}
