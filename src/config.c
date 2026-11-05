#include "config.h"
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

app_config g_cfg;

#define CFG_PATH DATA_DIR "/config.txt"

static void set(const char *k, const char *v) {
  if (!strcmp(k, "server")) snprintf(g_cfg.server, sizeof(g_cfg.server), "%s", v);
  else if (!strcmp(k, "username")) snprintf(g_cfg.username, sizeof(g_cfg.username), "%s", v);
  else if (!strcmp(k, "user_id")) snprintf(g_cfg.user_id, sizeof(g_cfg.user_id), "%s", v);
  else if (!strcmp(k, "token")) snprintf(g_cfg.token, sizeof(g_cfg.token), "%s", v);
  else if (!strcmp(k, "device_id")) snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "%s", v);
  else if (!strcmp(k, "server_name")) snprintf(g_cfg.server_name, sizeof(g_cfg.server_name), "%s", v);
  else if (!strcmp(k, "max_bitrate")) g_cfg.max_bitrate = atoi(v);
  else if (!strcmp(k, "theme")) g_cfg.theme = atoi(v);
}

void config_load(void) {
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.max_bitrate = 5000;
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(DATA_DIR, 0777);
  FILE *f = fopen(CFG_PATH, "r");
  if (f) {
    char line[512];
    while (fgets(line, sizeof(line), f)) {
      line[strcspn(line, "\r\n")] = 0;
      char *eq = strchr(line, '=');
      if (!eq) continue;
      *eq = 0;
      set(line, eq + 1);
    }
    fclose(f);
  }
  if (!g_cfg.device_id[0]) {
    SceRtcTick t;
    sceRtcGetCurrentTick(&t);
    srand((unsigned)t.tick);
    snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "vita-%08x%08x", rand(), (unsigned)t.tick);
    config_save();
  }
}

void config_save(void) {
  FILE *f = fopen(CFG_PATH, "w");
  if (!f) return;
  fprintf(f, "server=%s\nusername=%s\nuser_id=%s\ntoken=%s\ndevice_id=%s\nserver_name=%s\nmax_bitrate=%d\ntheme=%d\n",
          g_cfg.server, g_cfg.username, g_cfg.user_id, g_cfg.token, g_cfg.device_id, g_cfg.server_name,
          g_cfg.max_bitrate, g_cfg.theme);
  fclose(f);
}
