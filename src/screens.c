#include "screens.h"
#include "config.h"
#include "images.h"
#include "ime.h"
#include "input.h"
#include "jellyfin.h"
#include "player.h"
#include "sfx.h"
#include "ui.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SCR_LOGIN, SCR_HOME, SCR_BROWSE, SCR_DETAIL } scr_kind;
typedef enum { SRC_CHILDREN, SRC_RESUME, SRC_NEXTUP, SRC_SEASONS, SRC_EPISODES } src_kind;
typedef enum { LAY_LIST, LAY_POSTER, LAY_SQUARE, LAY_TRACKS, LAY_SETTINGS } layout_kind;

#define PAGE 120
#define MAX_STACK 16
#define ROW_X 64
#define ROW_W (SCREEN_W - 2 * ROW_X)

typedef struct screen {
  scr_kind kind;
  src_kind src;
  layout_kind layout;
  char title[192];
  icon_id emblem;
  jf_item parent;
  jf_list list;
  scroller sc;
  options_menu opt;
  char sort[24];
  int desc;

  pthread_t th;
  int th_running;
  volatile int done;
  int rc;
  int loading_more;
  jf_list result;
  jf_item result_item;
  int needs_reload;
  int loaded_once;

  int has_resume, has_nextup;

  jf_item detail;
  int have_detail;
  int btn_sel;
  int nbtn;
  float detail_appear;

  char l_server[256], l_user[128], l_pass[128];
  int l_sel, l_editing, l_busy;
} screen;

static screen *g_stack[MAX_STACK];
static int g_depth;
static int g_playing;
static int g_pending_msg;   /* 1 = info, 2 = sign-out confirm */

/* page-stack animation */
static struct {
  int active;
  int dir;          /* +1 push, -1 pop */
  float t;
  screen *leaving;  /* popped page, kept alive until its exit animation ends */
} g_nav;

static screen *top(void) { return g_depth ? g_stack[g_depth - 1] : NULL; }

/* ------------------------------------------------------------------ */
/* Async loading                                                       */
/* ------------------------------------------------------------------ */

static void *load_thread(void *arg) {
  screen *s = arg;
  jf_list l = {0};
  int rc = 0;
  int start = s->loading_more ? s->list.count : 0;
  switch (s->kind) {
    case SCR_HOME: {
      rc = jf_get_views(&l);
      if (rc == 0) {
        jf_list r = {0}, n = {0};
        s->has_resume = jf_get_resume(&r, 1) == 0 && r.count > 0;
        s->has_nextup = jf_get_nextup(&n, 1) == 0 && n.count > 0;
        jf_list_free(&r);
        jf_list_free(&n);
      }
      break;
    }
    case SCR_BROWSE:
      switch (s->src) {
        case SRC_CHILDREN: rc = jf_get_children(&s->parent, s->sort, s->desc, start, PAGE, &l); break;
        case SRC_RESUME: rc = jf_get_resume(&l, 60); break;
        case SRC_NEXTUP: rc = jf_get_nextup(&l, 60); break;
        case SRC_SEASONS: rc = jf_get_seasons(s->parent.id, &l); break;
        case SRC_EPISODES: rc = jf_get_episodes(s->parent.series_id, s->parent.id, &l); break;
      }
      break;
    case SCR_DETAIL:
      memset(&s->result_item, 0, sizeof(s->result_item));
      rc = jf_get_item(s->parent.id, &s->result_item);
      break;
    default:
      break;
  }
  s->result = l;
  s->rc = rc;
  s->done = 1;
  return NULL;
}

static void start_load(screen *s, int more) {
  if (s->th_running) return;
  s->done = 0;
  s->loading_more = more;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 * 1024);
  pthread_create(&s->th, &a, load_thread, s);
  pthread_attr_destroy(&a);
  s->th_running = 1;
}

static void goto_login(const char *msg);

/* 1 = a load just completed, -1 = navigated away (s freed), 0 = nothing. */
static int poll_load(screen *s) {
  if (!s->th_running || !s->done) return 0;
  pthread_join(s->th, NULL);
  s->th_running = 0;
  int first = !s->loaded_once;
  s->loaded_once = 1;
  if (s->rc == -401) { jf_list_free(&s->result); goto_login("Your session has expired. Please sign in again."); return -1; }
  if (s->kind == SCR_DETAIL) {
    if (s->rc == 0) {
      if (s->have_detail) jf_item_clear(&s->detail);
      s->detail = s->result_item;
      s->have_detail = 1;
    }
    return 1;
  }
  if (s->loading_more) {
    jf_list_append(&s->list, &s->result);
  } else {
    int keep = s->sc.sel;
    jf_list_free(&s->list);
    s->list = s->result;
    memset(&s->result, 0, sizeof(s->result));
    s->sc.sel = keep;
  }
  int extra = s->kind == SCR_HOME ? s->has_resume + s->has_nextup : 0;
  scroller_set_count(&s->sc, s->list.count + extra);
  if (first) scroller_restart_appear(&s->sc);
  if (s->rc != 0 && !s->list.count && !g_pending_msg) {
    msg_open("Could not connect to the server.", 0);
    g_pending_msg = 1;
  }
  return 1;
}

