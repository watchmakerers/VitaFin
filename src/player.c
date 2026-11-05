#include "player.h"
#include "images.h"
#include "input.h"
#include "log.h"
#include "media.h"
#include "sfx.h"
#include "subs.h"
#include "http.h"
#include "ui.h"
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979f
#endif

typedef enum { P_IDLE, P_ACTIVE, P_ERROR } pstate;

static struct {
  pstate state;
  jf_item item;
  char session[40];
  char title[256], subtitle[256];
  int audio_only;
  media *m;
  int64_t base_ms;       /* the server starts the stream at StartTimeTicks */
  int64_t duration_ms;
  int paused;
  float overlay;
  float overlay_timer;
  int64_t seek_target;   /* pending debounced seek, -1 none */
  float seek_delay;
  int scrubbing;
  float scrub_frac;
  int64_t last_report_ms;
  int started_reported;
  char error[200];
  float fade_in;
  float play_btn_press;

  /* subtitles */
  int sub_sel;            /* server stream index of the chosen track, -1 = off */
  int burn;               /* stream index burned into the video, -1 = none */
  sub_track subs;
  volatile int sub_ready;
  int sub_loading;
  pthread_t sub_th;
  sub_track sub_new;
  int sub_gen;            /* bumps when the selection changes; stale downloads are dropped */
  volatile int sub_new_gen;
  float sub_y;
  options_menu opt;
  int opt_map[OPT_MAX];   /* menu row -> stream index (-1 = off) */
  char opt_labels[OPT_MAX][80];
  char sub_note[128];
  int audio_sel;          /* server stream index of the chosen audio track, -1 = default */
  int menu;               /* which level of the options menu is showing */
  float sub_note_t;
} P;

/* ---------------- subtitles ---------------- */

typedef struct { char url[1024]; int gen; } sub_job;

static void *sub_thread(void *arg) {
  sub_job *j = arg;
  http_resp r;
  sub_track t;
  memset(&t, 0, sizeof(t));
  if (http_request("GET", j->url, NULL, &r) == 0 && r.status == 200) subs_parse_srt(r.data, r.size, &t);
  else applog("subtitles: download failed (HTTP %ld %s)", r.status, r.error);
  http_resp_free(&r);
  applog("subtitles: %d cues", t.n);
  P.sub_new = t;
  P.sub_new_gen = j->gen;
  P.sub_ready = 1;
  free(j);
  return NULL;
}

static const jf_audio *find_audio(int index);

static const jf_sub *find_sub(int index) {
  for (int i = 0; i < P.item.nsubs; i++) if (P.item.subs[i].index == index) return &P.item.subs[i];
  return NULL;
}

static void fetch_text_sub(int index) {
  if (P.sub_loading) { pthread_join(P.sub_th, NULL); P.sub_loading = 0; if (P.sub_ready) { subs_free(&P.sub_new); P.sub_ready = 0; } }
  sub_job *j = calloc(1, sizeof(*j));
  jf_subtitle_url(&P.item, index, j->url, sizeof(j->url));
  j->gen = P.sub_gen;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 * 1024);
  pthread_create(&P.sub_th, &a, sub_thread, j);
  pthread_attr_destroy(&a);
  P.sub_loading = 1;
}

static void poll_subs(void) {
  if (!P.sub_loading || !P.sub_ready) return;
  pthread_join(P.sub_th, NULL);
  P.sub_loading = 0;
  P.sub_ready = 0;
  if (P.sub_new_gen == P.sub_gen) {
    subs_free(&P.subs);
    P.subs = P.sub_new;
    if (!P.subs.n) { snprintf(P.sub_note, sizeof(P.sub_note), "Subtitles could not be loaded."); P.sub_note_t = 3; }
  } else {
    subs_free(&P.sub_new);
  }
  memset(&P.sub_new, 0, sizeof(P.sub_new));
}

/* ---------------- progress reporting (never blocks the UI) ---------------- */

typedef struct { char what[12]; jf_item item; int64_t pos; int paused; char session[40]; } report_job;

