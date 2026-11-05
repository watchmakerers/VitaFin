#include "font.h"
#include "log.h"
#include <psp2/pvf.h>
#include <vita2d.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Glyphs are rasterised with ScePvf at the exact pixel size requested and packed
 * into one U8 atlas. vita2d's own PVF path renders at ~18 px and scales, which is
 * what made large text blurry. */

#define ATLAS_W 1024
#define ATLAS_H 1024
#define MARGIN 1
#define MAX_SIZES 16
#define HASH_SIZE 4096

typedef struct {
  uint32_t key;   /* size << 21 | codepoint, 0 = empty */
  int16_t x, y;   /* atlas position (top-left of bitmap) */
  int16_t w, h;
  int16_t left, top;   /* bearing: bitmap offset from pen / baseline */
  int32_t adv64;       /* advance in 1/64 px */
} glyph;

typedef struct {
  int px;
  float cap;
} sized;

static ScePvfLibId g_lib;
static ScePvfFontIndex g_idx_main, g_idx_cjk;
static int g_have_cjk;
/* ScePvf won't open the same font twice, so each font has one handle and we
 * switch its size when a different pixel size is needed. */
static ScePvfFontId g_main, g_cjk;
static int g_main_px, g_cjk_px;
static int g_ok;              /* glyph rendering verified at init */
static vita2d_pvf *g_fallback; /* vita2d's own text, if ours can't render */
static int g_errs;
static sized g_sizes[MAX_SIZES];
static int g_nsizes;
static glyph g_hash[HASH_SIZE];
static vita2d_texture *g_atlas;
static uint8_t *g_pix;
static int g_stride;
static int g_shelf_x, g_shelf_y, g_shelf_h;
static int g_full;

static glyph *get_glyph(sized *sz, unsigned cp);
static sized *get_size(int px);
static ScePvfFontId open_font(ScePvfFontIndex idx);

static void *pa(void *u, unsigned int s) { (void)u; return memalign(sizeof(int), (s + 3) & ~3); }
static void *pr(void *u, void *p, unsigned int s) { (void)u; return realloc(p, (s + 3) & ~3); }
static void pf(void *u, void *p) { (void)u; free(p); }

static ScePvfFontIndex find_font(int lang, int *ok) {
  ScePvfFontStyleInfo st;
  ScePvfError err = 0;
  memset(&st, 0, sizeof(st));
  st.languageCode = lang;
  st.familyCode = SCE_PVF_DEFAULT_FAMILY_CODE;
  st.style = SCE_PVF_DEFAULT_STYLE_CODE;
  ScePvfFontIndex i = scePvfFindOptimumFont(g_lib, &st, &err);
  *ok = err == 0;
  return i;
}

void font_init(void) {
  ScePvfError err = 0;
  ScePvfInitRec p = { NULL, 2, NULL, NULL, pa, pr, pf };
  g_lib = scePvfNewLib(&p, &err);
  if (err) { applog("scePvfNewLib failed 0x%08X", (unsigned)err); return; }
  /* Same lib setup as vita2d: 128 dpi with a matching EM so 1/64 metrics are pixels. */
  scePvfSetEM(g_lib, 72.0f / (10.125f * 128.0f));
  scePvfSetResolution(g_lib, 128.0f, 128.0f);
  int ok;
  g_idx_main = find_font(SCE_PVF_DEFAULT_LANGUAGE_CODE, &ok);
  g_idx_cjk = find_font(SCE_PVF_LANGUAGE_J, &g_have_cjk);

  g_atlas = vita2d_create_empty_texture_format(ATLAS_W, ATLAS_H, SCE_GXM_TEXTURE_FORMAT_U8_R111);
  g_pix = vita2d_texture_get_datap(g_atlas);
  g_stride = vita2d_texture_get_stride(g_atlas);
  memset(g_pix, 0, g_stride * ATLAS_H);
  vita2d_texture_set_filters(g_atlas, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);

  g_main = open_font(g_idx_main);
  if (g_have_cjk && g_idx_cjk != g_idx_main) g_cjk = open_font(g_idx_cjk);

  /* Self-test: rasterise 'W' and check pixels actually landed in the atlas. */
  glyph *g = get_glyph(get_size(24), 'W');
  int ink = 0;
  if (g) for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++) ink += g_pix[(g->y + y) * g_stride + g->x + x] > 0;
  float cap = get_size(24)->cap, adv = g ? g->adv64 / 64.0f : 0;
  /* sane metrics for a 24 px 'W': visible ink, plausible height, cap height and advance */
  g_ok = g && ink > 10 && g->h >= 8 && g->h <= 40 && cap >= 10 && cap <= 30 && adv >= 8 && adv <= 48;
  applog("font: 24px W h=%d adv=%.1f cap=%.1f", g ? g->h : -1, adv, cap);
  applog("font: main=%d cjk=%d glyph=%s ink=%d -> %s", (int)(g_main != 0), (int)(g_cjk != 0), g ? "yes" : "no", ink,
         g_ok ? "native renderer" : "FALLBACK to vita2d pvf");
  if (!g_ok) g_fallback = vita2d_load_default_pvf();
}