static void free_screen(screen *s) {
  if (!s) return;
  if (s->th_running) {
    pthread_join(s->th, NULL);
    jf_list_free(&s->result);
    if (s->kind == SCR_DETAIL && s->rc == 0) jf_item_clear(&s->result_item);
  }
  jf_list_free(&s->list);
  jf_item_clear(&s->parent);
  if (s->have_detail) jf_item_clear(&s->detail);
  free(s);
}

static void finish_nav(void) {
  if (g_nav.leaving) { free_screen(g_nav.leaving); g_nav.leaving = NULL; }
  g_nav.active = 0;
}

static void push(screen *s) {
  finish_nav();
  if (g_depth >= MAX_STACK) { free_screen(g_stack[1]); memmove(&g_stack[1], &g_stack[2], (MAX_STACK - 2) * sizeof(screen *)); g_depth--; }
  g_stack[g_depth++] = s;
  images_cancel_pending();
  if (g_depth > 1) { g_nav.active = 1; g_nav.dir = 1; g_nav.t = 0; sfx_play(SFX_OPEN); }
}

static void pop(void) {
  if (g_depth <= 1) return;
  finish_nav();
  sfx_play(SFX_BACK);
  g_nav.leaving = g_stack[--g_depth];
  g_nav.active = 1;
  g_nav.dir = -1;
  g_nav.t = 0;
  images_cancel_pending();
  screen *t = top();
  if (t && t->needs_reload) { t->needs_reload = 0; start_load(t, 0); }
}

static void clear_stack(void) {
  finish_nav();
  while (g_depth) free_screen(g_stack[--g_depth]);
}

/* ------------------------------------------------------------------ */
/* Screen construction                                                 */
/* ------------------------------------------------------------------ */

static screen *new_screen(scr_kind k) {
  screen *s = calloc(1, sizeof(screen));
  s->kind = k;
  s->emblem = ICON_NONE;
  snprintf(s->sort, sizeof(s->sort), "SortName");
  return s;
}

static void setup_layout(screen *s) {
  float y = CONTENT_Y, h = SCREEN_H - CONTENT_Y;
  switch (s->layout) {
    case LAY_POSTER: scroller_init(&s->sc, 6, 0, y, SCREEN_W, h, 124, 186 + 40, 22, 16); break;
    case LAY_SQUARE: scroller_init(&s->sc, 5, 0, y, SCREEN_W, h, 154, 154 + 54, 24, 14); break;
    case LAY_TRACKS: scroller_init(&s->sc, 1, 0, y, SCREEN_W, h, ROW_W, 64, 0, 0); break;
    case LAY_SETTINGS: scroller_init(&s->sc, 1, 0, y, SCREEN_W, h, ROW_W, 76, 0, 0); break;
    case LAY_LIST:
    default: scroller_init(&s->sc, 1, 0, y, SCREEN_W, h, ROW_W, 100, 0, 0); break;
  }
  s->sc.pad_top = (s->layout == LAY_POSTER || s->layout == LAY_SQUARE) ? 14 : 4;
}

static void open_login(void) {
  screen *s = new_screen(SCR_LOGIN);
  snprintf(s->title, sizeof(s->title), "Sign In to Jellyfin");
  s->emblem = ICON_SERVER;
  snprintf(s->l_server, sizeof(s->l_server), "%s", g_cfg.server);
  snprintf(s->l_user, sizeof(s->l_user), "%s", g_cfg.username);
  s->layout = LAY_SETTINGS;
  setup_layout(s);
  scroller_set_count(&s->sc, 3);
  s->sc.h = s->sc.pad_top + 3 * s->sc.cell_h + 4;
  s->sc.pad_bottom = 0;
  push(s);
}

static void open_home(void) {
  screen *s = new_screen(SCR_HOME);
  snprintf(s->title, sizeof(s->title), "%s", g_cfg.server_name[0] ? g_cfg.server_name : "Jellyfin");
  s->emblem = ICON_HOME;
  s->layout = LAY_LIST;
  setup_layout(s);
  s->sc.cell_h = 86;
  push(s);
  start_load(s, 0);
}

static void open_browse(const jf_item *parent, src_kind src, const char *title) {
  screen *s = new_screen(SCR_BROWSE);
  s->src = src;
  if (parent) jf_item_copy(&s->parent, parent);
  snprintf(s->title, sizeof(s->title), "%s", title);
  const char *ct = parent ? parent->collection_type : "";
  if (src == SRC_SEASONS) s->layout = LAY_POSTER;
  else if (src != SRC_CHILDREN) s->layout = LAY_LIST;
  else if (!strcmp(ct, "movies") || !strcmp(ct, "tvshows") || !strcmp(ct, "boxsets") || (parent && !strcmp(parent->type, "BoxSet"))) s->layout = LAY_POSTER;
  else if (!strcmp(ct, "music")) s->layout = LAY_SQUARE;
  else if (parent && !strcmp(parent->type, "MusicAlbum")) s->layout = LAY_TRACKS;
  else s->layout = LAY_LIST;
  s->emblem = src == SRC_RESUME ? ICON_RESUME : src == SRC_NEXTUP ? ICON_NEXT :
              parent ? ui_icon_for_item(parent->type, parent->collection_type) : ICON_FOLDER;
  setup_layout(s);
  push(s);
  start_load(s, 0);
}

