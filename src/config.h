#ifndef CONFIG_H
#define CONFIG_H

#define DATA_DIR "ux0:data/VitaFin"

typedef struct {
  char server[256];
  char username[128];
  char user_id[64];
  char token[128];
  char device_id[40];
  char server_name[128];
  int max_bitrate;   /* kbps for transcodes */
  int theme;
} app_config;

extern app_config g_cfg;

void config_load(void);
void config_save(void);

#endif