int font_native(void) { return g_ok; }

void font_fallback_draw(float x, float baseline, int px, unsigned int color, const char *s) {
  if (g_fallback) vita2d_pvf_draw_text(g_fallback, (int)x, (int)baseline, color, px / 18.0f, s);
}

float font_fallback_width(int px, const char *s) {
  return g_fallback ? vita2d_pvf_text_width(g_fallback, px / 18.0f, s) : 0;
}

void font_shutdown(void) {
  if (g_main) scePvfClose(g_main);
  if (g_cjk) scePvfClose(g_cjk);
  if (g_lib) scePvfDoneLib(g_lib);
  if (g_atlas) vita2d_free_texture(g_atlas);
}

struct vita2d_texture *font_atlas(void) { return g_atlas; }

static ScePvfFontId open_font(ScePvfFontIndex idx) {
  ScePvfError err = 0;
  ScePvfFontId f = scePvfOpen(g_lib, idx, 0, &err);
  if (err) { applog("scePvfOpen(%d) failed 0x%08X", (int)idx, (unsigned)err); return 0; }
  return f;
}

static void set_px(ScePvfFontId f, int *cur, int px) {
  if (!f || *cur == px) return;
  float pt = px * 72.0f / 128.0f;
  ScePvfError e = scePvfSetCharSize(f, pt, pt);
  if (e && g_errs++ < 4) applog("scePvfSetCharSize %d px failed 0x%08X", px, (unsigned)e);
  *cur = px;
}

static sized *get_size(int px) {
  for (int i = 0; i < g_nsizes; i++) if (g_sizes[i].px == px) return &g_sizes[i];
  if (g_nsizes >= MAX_SIZES) {
    sized *best = &g_sizes[0];
    for (int i = 1; i < g_nsizes; i++) if (abs(g_sizes[i].px - px) < abs(best->px - px)) best = &g_sizes[i];
    return best;
  }
  sized *s = &g_sizes[g_nsizes++];
  s->px = px;
  s->cap = px * 0.7f;
  ScePvfCharInfo ci;
  set_px(g_main, &g_main_px, px);
  if (g_main && scePvfGetCharInfo(g_main, 'H', &ci) >= 0 && ci.glyphMetrics.horizontalBearingY64 > 0)
    s->cap = ci.glyphMetrics.horizontalBearingY64 / 64.0f;
  return s;
}

static glyph *lookup(uint32_t key) {
  uint32_t h = (key * 2654435761u) & (HASH_SIZE - 1);
  for (int i = 0; i < HASH_SIZE; i++) {
    glyph *g = &g_hash[(h + i) & (HASH_SIZE - 1)];
    if (g->key == key || g->key == 0) return g;
  }
  return NULL;
}

