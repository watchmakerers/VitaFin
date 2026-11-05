#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/* System font (ScePvf) rasterised natively at each requested pixel size,
 * so text is pixel-sharp instead of a scaled bitmap. */

void font_init(void);
void font_shutdown(void);

/* Width of text in pixels at size px. */
float font_width(int px, const char *s);
/* Ascender (baseline offset from top of the line box) for size px. */
float font_ascent(int px);
/* Iterate glyph quads. cb receives screen-space rect and atlas uv rect. */
typedef void (*font_quad_fn)(void *ctx, float x, float y, float w, float h, float u0, float v0, float u1, float v1);
void font_layout(int px, float x, float baseline, const char *s, font_quad_fn cb, void *ctx);
/* 0 if the native renderer failed its self-test and vita2d's text is used instead. */
int font_native(void);
void font_fallback_draw(float x, float baseline, int px, unsigned int color, const char *s);
/* Atlas texture (U8 alpha). */
struct vita2d_texture *font_atlas(void);

#endif