static void open_detail(const jf_item *it) {
  screen *s = new_screen(SCR_DETAIL);
  jf_item_copy(&s->parent, it);
  snprintf(s->title, sizeof(s->title), "%s", !strcmp(it->type, "Episode") && it->series_name[0] ? it->series_name : it->name);
  s->emblem = ui_icon_for_item(it->type, "");
  push(s);
  start_load(s, 0);
}

static void goto_login(const char *msg) {
  clear_stack();
  g_cfg.token[0] = 0;
  config_save();
  jf_apply_config();
  open_login();
  if (msg && !g_pending_msg) { msg_open(msg, 0); g_pending_msg = 1; }
}

static void play_item(const jf_item *it, int64_t resume) {
  player_start(it, resume);
  g_playing = 1;
}

static void activate_item(screen *s, const jf_item *it) {
  if (!strcmp(it->type, "Series")) open_browse(it, SRC_SEASONS, it->name);
  else if (!strcmp(it->type, "Season")) {
    jf_item p;
    jf_item_copy(&p, it);
    if (!p.series_id[0]) snprintf(p.series_id, sizeof(p.series_id), "%s", s->parent.id);
    if (!p.series_name[0]) snprintf(p.series_name, sizeof(p.series_name), "%s", s->parent.name);
    char t[400];
    snprintf(t, sizeof(t), "%s", p.name);
    open_browse(&p, SRC_EPISODES, t);
    jf_item_clear(&p);
  } else if (!strcmp(it->type, "Audio")) {
    play_item(it, 0);
  } else if (jf_is_playable(it)) {
    open_detail(it);
  } else {
    open_browse(it, SRC_CHILDREN, it->name);
  }
}

/* ------------------------------------------------------------------ */
/* Cell renderers                                                      */
/* ------------------------------------------------------------------ */

static void placeholder(float x, float y, float w, float h, icon_id ic) {
  ui_rrect_vgrad(x, y, w, h, 3, C_RGBA(255, 255, 255, 46), C_RGBA(255, 255, 255, 18));
  ui_icon(ic, x + w / 2, y + h / 2, fminf(w, h) * 0.4f, C_RGBA(255, 255, 255, 110));
}

static void thumb(const jf_item *it, int landscape, float x, float y, float w, float h) {
  char url[512];
  float a = 0;
  vita2d_texture *t = jf_tile_image(it, landscape, url, sizeof(url), (int)w, (int)h) ? images_get(url, &a) : NULL;
  if (!t || a < 1) placeholder(x, y, w, h, ui_icon_for_item(it->type, it->collection_type));
  if (t) ui_tex_cover(t, x, y, w, h, a);
  ui_rrect_outline(x - 1, y - 1, w + 2, h + 2, 1, 1, C_RGBA(0, 0, 0, 70));
}

static void badge_count(float rx, float y, int n) {
  char b[12];
  snprintf(b, sizeof(b), "%d", n);
  float w = fmaxf(28, ui_text_w(16, b) + 14);
  ui_rrect_vgrad(rx - w, y, w, 26, 13, C_RGBA(255, 255, 255, 255), C_RGBA(0xD8, 0xD8, 0xDC, 255));
  ui_text_center(rx - w / 2, y + 13, 16, col_lerp(T->bg_top, C_RGBA(0, 0, 0, 255), 0.3f), b);
}

/* Settings-style row: press/focus highlight + etched separator underneath. */
static void row_frame(float x, float y, float w, float h, int focused, float press) {
  if (press > 0.01f) ui_row_highlight(x - 16, y + 1, w + 32, h - 2, press);
  else if (focused) ui_focus_bar(x - 16, y + 1, w + 32, h - 2);
  ui_row_separator(x, y + h - 1, w);
}

static void item_subtitle(const jf_item *it, char *buf, int len) {
  char dur[32] = "";
  if (it->runtime_ticks > 0) fmt_duration(dur, sizeof(dur), it->runtime_ticks);
  if (!strcmp(it->type, "Episode")) {
    if (it->series_name[0] && it->parent_index_number >= 0)
      snprintf(buf, len, "%s · S%d:E%d%s%s", it->series_name, it->parent_index_number, it->index_number, dur[0] ? " · " : "", dur);
    else snprintf(buf, len, "%s", dur);
  } else if (!strcmp(it->type, "Movie")) {
    if (it->year) snprintf(buf, len, "%d%s%s", it->year, dur[0] ? " · " : "", dur);
    else snprintf(buf, len, "%s", dur);
  } else if (it->is_folder && it->child_count > 0) {
    snprintf(buf, len, "%d items", it->child_count);
  } else {
    snprintf(buf, len, "%s", dur);
  }
}