static glyph *get_glyph(sized *sz, unsigned cp) {
  uint32_t key = ((uint32_t)sz->px << 21) | (cp & 0x1FFFFF);
  glyph *g = lookup(key);
  if (!g) return NULL;
  if (g->key == key) return g;
  if (g_full) return NULL;

  ScePvfFontId f = g_main;
  ScePvfCharInfo ci;
  ScePvfIrect r;
  set_px(g_main, &g_main_px, sz->px);
  if (!f || scePvfGetCharInfo(f, cp, &ci) < 0 || (ci.glyphMetrics.horizontalAdvance64 == 0 && cp != ' ')) {
    f = g_cjk;
    set_px(g_cjk, &g_cjk_px, sz->px);
    if (!f || scePvfGetCharInfo(f, cp, &ci) < 0) return NULL;
  }
  if (scePvfGetCharImageRect(f, cp, &r) < 0) return NULL;

  int w = r.width + 2 * MARGIN, h = r.height + 2 * MARGIN;
  if (g_shelf_x + w > ATLAS_W) { g_shelf_x = 0; g_shelf_y += g_shelf_h; g_shelf_h = 0; }
  if (g_shelf_y + h > ATLAS_H) { g_full = 1; applog("font atlas full"); return NULL; }
  int ax = g_shelf_x, ay = g_shelf_y;
  g_shelf_x += w;
  if (h > g_shelf_h) g_shelf_h = h;

  if (r.width && r.height) {
    ScePvfUserImageBufferRec img;
    img.pixelFormat = SCE_PVF_USERIMAGE_DIRECT8;
    img.xPos64 = ((ax + MARGIN) << 6) - ci.glyphMetrics.horizontalBearingX64;
    img.yPos64 = ((ay + MARGIN) << 6) + ci.glyphMetrics.horizontalBearingY64;
    img.rect.width = ATLAS_W;
    img.rect.height = ATLAS_H;
    img.bytesPerLine = g_stride;
    img.reserved = 0;
    img.buffer = g_pix;
    scePvfGetCharGlyphImage(f, cp, &img);
  }
  g->key = key;
  g->x = ax + MARGIN;
  g->y = ay + MARGIN;
  g->w = r.width;
  g->h = r.height;
  g->left = (int16_t)floorf(ci.glyphMetrics.horizontalBearingX64 / 64.0f);
  g->top = (int16_t)floorf(ci.glyphMetrics.horizontalBearingY64 / 64.0f + 0.5f);
  g->adv64 = ci.glyphMetrics.horizontalAdvance64;
  return g;
}

static int utf8_next(const char **p, unsigned *cp) {
  const unsigned char *s = (const unsigned char *)*p;
  if (!*s) return 0;
  unsigned c = *s++;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (extra) c &= (0x3F >> extra);
  while (extra-- && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F);
  *cp = c;
  *p = (const char *)s;
  return 1;
}

float font_width(int px, const char *s) {
  if (!g_ok) return font_fallback_width(px, s);
  sized *sz = get_size(px);
  int64_t pen = 0;
  unsigned cp;
  while (utf8_next(&s, &cp)) {
    glyph *g = get_glyph(sz, cp);
    if (g) pen += g->adv64;
  }
  return pen / 64.0f;
}

float font_ascent(int px) { return g_ok ? get_size(px)->cap : px * 0.7f; }

void font_layout(int px, float x, float baseline, const char *s, font_quad_fn cb, void *ctx) {
  sized *sz = get_size(px);
  int64_t pen = (int64_t)(x * 64);
  float by = roundf(baseline);
  unsigned cp;
  while (utf8_next(&s, &cp)) {
    glyph *g = get_glyph(sz, cp);
    if (!g) continue;
    if (g->w && g->h) {
      float gx = roundf(pen / 64.0f) + g->left;
      float gy = by - g->top;
      cb(ctx, gx, gy, g->w, g->h,
         (float)g->x / ATLAS_W, (float)g->y / ATLAS_H,
         (float)(g->x + g->w) / ATLAS_W, (float)(g->y + g->h) / ATLAS_H);
    }
    pen += g->adv64;
  }
}