static void *report_thread(void *arg) {
  report_job *j = arg;
  jf_report(j->what, &j->item, j->pos, j->paused, j->session, 1);
  free(j);
  return NULL;
}

static void report(const char *what, int64_t pos_ms) {
  report_job *j = calloc(1, sizeof(*j));
  snprintf(j->what, sizeof(j->what), "%s", what);
  j->item = P.item;
  j->item.overview = NULL;
  j->pos = pos_ms * 10000LL;
  j->paused = P.paused;
  snprintf(j->session, sizeof(j->session), "%s", P.session);
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 128 * 1024);
  pthread_create(&t, &a, report_thread, j);
  pthread_attr_destroy(&a);
  pthread_detach(t);
}

/* ---------------- lifecycle ---------------- */

void player_sysinit(void) { media_sysinit(); }

static int64_t pos_ms(void) {
  if (!P.m) return P.base_ms;
  return P.base_ms + media_clock_ms(P.m);
}

/* Jellyfin names a progressive transcode's temp file after (media, user agent, device,
 * PlaySessionId) — not the start position — and serves an existing file from its
 * beginning. So every stream open, including each seek, needs a new session id,
 * or the server hands back the old transcode and the seek/resume is ignored. */
static void new_session(void) {
  uint64_t t = sceKernelGetProcessTimeWide();
  static unsigned counter;
  unsigned a = (unsigned)rand() ^ (unsigned)t, b = (unsigned)rand() ^ (unsigned)(t >> 32) ^ (++counter * 2654435761u), c = (unsigned)rand();
  snprintf(P.session, sizeof(P.session), "%08x%08x%08x", a, b, c);
}

typedef struct { char session[40]; } kill_job;

static void *kill_thread(void *arg) {
  kill_job *k = arg;
  jf_kill_encoding(k->session);
  free(k);
  return NULL;
}

/* Tell the server to drop the previous transcode (frees its CPU right away). */
static void kill_old_session(void) {
  if (!P.session[0]) return;
  kill_job *k = calloc(1, sizeof(*k));
  snprintf(k->session, sizeof(k->session), "%s", P.session);
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 128 * 1024);
  pthread_create(&t, &a, kill_thread, k);
  pthread_attr_destroy(&a);
  pthread_detach(t);
}

static void open_at(int64_t start_ms) {
  char url[1536];
  kill_old_session();
  new_session();
  jf_stream_url(&P.item, start_ms * 10000LL, url, sizeof(url), P.session, P.audio_sel, P.burn);
  applog("play '%s' at %lld ms (session %s)", P.item.name, (long long)start_ms, P.session);
  P.base_ms = start_ms;
  P.m = media_open(url, P.audio_only);
  if (P.paused) media_set_paused(P.m, 1);
  P.state = P_ACTIVE;
}

void player_start(const jf_item *it, int64_t resume_ticks) {
  memset(&P, 0, sizeof(P));
  jf_item_copy(&P.item, it);
  P.audio_only = !strcmp(it->type, "Audio");
  if (!strcmp(it->type, "Episode")) {
    snprintf(P.title, sizeof(P.title), "%s", it->series_name);
    if (it->parent_index_number >= 0 && it->index_number >= 0)
      snprintf(P.subtitle, sizeof(P.subtitle), "S%d:E%d  %s", it->parent_index_number, it->index_number, it->name);
    else snprintf(P.subtitle, sizeof(P.subtitle), "%s", it->name);
  } else if (P.audio_only) {
    snprintf(P.title, sizeof(P.title), "%s", it->name);
    snprintf(P.subtitle, sizeof(P.subtitle), "%s", it->album_artist);
  } else {
    snprintf(P.title, sizeof(P.title), "%s", it->name);
  }
  P.duration_ms = it->runtime_ticks / 10000;
  P.seek_target = -1;
  P.sub_y = SCREEN_H - 40;

  /* Subtitle default: a forced track, else the server's pick (it follows the user's
   * language / subtitle-mode preferences), else off. */
  P.sub_sel = -1;
  P.burn = -1;
  /* audio default: the server's pick (user language preference), else the file's default */
  P.audio_sel = it->default_audio;
  if (P.audio_sel < 0 || !find_audio(P.audio_sel))
    for (int i = 0; i < it->nauds; i++) if (it->auds[i].is_default) { P.audio_sel = it->auds[i].index; break; }
  if (P.audio_sel < 0 && it->nauds) P.audio_sel = it->auds[0].index;
  for (int i = 0; i < it->nsubs; i++) if (it->subs[i].is_forced) { P.sub_sel = it->subs[i].index; break; }
  if (P.sub_sel < 0 && it->default_sub >= 0 && find_sub(it->default_sub)) P.sub_sel = it->default_sub;
  if (P.sub_sel >= 0) {
    const jf_sub *sb = find_sub(P.sub_sel);
    if (sb && sb->is_text) fetch_text_sub(P.sub_sel);
    else if (sb) P.burn = P.sub_sel;
  }
  P.overlay = 1;
  P.overlay_timer = 4;
  open_at(resume_ticks / 10000);
}

