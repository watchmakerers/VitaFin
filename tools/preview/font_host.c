/* Host font: FreeType with a sans font, same API as font.c (Vita uses ScePvf). */
#include "font.h"
#include <vita2d.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <string.h>
#include <stdlib.h>
static FT_Library lib; static FT_Face face; static vita2d_texture *atlas; static unsigned char *pix; static int stride;
static int sx, sy, sh;
typedef struct { unsigned key; int x, y, w, h, left, top, adv64; } glyph;
static glyph tab[8192];
void font_init(void) { FT_Init_FreeType(&lib); FT_New_Face(lib, "/usr/share/fonts/noto/NotoSans-Regular.ttf", 0, &face);
  atlas = vita2d_create_empty_texture_format(2048, 2048, SCE_GXM_TEXTURE_FORMAT_U8_R111); pix = vita2d_texture_get_datap(atlas); stride = vita2d_texture_get_stride(atlas); }
void font_shutdown(void) {}
struct vita2d_texture *font_atlas(void) { return atlas; }
static glyph *get(int px, unsigned cp) {
  unsigned key = (px << 21) | cp; unsigned h = (key * 2654435761u) & 8191;
  while (tab[h].key && tab[h].key != key) h = (h + 1) & 8191;
  glyph *g = &tab[h]; if (g->key == key) return g;
  FT_Set_Pixel_Sizes(face, 0, px); if (FT_Load_Char(face, cp, FT_LOAD_RENDER)) return NULL;
  FT_Bitmap *b = &face->glyph->bitmap;
  if (sx + (int)b->width + 2 > 2048) { sx = 0; sy += sh; sh = 0; }
  for (unsigned r = 0; r < b->rows; r++) memcpy(pix + (sy + 1 + r) * stride + sx + 1, b->buffer + r * b->pitch, b->width);
  g->key = key; g->x = sx + 1; g->y = sy + 1; g->w = b->width; g->h = b->rows; g->left = face->glyph->bitmap_left; g->top = face->glyph->bitmap_top; g->adv64 = face->glyph->advance.x;
  sx += b->width + 2; if ((int)b->rows + 2 > sh) sh = b->rows + 2; return g; }
static int next(const char **p, unsigned *cp) { const unsigned char *s = (const unsigned char *)*p; if (!*s) return 0; unsigned c = *s++; int e = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0; if (e) c &= (0x3F >> e); while (e-- && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F); *cp = c; *p = (const char *)s; return 1; }
float font_width(int px, const char *s) { long pen = 0; unsigned cp; while (next(&s, &cp)) { glyph *g = get(px, cp); if (g) pen += g->adv64; } return pen / 64.f; }
float font_ascent(int px) { glyph *g = get(px, 'H'); return g ? g->top : px * 0.7f; }
void font_layout(int px, float x, float base, const char *s, font_quad_fn cb, void *ctx) {
  long pen = (long)(x * 64); unsigned cp;
  while (next(&s, &cp)) { glyph *g = get(px, cp); if (!g) continue;
    if (g->w) cb(ctx, roundf(pen / 64.f) + g->left, roundf(base) - g->top, g->w, g->h, g->x / 2048.f, g->y / 2048.f, (g->x + g->w) / 2048.f, (g->y + g->h) / 2048.f);
    pen += g->adv64; } }
int font_native(void) { return 1; }
void font_fallback_draw(float x, float b, int px, unsigned c, const char *s) {}