static void draw_home_row(void *ctx, int idx, float x, float y, float w, float h, int focused, float press) {
  screen *s = ctx;
  row_frame(x, y, w, h, focused, press);
  float ix = x + 6, iw = 112, ih = 63, iy = y + (h - ih) / 2, cy = y + h / 2;
  const char *title, *sub;
  int special = -1;
  if (s->has_resume && idx == 0) special = 0;
  else if (s->has_nextup && idx == s->has_resume) special = 1;
  if (special >= 0) {
    ui_rrect_vgrad(ix, iy, iw, ih, 4, C_RGBA(255, 255, 255, 70), C_RGBA(255, 255, 255, 24));
    ui_rrect_outline(ix, iy, iw, ih, 4, 1, C_RGBA(255, 255, 255, 70));
    ui_icon(special == 0 ? ICON_RESUME : ICON_NEXT, ix + iw / 2, iy + ih / 2, 32, COL_TEXT);
    title = special == 0 ? "Continue Watching" : "Next Up";
    sub = special == 0 ? "Pick up where you left off" : "The next episode of the shows you watch";
  } else {
    const jf_item *it = &s->list.items[idx - s->has_resume - s->has_nextup];
    thumb(it, 0, ix, iy, iw, ih);
    title = it->name;
    const char *ct = it->collection_type;
    sub = !strcmp(ct, "movies") ? "Movies" : !strcmp(ct, "tvshows") ? "TV Shows" : !strcmp(ct, "music") ? "Music" :
          !strcmp(ct, "homevideos") ? "Home Videos & Photos" : !strcmp(ct, "boxsets") ? "Collections" :
          !strcmp(ct, "playlists") ? "Playlists" : !strcmp(ct, "musicvideos") ? "Music Videos" : "Library";
  }
  float tx = ix + iw + 26;
  ui_text_clip_vc(tx, cy - 13, 26, COL_TEXT, title, w - (tx - x) - 50);
  ui_text_clip_vc(tx, cy + 17, 18, COL_TEXT_SUB, sub, w - (tx - x) - 50);
  ui_icon(ICON_CHEVRON, x + w - 18, cy, 30, C_RGBA(255, 255, 255, 190));
}

static void draw_list_row(void *ctx, int idx, float x, float y, float w, float h, int focused, float press) {
  screen *s = ctx;
  const jf_item *it = &s->list.items[idx];
  row_frame(x, y, w, h, focused, press);
  float ih = h - 24, iw = ih * 16 / 9, ix = x + 6, iy = y + 12;
  thumb(it, 1, ix, iy, iw, ih);
  if (it->position_ticks > 0 && it->runtime_ticks > 0)
    ui_progress_bar(ix + 6, iy + ih - 10, iw - 12, 5, (float)it->position_ticks / it->runtime_ticks);
  float tx = ix + iw + 24, cy = y + h / 2;
  char title[256], sub[256];
  if (!strcmp(it->type, "Episode") && it->index_number >= 0 && s->src == SRC_EPISODES)
    snprintf(title, sizeof(title), "%d. %s", it->index_number, it->name);
  else
    snprintf(title, sizeof(title), "%s", it->name);
  item_subtitle(it, sub, sizeof(sub));
  float maxw = w - (tx - x) - 60;
  if (sub[0]) {
    ui_text_clip_vc(tx, cy - 14, 24, COL_TEXT, title, maxw);
    ui_text_clip_vc(tx, cy + 17, 18, COL_TEXT_SUB, sub, maxw);
  } else {
    ui_text_clip_vc(tx, cy, 24, COL_TEXT, title, maxw);
  }
  if (it->played) ui_badge_check(x + w - 22, cy);
  else if (it->is_folder) ui_icon(ICON_CHEVRON, x + w - 18, cy, 30, C_RGBA(255, 255, 255, 190));
}

static void draw_track_row(void *ctx, int idx, float x, float y, float w, float h, int focused, float press) {
  screen *s = ctx;
  const jf_item *it = &s->list.items[idx];
  row_frame(x, y, w, h, focused, press);
  float cy = y + h / 2;
  char num[16] = "", dur[24] = "";
  if (it->index_number > 0) snprintf(num, sizeof(num), "%d", it->index_number);
  ui_text_right(x + 44, cy, 20, COL_TEXT_SUB, num);
  ui_text_clip_vc(x + 70, cy, 23, COL_TEXT, it->name, w - 70 - 110);
  if (it->runtime_ticks > 0) fmt_clock(dur, sizeof(dur), it->runtime_ticks / 10000);
  ui_text_right(x + w - 10, cy, 19, COL_TEXT_SUB, dur);
}

static void draw_tile(void *ctx, int idx, float x, float y, float w, float h, int focused, float press) {
  screen *s = ctx;
  const jf_item *it = &s->list.items[idx];
  float ih = s->layout == LAY_SQUARE ? w : w * 1.5f;
  /* focused / pressed tiles lift slightly, like the system's touch feedback */
  float lift = focused ? 1 : 0;
  float k = 1 + 0.04f * lift - 0.05f * press;
  float dw = w * k, dh = ih * k, dx = x - (dw - w) / 2, dy = y - (dh - ih) / 2;
  ui_shadow(dx, dy, dw, dh, 2, 10 + 4 * lift, 0.45f);
  thumb(it, 0, dx, dy, dw, dh);
  if (press > 0.01f) ui_rrect(dx, dy, dw, dh, 1, C_RGBA(255, 255, 255, (int)(70 * press)));
  if (it->played && !it->is_folder) ui_badge_check(dx + dw - 16, dy + 16);
  else if (it->unplayed_count > 0 && !strcmp(it->type, "Series")) badge_count(dx + dw - 6, dy + 6, it->unplayed_count);
  if (it->position_ticks > 0 && it->runtime_ticks > 0)
    ui_progress_bar(dx + 6, dy + dh - 12, dw - 12, 5, (float)it->position_ticks / it->runtime_ticks);
  if (focused) ui_focus_ring(dx - 4, dy - 4, dw + 8, dh + 8, 4);
  float ly = y + ih + 8;
  float tw = ui_text_w(17, it->name);
  if (tw <= w) ui_text(x + (w - tw) / 2, ly, 17, COL_TEXT, it->name);
  else ui_text_clip(x, ly, 17, COL_TEXT, it->name, w);
  const char *sub = s->layout == LAY_SQUARE ? it->album_artist : NULL;
  char yr[16];
  if (!sub && it->year && !strcmp(it->type, "Movie")) { snprintf(yr, sizeof(yr), "%d", it->year); sub = yr; }
  if (sub && sub[0]) {
    float aw = ui_text_w(15, sub);
    if (aw <= w) ui_text(x + (w - aw) / 2, ly + 22, 15, COL_TEXT_SUB, sub);
    else ui_text_clip(x, ly + 22, 15, COL_TEXT_SUB, sub, w);
  }
}

