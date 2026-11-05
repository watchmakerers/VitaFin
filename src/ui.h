#ifndef UI_H
#define UI_H

#include <vita2d.h>
#include <stdint.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define STATUS_H 30
#define TITLE_H 72                      /* centred page title under the status bar */
#define CONTENT_Y (STATUS_H + TITLE_H)

#define C_RGBA(r, g, b, a) RGBA8(r, g, b, a)

/* Colour theme. The default reproduces the green of the PS Vita Settings app. */
typedef struct {
  const char *name;
  uint32_t bg_top, bg_mid, bg_bot, glow;
  uint32_t accent;      /* checkmarks, progress, badges */
} ui_theme;

#define NUM_THEMES 5
extern const ui_theme g_themes[NUM_THEMES];
extern const ui_theme *T;
void ui_set_theme(int idx);

#define COL_TEXT     C_RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_TEXT_SUB C_RGBA(0xFF, 0xFF, 0xFF, 0xB8)
#define COL_TEXT_DIM C_RGBA(0xFF, 0xFF, 0xFF, 0x80)
#define COL_DARK     C_RGBA(0x1A, 0x1A, 0x1A, 0xFF)

typedef struct { float x, y; } vec2;

void ui_init(void);
void ui_shutdown(void);
void ui_begin_frame(void);   /* call before vita2d_start_drawing */
unsigned ui_frame(void);
float ui_time(void);
float ui_dt(void);

uint32_t col_alpha(uint32_t c, float a);
uint32_t col_lerp(uint32_t a, uint32_t b, float t);
float ease_out(float t);
float ease_in_out(float t);
float approach(float cur, float target, float rate);   /* frame-rate independent smoothing */

/* --- page transform: everything drawn between push/pop is moved, faded and
 *     optionally swung back in 3D (rotation about a vertical axis). --- */
typedef struct {
  float tx, ty;       /* translation */
  float alpha;        /* multiplies every colour */
  float theta;        /* rotation (radians) about x = axis_x; 0 = flat */
  float axis_x;
  float depth;        /* push away from the viewer */
  float scale;        /* uniform scale around screen centre */
} ui_xform;

void ui_push_xform(ui_xform x);      /* composes with the current one */
void ui_pop_xform(void);
ui_xform ui_xform_identity(void);

/* --- primitives --- */
void ui_rect(float x, float y, float w, float h, uint32_t c);
void ui_vgrad(float x, float y, float w, float h, uint32_t top, uint32_t bot);
void ui_hgrad(float x, float y, float w, float h, uint32_t left, uint32_t right);
void ui_rrect(float x, float y, float w, float h, float r, uint32_t c);
void ui_rrect_vgrad(float x, float y, float w, float h, float r, uint32_t top, uint32_t bot);
void ui_rrect_outline(float x, float y, float w, float h, float r, float thick, uint32_t c);
void ui_circle(float x, float y, float r, uint32_t c);
void ui_circle_vgrad(float x, float y, float r, uint32_t top, uint32_t bot);
void ui_circle_radial(float x, float y, float r, uint32_t inner, uint32_t outer);
void ui_ring(float x, float y, float r, float thick, uint32_t c);
void ui_arc(float x, float y, float r, float thick, float a0, float a1, uint32_t c);
void ui_line(float x1, float y1, float x2, float y2, float w, uint32_t c);
void ui_tri(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c);
void ui_glow(float x, float y, float w, float h, float r, float spread, uint32_t c);
void ui_shadow(float x, float y, float w, float h, float r, float spread, float alpha);

void ui_clip(int x, int y, int w, int h);
void ui_unclip(void);

/* --- textures --- */
void ui_tex(vita2d_texture *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t tint);
void ui_tex_cover(vita2d_texture *t, float x, float y, float w, float h, float alpha);
void ui_tex_fit(vita2d_texture *t, float x, float y, float w, float h, float alpha);