static void close_player(void) {
  sfx_play(SFX_BACK);
  kill_old_session();
  if (P.sub_loading) { pthread_join(P.sub_th, NULL); P.sub_loading = 0; if (P.sub_ready) subs_free(&P.sub_new); }
  subs_free(&P.subs);
  if (P.m) {
    report("stop", pos_ms());
    media_close(P.m);
    P.m = NULL;
  }
  jf_item_clear(&P.item);
  P.state = P_IDLE;
}

static void seek_to(int64_t ms) {
  if (P.duration_ms && ms > P.duration_ms - 3000) ms = P.duration_ms - 3000;
  if (ms < 0) ms = 0;
  /* The server restarts the transcode at the new point; debounce repeated presses. */
  sfx_play(SFX_MOVE);
  P.seek_target = ms;
  P.seek_delay = 0.55f;
}

static void toggle_pause(void) {
  sfx_play(SFX_SELECT);
  P.paused = !P.paused;
  if (P.m) media_set_paused(P.m, P.paused);
  report("progress", pos_ms());
}

/* ---------------- drawing ---------------- */

#define BAR_X 150
#define BAR_W 660
#define BAR_Y 452

static void draw_video(void) {
  if (!P.m) return;
  int w = 0, h = 0;
  vita2d_texture *t = media_video_frame(P.m, &w, &h);
  if (!t || w <= 0 || h <= 0) return;
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  float ar = (float)w / h;
  float dw = SCREEN_W, dh = dw / ar;
  if (dh > SCREEN_H) { dh = SCREEN_H; dw = dh * ar; }
  ui_tex(t, (SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2, dw, dh, 0, 0, w / tw, h / th, C_RGBA(255, 255, 255, 255));
}

static void round_button(float cx, float cy, float r, icon_id ic, float icon_size, float a, float press) {
  float k = 1 - 0.07f * press;
  ui_circle_radial(cx, cy + 3, r * k + 10, col_alpha(C_RGBA(0, 0, 0, 90), a), C_RGBA(0, 0, 0, 0));
  ui_circle_vgrad(cx, cy, r * k, col_alpha(C_RGBA(255, 255, 255, 70 + (int)(60 * press)), a), col_alpha(C_RGBA(255, 255, 255, 25 + (int)(40 * press)), a));
  ui_arc(cx, cy, r * k - 1, r * 0.42f, M_PI * 1.08f, M_PI * 1.92f, col_alpha(C_RGBA(255, 255, 255, 30), a));
  ui_ring(cx, cy, r * k, 1.5f, col_alpha(C_RGBA(255, 255, 255, 110), a));
  if (ic != ICON_NONE) ui_icon(ic, cx + (ic == ICON_PLAY ? icon_size * 0.08f : 0), cy, icon_size * k, col_alpha(COL_TEXT, a));
}

static void skip_icon(float cx, float cy, int dir, uint32_t c) {
  for (int k = 0; k < 2; k++) {
    float o = (k ? 8 : -8) * dir;
    if (dir > 0) ui_tri(cx + o - 8, cy - 10, cx + o + 8, cy, cx + o - 8, cy + 10, c);
    else ui_tri(cx + o + 8, cy - 10, cx + o - 8, cy, cx + o + 8, cy + 10, c);
  }
}

static void draw_overlay(int64_t pos) {
  float a = P.overlay;
  if (a < 0.01f) return;
  ui_vgrad(0, 0, SCREEN_W, 130, col_alpha(C_RGBA(0, 0, 0, 210), a), C_RGBA(0, 0, 0, 0));
  ui_vgrad(0, SCREEN_H - 190, SCREEN_W, 190, C_RGBA(0, 0, 0, 0), col_alpha(C_RGBA(0, 0, 0, 225), a));

  ui_text_clip_vc(30, 52, 28, col_alpha(COL_TEXT, a), P.title, SCREEN_W - 60);
  if (P.subtitle[0]) ui_text_clip_vc(30, 86, 20, col_alpha(COL_TEXT_SUB, a), P.subtitle, SCREEN_W - 60);

  float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f;
  round_button(cx, cy, 48, P.paused ? ICON_PLAY : ICON_PAUSE, 40, a, P.play_btn_press);
  for (int d = -1; d <= 1; d += 2) {
    float bx = cx + d * 160;
    round_button(bx, cy, 34, ICON_NONE, 0, a, 0);
    skip_icon(bx, cy, d, col_alpha(COL_TEXT, a));
  }

  int64_t show = P.scrubbing ? (int64_t)(P.scrub_frac * P.duration_ms) : (P.seek_target >= 0 ? P.seek_target : pos);
  float frac = P.duration_ms > 0 ? (float)show / P.duration_ms : 0;
  frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
  ui_rrect(BAR_X, BAR_Y - 3, BAR_W, 6, 3, col_alpha(C_RGBA(255, 255, 255, 70), a));
  ui_rrect_vgrad(BAR_X, BAR_Y - 3, fmaxf(6, BAR_W * frac), 6, 3, col_alpha(C_RGBA(255, 255, 255, 255), a), col_alpha(T->accent, a));
  float kx = BAR_X + BAR_W * frac, kr = P.scrubbing ? 14 : 11;
  ui_circle_radial(kx, BAR_Y + 2, kr + 6, col_alpha(C_RGBA(0, 0, 0, 110), a), C_RGBA(0, 0, 0, 0));
  ui_circle_vgrad(kx, BAR_Y, kr, col_alpha(C_RGBA(255, 255, 255, 255), a), col_alpha(C_RGBA(0xC8, 0xCA, 0xD0, 255), a));
  char t1[24], t2[24];
  fmt_clock(t1, sizeof(t1), show);
  fmt_clock(t2, sizeof(t2), P.duration_ms);
  ui_text_right(BAR_X - 18, BAR_Y, 19, col_alpha(COL_TEXT, a), t1);
  ui_text_vc(BAR_X + BAR_W + 18, BAR_Y, 19, col_alpha(COL_TEXT_SUB, a), t2);
}

static void restart_here(void) {
  int64_t pos = pos_ms();
  if (P.m) { media_close(P.m); P.m = NULL; }
  P.started_reported = 0;
  open_at(pos);
}

static void select_sub(int index) {
  int prev_burn = P.burn;
  P.sub_gen++;
  subs_free(&P.subs);
  P.sub_sel = index;
  const jf_sub *sb = index >= 0 ? find_sub(index) : NULL;
  if (!sb) { P.sub_sel = -1; P.burn = -1; snprintf(P.sub_note, sizeof(P.sub_note), "Subtitles: Off"); }
  else if (sb->is_text) { P.burn = -1; fetch_text_sub(index); snprintf(P.sub_note, sizeof(P.sub_note), "Subtitles: %s", sb->title); }
  else { P.burn = index; snprintf(P.sub_note, sizeof(P.sub_note), "Subtitles: %s (rendered by the server)", sb->title); }
  P.sub_note_t = 2.5f;
  /* picture subtitles live in the video itself, so switching them restarts the stream */
  if (P.burn != prev_burn) restart_here();
}

static const jf_audio *find_audio(int index) {
  for (int i = 0; i < P.item.nauds; i++) if (P.item.auds[i].index == index) return &P.item.auds[i];
  return NULL;
}

static void select_audio(int index) {
  const jf_audio *au = find_audio(index);
  if (!au || index == P.audio_sel) return;
  P.audio_sel = index;
  snprintf(P.sub_note, sizeof(P.sub_note), "Audio: %s", au->title);
  P.sub_note_t = 2.5f;
  /* the server builds the stream with the chosen track, so restart where we are */
  restart_here();
}

enum { MENU_ROOT, MENU_AUDIO, MENU_SUBS };

static void open_menu(int which) {
  options_clear(&P.opt);
  P.menu = which;
  int n = 0;
  if (which == MENU_ROOT) {
    const jf_audio *au = find_audio(P.audio_sel);
    const jf_sub *sb = P.sub_sel >= 0 ? find_sub(P.sub_sel) : NULL;
    snprintf(P.opt_labels[n], sizeof(P.opt_labels[n]), "Audio: %s", au ? au->title : "Default");
    P.opt_map[n] = MENU_AUDIO;
    options_add(&P.opt, P.opt_labels[n++], 0);
    snprintf(P.opt_labels[n], sizeof(P.opt_labels[n]), "Subtitles: %s", sb ? sb->title : "Off");
    P.opt_map[n] = MENU_SUBS;
    options_add(&P.opt, P.opt_labels[n++], 0);
  } else if (which == MENU_AUDIO) {
    for (int i = 0; i < P.item.nauds && n < OPT_MAX; i++, n++) {
      const jf_audio *au = &P.item.auds[i];
      snprintf(P.opt_labels[n], sizeof(P.opt_labels[n]), "%s", au->title);
      P.opt_map[n] = au->index;
      options_add(&P.opt, P.opt_labels[n], au->index == P.audio_sel);
    }
    if (n == 0) {  /* nothing listed: a single "Default" entry keeps the menu valid */
      snprintf(P.opt_labels[n], sizeof(P.opt_labels[n]), "Default");
      P.opt_map[n] = -1;
      options_add(&P.opt, P.opt_labels[n++], 1);
    }
  } else {
    snprintf(P.opt_labels[n], sizeof(P.opt_labels[n]), "Subtitles Off");
    P.opt_map[n] = -1;
    options_add(&P.opt, P.opt_labels[n], P.sub_sel < 0);
    n++;
    for (int i = 0; i < P.item.nsubs && n < OPT_MAX; i++, n++) {
      const jf_sub *sb = &P.item.subs[i];
      snprintf(P.opt_labels[n], sizeof(P.opt_labels[n]), "%s%s", sb->title, sb->is_text ? "" : " (image)");
      P.opt_map[n] = sb->index;
      options_add(&P.opt, P.opt_labels[n], sb->index == P.sub_sel);
    }
  }
  options_open(&P.opt);
  for (int i = 0; i < P.opt.count; i++) if (P.opt.checked[i]) P.opt.sel = i;
}

static void menu_choose(int row) {
  int v = P.opt_map[row];
  if (P.menu == MENU_ROOT) open_menu(v);  /* into the Audio or Subtitles list */
  else if (P.menu == MENU_AUDIO) select_audio(v);
  else select_sub(v);
}

int player_frame(void) {
  if (P.state == P_IDLE) return 0;
  sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
  sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
  sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
  float dt = ui_dt();
  P.fade_in = fminf(1, P.fade_in + dt * 4);

  int st = P.m ? media_state(P.m) : MEDIA_ERROR;
  if (P.state == P_ACTIVE && st == MEDIA_ERROR) {
    snprintf(P.error, sizeof(P.error), "%s", P.m && media_error(P.m)[0] ? media_error(P.m) : "This video cannot be played.");
    P.state = P_ERROR;
  }
  if (P.state == P_ACTIVE && st == MEDIA_ENDED) { close_player(); return 0; }
  if (P.state == P_ACTIVE && st == MEDIA_PLAYING && !P.started_reported) {
    P.started_reported = 1;
    report("start", pos_ms());
  }

  int64_t pos = pos_ms();
  poll_subs();
  if (P.sub_note_t > 0) P.sub_note_t -= dt;

  /* --- input --- */
  int any = g_in.pressed || g_in.touch_down;
  int menu_open = P.opt.open;
  if (menu_open) {
    int o = options_update(&P.opt);
    if (o >= 0) menu_choose(o);
    /* backing out of a sub-list returns to the root menu, like the system */
    else if (o == -2 && P.menu != MENU_ROOT && (BTN_PRESSED(g_in.back) || BTN_PRESSED(SCE_CTRL_TRIANGLE))) open_menu(MENU_ROOT);
    any = 1;
  } else if (P.state == P_ERROR) {
    if (BTN_PRESSED(g_in.back) || BTN_PRESSED(g_in.enter) || g_in.tap) { close_player(); return 0; }
  } else {
    if (BTN_PRESSED(g_in.back)) { close_player(); return 0; }
    if (BTN_PRESSED(SCE_CTRL_TRIANGLE) && !P.audio_only) open_menu(MENU_ROOT);
    if (BTN_PRESSED(g_in.enter) || BTN_PRESSED(SCE_CTRL_START)) toggle_pause();
    int64_t base = P.seek_target >= 0 ? P.seek_target : pos;
    if (BTN_REPEAT(SCE_CTRL_RIGHT)) seek_to(base + 10000);
    if (BTN_REPEAT(SCE_CTRL_LEFT)) seek_to(base - 10000);
    if (BTN_REPEAT(SCE_CTRL_RTRIGGER)) seek_to(base + 60000);
    if (BTN_REPEAT(SCE_CTRL_LTRIGGER)) seek_to(base - 60000);

    float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f;
    int vis = P.overlay > 0.5f;
    int on_bar = vis && point_in(g_in.sx, g_in.sy, BAR_X - 40, BAR_Y - 34, BAR_W + 80, 68);
    int on_play = vis && hypotf(g_in.sx - cx, g_in.sy - cy) < 60;
    P.play_btn_press = approach(P.play_btn_press, g_in.touching && on_play && !g_in.dragging ? 1.0f : 0.0f, 24);
    if (g_in.touching && on_bar && P.duration_ms > 0) {
      P.scrubbing = 1;
      P.scrub_frac = fminf(1, fmaxf(0, (g_in.tx - BAR_X) / (float)BAR_W));
      P.overlay_timer = 4;
    } else if (P.scrubbing && !g_in.touching) {
      P.scrubbing = 0;
      seek_to((int64_t)(P.scrub_frac * P.duration_ms));
    } else if (g_in.tap) {
      if (vis && point_in(g_in.sx, g_in.sy, 0, SCREEN_H - 100, 110, 100)) { close_player(); return 0; }
      if (vis && !P.audio_only && point_in(g_in.sx, g_in.sy, SCREEN_W - 110, SCREEN_H - 100, 110, 100)) { sfx_play(SFX_MENU); open_menu(MENU_ROOT); }
      else
      if (on_play) toggle_pause();
      else if (vis && hypotf(g_in.sx - (cx - 160), g_in.sy - cy) < 46) seek_to(base - 10000);
      else if (vis && hypotf(g_in.sx - (cx + 160), g_in.sy - cy) < 46) seek_to(base + 10000);
      else if (vis) { P.overlay_timer = 0; any = 0; }
    }
  }
  if (any) P.overlay_timer = 4;

  if (P.seek_target >= 0 && (P.seek_delay -= dt) <= 0) {
    int64_t t = P.seek_target;
    P.seek_target = -1;
    if (P.m) { media_close(P.m); P.m = NULL; }
    P.started_reported = 0;
    open_at(t);
    pos = t;
  }

  if (st == MEDIA_PLAYING && !P.paused && llabs(pos - P.last_report_ms) > 10000) {
    P.last_report_ms = pos;
    report("progress", pos);
  }

  int keep = P.paused || st != MEDIA_PLAYING || P.scrubbing || P.seek_target >= 0 || P.state == P_ERROR || P.opt.open;
  if (P.overlay_timer > 0 && !keep) P.overlay_timer -= dt;
  P.overlay = approach(P.overlay, (P.overlay_timer > 0 || keep || P.audio_only) ? 1.0f : 0.0f, 10);

  /* --- draw --- */
  ui_rect(0, 0, SCREEN_W, SCREEN_H, C_RGBA(0, 0, 0, 255));
  if (P.audio_only) {
    ui_background();
    char url[512];
    float al = 0;
    vita2d_texture *art = jf_tile_image(&P.item, 0, url, sizeof(url), 320, 320) ? images_get(url, &al) : NULL;
    float ax = SCREEN_W / 2.0f - 140, ay = 120;
    ui_shadow(ax, ay, 280, 280, 4, 22, 0.6f);
    if (!art || al < 1) { ui_rrect(ax, ay, 280, 280, 4, C_RGBA(0, 0, 0, 90)); ui_icon(ICON_MUSIC, ax + 140, ay + 140, 100, COL_TEXT_DIM); }
    if (art) ui_tex_cover(art, ax, ay, 280, 280, al);
  } else {
    draw_video();
  }

  if (P.state == P_ERROR) {
    ui_rect(0, 0, SCREEN_W, SCREEN_H, C_RGBA(0, 0, 0, 160));
    ui_text_center(SCREEN_W / 2.0f, SCREEN_H / 2.0f - 18, 24, COL_TEXT, P.error);
    ui_text_center(SCREEN_W / 2.0f, SCREEN_H / 2.0f + 22, 18, COL_TEXT_SUB, "Details were written to ux0:data/VitaFin/log.txt");
    ui_back_button(1);
    return 1;
  }

  /* subtitles sit above the controls while they're showing */
  if (P.subs.n && P.sub_sel >= 0 && P.burn < 0) {
    char sbuf[1024];
    float target = P.overlay > 0.05f ? BAR_Y - 52 - 26 * P.overlay : SCREEN_H - 36;
    P.sub_y = approach(P.sub_y, target, 12);
    subs_draw(subs_at(&P.subs, pos, sbuf, sizeof(sbuf)), P.sub_y, 1);
  }
  draw_overlay(pos);
  int waiting = st == MEDIA_BUFFERING || P.seek_target >= 0;
  if (waiting && P.seek_target < 0) {
    if (P.overlay < 0.5f) ui_circle_radial(SCREEN_W / 2.0f, SCREEN_H / 2.0f, 60, C_RGBA(0, 0, 0, 150), C_RGBA(0, 0, 0, 0));
    ui_spinner(SCREEN_W / 2.0f, SCREEN_H / 2.0f + (P.overlay > 0.5f ? 90 : 0), 24, 1);
  }
  if (P.overlay > 0.01f) {
    ui_xform x = ui_xform_identity();
    x.alpha = P.overlay;
    ui_push_xform(x);
    g_ui_input = 0;  /* taps on the corners are handled above */
    ui_back_button(1);
    ui_options_button(!P.audio_only);
    g_ui_input = 1;
    ui_pop_xform();
  }
  if (P.sub_note_t > 0) {
    float na = fminf(1, P.sub_note_t * 3);
    float w = ui_text_w(19, P.sub_note) + 40;
    ui_rrect((SCREEN_W - w) / 2, 112, w, 40, 20, col_alpha(C_RGBA(0, 0, 0, 170), na));
    ui_text_center(SCREEN_W / 2.0f, 132, 19, col_alpha(COL_TEXT, na), P.sub_note);
  }
  if (P.sub_loading && P.sub_sel >= 0) ui_spinner(SCREEN_W - 40, 40, 10, 0.8f);
  options_draw(&P.opt);
  if (P.fade_in < 1) ui_rect(0, 0, SCREEN_W, SCREEN_H, C_RGBA(0, 0, 0, (int)(255 * (1 - P.fade_in))));
  return 1;
}