/* ------------------------------------------------------------------ */
/* Login (laid out like a Settings page: label left, value right)      */
/* ------------------------------------------------------------------ */

typedef struct { screen *s; volatile int done; int rc; char err[200]; pthread_t th; } login_job;
static login_job g_login;

static void *login_thread(void *arg) {
  (void)arg;
  screen *s = g_login.s;
  g_login.rc = jf_login(s->l_server, s->l_user, s->l_pass, g_login.err, sizeof(g_login.err));
  g_login.done = 1;
  return NULL;
}

static void draw_login_row(void *ctx, int idx, float x, float y, float w, float h, int focused, float press) {
  screen *s = ctx;
  static const char *labels[] = { "Server Address", "User Name", "Password" };
  row_frame(x, y, w, h, focused, press);
  float cy = y + h / 2;
  ui_text_vc(x + 36, cy, 27, COL_TEXT, labels[idx]);
  const char *v = idx == 0 ? s->l_server : idx == 1 ? s->l_user : NULL;
  char dots[64] = "";
  if (idx == 2) { int n = strlen(s->l_pass); if (n > 16) n = 16; for (int i = 0; i < n; i++) strcat(dots, "●"); v = dots; }
  int set = v && v[0];
  ui_text_clip_vc(x + 400, cy, 23, set ? COL_TEXT : COL_TEXT_DIM, set ? v : "Not Set", w - 400 - 20);
}

static void login_update(screen *s, int input) {
  static const char *titles[] = { "Server Address", "User Name", "Password" };
  if (s->l_editing) {
    char *dst = s->l_sel == 0 ? s->l_server : s->l_sel == 1 ? s->l_user : s->l_pass;
    int cap = s->l_sel == 0 ? sizeof(s->l_server) : s->l_sel == 1 ? sizeof(s->l_user) : sizeof(s->l_pass);
    char tmp[256];
    int r = ime_poll(tmp, sizeof(tmp));
    if (r != 0) {
      s->l_editing = 0;
      if (r > 0) snprintf(dst, cap, "%s", tmp);
    }
  }
  if (s->l_busy && g_login.done) {
    pthread_join(g_login.th, NULL);
    s->l_busy = 0;
    if (g_login.rc == 0) { clear_stack(); open_home(); return; }
    msg_open(g_login.err, 0);
    sfx_play(SFX_ERROR);
    g_pending_msg = 1;
  }
  int act = scroller_update(&s->sc, input && !s->l_busy && !s->l_editing);
  if (act >= 0) {
    s->l_sel = act;
    const char *cur = act == 0 ? (s->l_server[0] ? s->l_server : "http://") : act == 1 ? s->l_user : "";
    if (ime_open(titles[act], cur, act == 0 ? 250 : 120, act == 0 ? IME_URL : act == 2 ? IME_PASSWORD : IME_TEXT) == 0) s->l_editing = 1;
  }
}

static void login_draw(screen *s, int input) {
  scroller_draw(&s->sc, draw_login_row, s);
  float by = CONTENT_Y + s->sc.pad_top + 3 * s->sc.cell_h + 34 - s->sc.scroll;
  int can = s->l_server[0] && s->l_user[0] && !s->l_busy;
  int tapped = ui_button(s->l_busy ? "Signing In…" : "Sign In", ICON_NONE, SCREEN_W / 2 - 140, by, 280, 54, 0);
  if (s->l_busy) ui_spinner(SCREEN_W / 2 + 175, by + 27, 14, 1);
  ui_text_center(SCREEN_W / 2, by + 88, 17, COL_TEXT_SUB, "Example: http://192.168.1.10:8096    (START also signs in)");
  if (input && can && (tapped || BTN_PRESSED(SCE_CTRL_START))) {
    s->l_busy = 1;
    memset(&g_login, 0, sizeof(g_login));
    g_login.s = s;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_create(&g_login.th, &a, login_thread, NULL);
    pthread_attr_destroy(&a);
  }
}

/* ------------------------------------------------------------------ */
/* Detail                                                              */
/* ------------------------------------------------------------------ */

static int detail_update(screen *s, int input, int *act) {
  if (poll_load(s) < 0) return -1;
  s->detail_appear += ui_dt();
  const jf_item *it = s->have_detail ? &s->detail : &s->parent;
  int resume = it->position_ticks > 0 && !it->played;
  s->nbtn = resume ? 2 : 1;
  if (s->btn_sel >= s->nbtn) s->btn_sel = 0;
  *act = -1;
  if (input) {
    if (BTN_REPEAT(SCE_CTRL_RIGHT) && s->btn_sel < s->nbtn - 1) { s->btn_sel++; g_focus_visible = 1; sfx_play(SFX_MOVE); }
    if (BTN_REPEAT(SCE_CTRL_LEFT) && s->btn_sel > 0) { s->btn_sel--; g_focus_visible = 1; sfx_play(SFX_MOVE); }
    if (BTN_PRESSED(g_in.enter)) { if (g_focus_visible) { *act = s->btn_sel; sfx_play(SFX_SELECT); } else g_focus_visible = 1; }
  }
  return 0;
}

