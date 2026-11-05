#include "config.h"
#include "http.h"
#include "images.h"
#include "input.h"
#include "jellyfin.h"
#include "screens.h"
#include "ui.h"
#include "subs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern unsigned long long host_sim_us; extern unsigned host_buttons; extern int host_touch, host_tx, host_ty;
void host_begin_frame(void); void host_save(const char *);
static int sign_in_y = CONTENT_Y + 4 + 3 * 76 + 34 + 27;
typedef struct { int frame; unsigned btn; int touch, tx, ty; const char *shot; } step;
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  http_global_init();
  config_load();
  strcpy(g_cfg.server, "http://127.0.0.1:18790"); strcpy(g_cfg.token, "t"); strcpy(g_cfg.user_id, "u");
  strcpy(g_cfg.username, "nathan"); strcpy(g_cfg.server_name, "Living Room Server");
  g_cfg.theme = argc > 1 ? atoi(argv[1]) : 0;
  int login = argc > 2 && !strcmp(argv[2], "login");
  if (login) g_cfg.token[0] = 0;
  jf_apply_config();
  input_init(); ui_init(); images_init();
  if (argc > 2 && !strcmp(argv[2], "subs_demo")) {
    /* a frame of "video" with subtitles, as the player draws them */
    host_begin_frame(); ui_begin_frame();
    ui_vgrad(0, 0, SCREEN_W, SCREEN_H, C_RGBA(0x50, 0x6a, 0x88, 255), C_RGBA(0xd8, 0xc8, 0xa0, 255));
    ui_circle(700, 200, 120, C_RGBA(255, 250, 230, 255));
    ui_rect(0, 380, SCREEN_W, 164, C_RGBA(0x30, 0x40, 0x28, 255));
    subs_draw("I never thought I'd see the harbour again,\nnot after everything that happened that winter.", SCREEN_H - 36, 1);
    host_save("shots/S1_subs.ppm"); printf("shot shots/S1_subs.ppm\n");
    return 0;
  }
  screens_init();
  step script[] = {
    { 150, 0, 0, 0, 0, "01_home" },
    { 160, SCE_CTRL_DOWN }, { 170, SCE_CTRL_DOWN }, { 200, 0, 0, 0, 0, "02_home_focus" },
    { 205, SCE_CTRL_CROSS }, { 216, 0, 0, 0, 0, "03_push_mid" },
    { 330, 0, 0, 0, 0, "04_movies" },
    { 335, SCE_CTRL_RIGHT }, { 345, SCE_CTRL_DOWN }, { 370, 0, 0, 0, 0, "05_movies_focus" },
    { 375, SCE_CTRL_CROSS }, { 520, 0, 0, 0, 0, "06_detail" },
    { 525, SCE_CTRL_CIRCLE }, { 537, 0, 0, 0, 0, "07_pop_mid" },
    { 600, SCE_CTRL_TRIANGLE }, { 615, SCE_CTRL_DOWN }, { 640, 0, 0, 0, 0, "08_options" },
    { 0 } };
  step login_script[] = {
    { 60, 0, 0, 0, 0, "L1_login" },
    /* finger down on the Sign In button, hold, release */
    { 70, 0, 1, 480, 0 }, { 71, 0, 1, 480, 0 }, { 72, 0, 1, 480, 0 }, { 73, 0, 0, 0, 0 },
    { 200, 0, 0, 0, 0, "L2_after_tap" },
    /* tap the Movies row, then the back corner button */
    { 210, 0, 1, 400, 320 }, { 211, 0, 1, 400, 320 }, { 212, 0, 1, 400, 320 }, { 213, 0, 0, 0, 0 },
    { 300, 0, 0, 0, 0, "L3_tapped_row" },
    { 310, 0, 1, 30, 515 }, { 311, 0, 1, 30, 515 }, { 312, 0, 1, 30, 515 }, { 313, 0, 0, 0, 0 },
    { 380, 0, 0, 0, 0, "L4_tapped_back" },
    { 0 } };
  if (login) memcpy(script, login_script, sizeof(login_script));
  int si = 0;
  for (int f = 0; f <= 650; f++) {
    host_buttons = 0; host_touch = 0;
    while (script[si].frame && script[si].frame == f) {
      host_buttons |= script[si].btn;
      if (script[si].touch) { host_touch = 1; host_tx = script[si].tx; host_ty = script[si].ty ? script[si].ty : sign_in_y; }
      if (script[si].shot) { /* capture after this frame renders */ }
      break;
    }
    host_sim_us += 16667; input_update(); ui_begin_frame(); images_frame(); host_begin_frame();
    screens_frame();
    if (script[si].frame == f) { if (script[si].shot) { char p[128]; snprintf(p, sizeof p, "shots/%s.ppm", script[si].shot); host_save(p); printf("shot %s\n", p); } si++; }
    usleep(16667); if (f % 50 == 0) printf("frame %d\n", f);
  }
  return 0;
}