/* --- text (system font, natively rasterised; sizes in pixels) --- */
void ui_text(float x, float y, int px, uint32_t c, const char *s);        /* y = top of the line box */
void ui_text_vc(float x, float cy, int px, uint32_t c, const char *s);    /* centred on cy */
int  ui_text_w(int px, const char *s);
void ui_text_clip(float x, float y, int px, uint32_t c, const char *s, float max_w);
void ui_text_clip_vc(float x, float cy, int px, uint32_t c, const char *s, float max_w);
void ui_text_center(float cx, float cy, int px, uint32_t c, const char *s);
void ui_text_right(float rx, float cy, int px, uint32_t c, const char *s);
void ui_text_shadow_vc(float x, float cy, int px, uint32_t c, const char *s); /* soft drop shadow */
int  ui_text_wrap(float x, float y, int px, uint32_t c, const char *s, float max_w, float line_h, int max_lines, int draw);

/* --- icons --- */
typedef enum {
  ICON_FOLDER, ICON_FILM, ICON_TV, ICON_MUSIC, ICON_VIDEO, ICON_PLAY, ICON_PAUSE, ICON_RESUME,
  ICON_NEXT, ICON_SERVER, ICON_USER, ICON_KEY, ICON_SIGNOUT, ICON_CHECK, ICON_BACK, ICON_DOTS,
  ICON_COLLECTION, ICON_SETTINGS, ICON_CHEVRON, ICON_HOME, ICON_NONE
} icon_id;
void ui_icon(icon_id id, float cx, float cy, float size, uint32_t c);
icon_id ui_icon_for_item(const char *type, const char *collection_type);
void ui_star(float cx, float cy, float r, uint32_t c);

/* --- chrome --- */
void ui_background(void);
void ui_watermark(icon_id icon);           /* large faint page emblem, like Settings */
void ui_status_bar(void);
void ui_page_title(const char *title);
/* Bottom-corner round buttons. Return 1 when tapped this frame. */
int ui_back_button(int visible);
int ui_options_button(int visible);
void ui_spinner(float cx, float cy, float r, float alpha);
/* Settings-style row decorations */
void ui_row_separator(float x, float y, float w);
void ui_row_highlight(float x, float y, float w, float h, float a);   /* glossy white bar */
void ui_focus_bar(float x, float y, float w, float h);
void ui_focus_ring(float x, float y, float w, float h, float r);
/* Vita system push button (light glossy, dark label). Returns 1 when tapped. */
int ui_button(const char *label, icon_id icon, float x, float y, float w, float h, int focused);
void ui_progress_bar(float x, float y, float w, float h, float frac);
void ui_badge_check(float cx, float cy);

extern int g_focus_visible;
extern int g_ui_input;   /* 0 while drawing background pages: widgets ignore touch */

/* --- scrolling list / grid --- */
typedef struct {
  int count;
  int cols;
  float x, y, w, h;
  float cell_w, cell_h, gap_x, gap_y, pad_top, pad_bottom;
  int sel;
  float scroll, target, vel;
  int pressed;           /* index under finger, -1 */
  int press_frames;
  int dragging;
  int hl_idx;            /* index whose press highlight is fading */
  float hl_a;
  float focus_x, focus_y; /* animated focus position */
  int focus_init;
  float appear;          /* seconds since content arrived (entry cascade) */
  int last_count;
} scroller;

typedef void (*cell_draw_fn)(void *ctx, int idx, float x, float y, float w, float h, int focused, float press_a);

void scroller_init(scroller *s, int cols, float x, float y, float w, float h, float cell_w, float cell_h, float gap_x, float gap_y);
void scroller_set_count(scroller *s, int count);
int scroller_update(scroller *s, int input_enabled);
void scroller_draw(scroller *s, cell_draw_fn fn, void *ctx);
void scroller_cell_rect(scroller *s, int idx, float *x, float *y);
float scroller_content_h(scroller *s);
void scroller_restart_appear(scroller *s);

/* --- options menu --- */
#define OPT_MAX 32
#define OPT_VISIBLE 6
typedef struct {
  int open;
  int just_opened;   /* ignore the button press that opened it */
  float anim;
  int count;
  int sel;
  int pressed;
  int first;          /* first visible row when the list scrolls */
  float drag_acc;
  const char *labels[OPT_MAX];
  int checked[OPT_MAX];
} options_menu;

void options_open(options_menu *m);
void options_clear(options_menu *m);
void options_add(options_menu *m, const char *label, int checked);
int options_update(options_menu *m);
void options_draw(options_menu *m);

void fmt_duration(char *buf, int len, int64_t ticks);
void fmt_clock(char *buf, int len, int64_t ms);

#endif