static void detail_draw(screen *s, int input, int *act) {
  const jf_item *it = s->have_detail ? &s->detail : &s->parent;
  int resume = it->position_ticks > 0 && !it->played;

  /* backdrop, blended into the theme */
  char url[512];
  float a = 0;
  vita2d_texture *bd = NULL;
  if (it->backdrop_tag[0]) { jf_image_url(url, sizeof(url), it->id, "Backdrop", it->backdrop_tag, 960, 540); bd = images_get(url, &a); }
  else if (it->parent_backdrop_tag[0]) { jf_image_url(url, sizeof(url), it->parent_backdrop_id, "Backdrop", it->parent_backdrop_tag, 960, 540); bd = images_get(url, &a); }
  if (bd) {
    ui_tex_cover(bd, 0, CONTENT_Y - 20, SCREEN_W, SCREEN_H - CONTENT_Y + 20, a * 0.30f);
    ui_vgrad(0, CONTENT_Y - 20, SCREEN_W, 60, col_alpha(T->bg_top, 0.9f), col_alpha(T->bg_top, 0));
  }

  float t = ease_out(s->detail_appear / 0.4f);
  ui_xform xf = ui_xform_identity();
  xf.ty = (1 - t) * 20;
  xf.alpha = t;
  ui_push_xform(xf);

  int episode = !strcmp(it->type, "Episode");
  float px = 64, py = CONTENT_Y + 10, pw = episode ? 256 : 196, ph = episode ? 144 : 294;
  ui_shadow(px, py, pw, ph, 2, 16, 0.6f);
  thumb(it, episode, px, py, pw, ph);
  if (it->position_ticks > 0 && it->runtime_ticks > 0 && !it->played)
    ui_progress_bar(px + 8, py + ph - 13, pw - 16, 6, (float)it->position_ticks / it->runtime_ticks);

  float tx = px + pw + 36, tw = SCREEN_W - tx - 56, ty = py - 4;
  if (episode) {
    char ep[64] = "";
    if (it->parent_index_number >= 0 && it->index_number >= 0) snprintf(ep, sizeof(ep), "Season %d, Episode %d", it->parent_index_number, it->index_number);
    ui_text_clip(tx, ty, 19, COL_TEXT_SUB, ep, tw);
    ty += 28;
    int tl = ui_text_wrap(tx, ty, 28, COL_TEXT, it->name, tw, 34, 2, 1);
    ty += tl * 34 + 4;
  }

  char meta[200] = "", tmp[48];
  if (it->year) { snprintf(tmp, sizeof(tmp), "%d", it->year); strcat(meta, tmp); }
  if (it->runtime_ticks > 0) { fmt_duration(tmp, sizeof(tmp), it->runtime_ticks); if (meta[0]) strcat(meta, "   ·   "); strcat(meta, tmp); }
  if (it->official_rating[0]) { if (meta[0]) strcat(meta, "   ·   "); strncat(meta, it->official_rating, 16); }
  int star_at = -1;
  if (it->community_rating > 0) { if (meta[0]) strcat(meta, "   ·   "); star_at = strlen(meta); snprintf(tmp, sizeof(tmp), "    %.1f", it->community_rating); strcat(meta, tmp); }
  if (it->played) { if (meta[0]) strcat(meta, "   ·   "); strcat(meta, "Watched"); }
  ui_text_clip(tx, ty, 20, COL_TEXT_SUB, meta, tw);
  if (star_at >= 0) {
    char pre[200];
    snprintf(pre, sizeof(pre), "%.*s", star_at, meta);
    ui_star(tx + ui_text_w(20, pre) + 10, ty + 10, 9, COL_TEXT_SUB);
  }
  ty += 38;

  float bw = 230, bh = 52;
  char rlabel[48];
  for (int i = 0; i < s->nbtn; i++) {
    const char *label;
    icon_id ic;
    if (resume && i == 0) {
      char c[24];
      fmt_clock(c, sizeof(c), it->position_ticks / 10000);
      snprintf(rlabel, sizeof(rlabel), "Resume  %s", c);
      label = rlabel; ic = ICON_PLAY;
    } else if (resume) { label = "From Beginning"; ic = ICON_RESUME; }
    else { label = "Play"; ic = ICON_PLAY; }
    if (ui_button(label, ic, tx + i * (bw + 18), ty, bw, bh, s->btn_sel == i) && input) *act = i;
  }
  ty += bh + 24;

  if (it->overview && it->overview[0]) {
    int lines = (int)((SCREEN_H - 24 - ty) / 27);
    if (lines > 0) ui_text_wrap(tx, ty, 19, C_RGBA(255, 255, 255, 225), it->overview, tw, 27, lines, 1);
  }
  ui_pop_xform();
  if (!s->have_detail) ui_spinner(SCREEN_W - 70, CONTENT_Y + 30, 14, 1);
}

/* ------------------------------------------------------------------ */
/* Browse / home                                                       */
/* ------------------------------------------------------------------ */

static const char *SORT_KEYS[] = { "SortName", "DateCreated", "PremiereDate", "CommunityRating" };
static const char *SORT_LABELS[] = { "Sort by Name", "Sort by Date Added", "Sort by Release Date", "Sort by Rating" };
static char g_theme_label[48];

static int has_sort(screen *s) { return s->kind == SCR_BROWSE && s->src == SRC_CHILDREN && strcmp(s->parent.type, "MusicAlbum"); }

static void build_options(screen *s) {
  options_clear(&s->opt);
  if (s->kind == SCR_HOME) {
    snprintf(g_theme_label, sizeof(g_theme_label), "Color: %s", T->name);
    options_add(&s->opt, "Refresh", 0);
    options_add(&s->opt, g_theme_label, 0);
    options_add(&s->opt, "Sign Out", 0);
  } else if (s->kind == SCR_DETAIL) {
    const jf_item *it = s->have_detail ? &s->detail : &s->parent;
    options_add(&s->opt, it->played ? "Mark as Unwatched" : "Mark as Watched", 0);
    options_add(&s->opt, "Refresh", 0);
  } else {
    if (has_sort(s)) {
      for (int i = 0; i < 4; i++) options_add(&s->opt, SORT_LABELS[i], !strcmp(s->sort, SORT_KEYS[i]));
      options_add(&s->opt, s->desc ? "Order: Descending" : "Order: Ascending", 0);
    }
    options_add(&s->opt, "Refresh", 0);
  }
}

static void reload(screen *s) {
  if (s->th_running) return;
  start_load(s, 0);
}

static void handle_option(screen *s, int i) {
  const char *label = s->opt.labels[i];
  if (!strcmp(label, "Refresh")) { reload(s); return; }
  if (!strcmp(label, "Sign Out")) { msg_open("Sign out of this server?", 1); g_pending_msg = 2; return; }
  if (!strncmp(label, "Color:", 6)) {
    g_cfg.theme = (g_cfg.theme + 1) % NUM_THEMES;
    ui_set_theme(g_cfg.theme);
    config_save();
    return;
  }
  if (!strncmp(label, "Mark as", 7)) {
    jf_item *it = s->have_detail ? &s->detail : &s->parent;
    jf_mark_played(it->id, !it->played);
    it->played = !it->played;
    if (it->played) it->position_ticks = 0;
    for (int k = 0; k < g_depth - 1; k++) g_stack[k]->needs_reload = 1;
    return;
  }
  if (!strncmp(label, "Order:", 6)) { s->desc = !s->desc; s->sc.sel = 0; s->sc.target = s->sc.scroll = 0; scroller_restart_appear(&s->sc); reload(s); return; }
  for (int k = 0; k < 4; k++)
    if (!strcmp(label, SORT_LABELS[k])) {
      snprintf(s->sort, sizeof(s->sort), "%s", SORT_KEYS[k]);
      s->desc = k != 0;
      s->sc.sel = 0; s->sc.target = s->sc.scroll = 0;
      scroller_restart_appear(&s->sc);
      reload(s);
    }
}

static int list_update(screen *s, int input) {
  if (poll_load(s) < 0) return -1;
  if (!s->th_running && s->kind == SCR_BROWSE && s->src == SRC_CHILDREN && s->list.count < s->list.total && s->list.count > 0) {
    float cx, cy;
    scroller_cell_rect(&s->sc, s->list.count - 1, &cx, &cy);
    if (s->sc.sel > s->list.count - 3 * s->sc.cols || cy < SCREEN_H + 600) start_load(s, 1);
  }
  int act = scroller_update(&s->sc, input);
  if (act < 0) return 0;
  if (s->kind == SCR_HOME) {
    int r = s->has_resume, n = s->has_nextup;
    if (r && act == 0) open_browse(NULL, SRC_RESUME, "Continue Watching");
    else if (n && act == r) open_browse(NULL, SRC_NEXTUP, "Next Up");
    else if (act - r - n < s->list.count) {
      const jf_item *v = &s->list.items[act - r - n];
      open_browse(v, SRC_CHILDREN, v->name);
    }
  } else if (act < s->list.count) {
    activate_item(s, &s->list.items[act]);
  }
  return 1;
}

static void list_draw(screen *s) {
  cell_draw_fn fn = s->kind == SCR_HOME ? draw_home_row :
                    s->layout == LAY_LIST ? draw_list_row :
                    s->layout == LAY_TRACKS ? draw_track_row : draw_tile;
  scroller_draw(&s->sc, fn, s);
  float cy = CONTENT_Y + (SCREEN_H - CONTENT_Y) / 2 - 20;
  if (s->th_running && !s->loading_more && !s->list.count) ui_spinner(SCREEN_W / 2, cy, 24, 1);
  else if (s->loaded_once && !s->th_running && s->sc.count == 0) ui_text_center(SCREEN_W / 2, cy, 24, COL_TEXT_SUB, "There are no items.");
}

static void detail_play(screen *s, int act) {
  if (act < 0 || !s->have_detail) return;
  const jf_item *it = &s->detail;
  int resume = it->position_ticks > 0 && !it->played;
  play_item(&s->detail, (resume && act == 0) ? it->position_ticks : 0);
}

/* Draw one page's content (title + body). Input is only processed for the top page. */
static void page_draw(screen *s, int input) {
  g_ui_input = input;
  if (s->emblem != ICON_NONE) ui_watermark(s->emblem);
  switch (s->kind) {
    case SCR_LOGIN: login_draw(s, input); break;
    case SCR_DETAIL: { int act = -1; detail_draw(s, input, &act); detail_play(s, act); break; }
    default: list_draw(s); break;
  }
  ui_page_title(s->title);
  g_ui_input = 1;
}

/* ------------------------------------------------------------------ */
/* Page transforms                                                     */
/* ------------------------------------------------------------------ */

#define GHOST_THETA 1.22f   /* ~70° swung back, as the parent page in Settings */
#define GHOST_ALPHA 0.18f
#define NAV_TIME 0.42f

static ui_xform ghost_xf(float k) {
  /* k = 0 flat in front, 1 = fully swung back to the left */
  ui_xform x = ui_xform_identity();
  x.theta = GHOST_THETA * k;
  x.axis_x = 0;
  x.tx = -36 * k;
  x.alpha = 1 - (1 - GHOST_ALPHA) * k;
  return x;
}

static ui_xform enter_xf(float k) {
  /* k = 0 off to the right, 1 = in place */
  ui_xform x = ui_xform_identity();
  x.tx = (1 - k) * 220;
  x.alpha = k;
  return x;
}

static void draw_stack(int input) {
  screen *cur = top();
  if (!cur) return;
  float e = g_nav.active ? ease_in_out(g_nav.t) : 1;

  /* parent page: swung back as a faint ghost on the left */
  screen *parent = g_depth >= 2 ? g_stack[g_depth - 2] : NULL;
  if (g_nav.active && g_nav.dir > 0) {
    if (parent) { ui_push_xform(ghost_xf(e)); page_draw(parent, 0); ui_pop_xform(); }
    ui_push_xform(enter_xf(e)); page_draw(cur, 0); ui_pop_xform();
  } else if (g_nav.active && g_nav.dir < 0) {
    /* popping: the page that becomes current swings forward; its own parent fades in as ghost */
    if (parent) { ui_xform g = ghost_xf(1); g.alpha *= e; ui_push_xform(g); page_draw(parent, 0); ui_pop_xform(); }
    ui_push_xform(ghost_xf(1 - e)); page_draw(cur, 0); ui_pop_xform();
    if (g_nav.leaving) { ui_push_xform(enter_xf(1 - e)); page_draw(g_nav.leaving, 0); ui_pop_xform(); }
  } else {
    if (parent) { ui_push_xform(ghost_xf(1)); page_draw(parent, 0); ui_pop_xform(); }
    page_draw(cur, input);
  }
}

/* ------------------------------------------------------------------ */

void screens_init(void) {
  ui_set_theme(g_cfg.theme);
  if (g_cfg.token[0] && g_cfg.user_id[0] && g_cfg.server[0]) open_home();
  else open_login();
}

void screens_frame(void) {
  if (g_playing) {
    if (player_frame()) return;
    g_playing = 0;
    for (int k = 0; k < g_depth - 1; k++) g_stack[k]->needs_reload = 1;
    screen *t = top();
    if (t) { t->needs_reload = 0; reload(t); }
  }

  if (g_pending_msg) {
    int r = msg_poll();
    if (r != 0) {
      int which = g_pending_msg;
      g_pending_msg = 0;
      if (which == 2 && r > 0) {
        jf_logout();
        clear_stack();
        open_login();
      }
    }
  }

  if (g_nav.active) {
    g_nav.t += ui_dt() / NAV_TIME;
    if (g_nav.t >= 1) finish_nav();
  }

  screen *s = top();
  if (!s) return;
  int modal = dialog_active() || s->opt.open || g_nav.active;
  int input = !modal;
  int can_back = g_depth > 1;
  int has_opts = s->kind != SCR_LOGIN;

  if (input && can_back && BTN_PRESSED(g_in.back)) { pop(); input = 0; }
  else if (input && has_opts && BTN_PRESSED(SCE_CTRL_TRIANGLE)) { build_options(s); options_open(&s->opt); input = 0; }

  /* logic for the current page */
  s = top();
  int act_unused = -1;
  switch (s->kind) {
    case SCR_LOGIN: login_update(s, input); break;
    case SCR_DETAIL: if (detail_update(s, input, &act_unused) < 0) return; detail_play(s, act_unused); break;
    default: if (list_update(s, input) < 0) return; break;
  }
  if (top() != s) input = 0;  /* navigated this frame: the new page animates in */
  /* keep background pages' async loads flowing (e.g. reload after playback) */
  for (int k = 0; k < g_depth - 1; k++) if (g_stack[k]->th_running && g_stack[k]->done) poll_load(g_stack[k]);

  ui_background();
  draw_stack(input && top() == s);
  if (g_playing) return;
  ui_status_bar();

  s = top();
  if (ui_back_button(can_back) && !g_nav.active && !dialog_active() && !s->opt.open) { pop(); return; }
  if (ui_options_button(s->kind != SCR_LOGIN) && !g_nav.active && !dialog_active()) { build_options(s); options_open(&s->opt); }

  int o = options_update(&s->opt);
  options_draw(&s->opt);
  if (o >= 0) handle_option(s, o);
}
