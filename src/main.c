#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "http.h"
#include "images.h"
#include "input.h"
#include "jellyfin.h"
#include "log.h"
#include "sfx.h"
#include "player.h"
#include "screens.h"
#include "ui.h"

/* Give newlib a big heap: thumbnails, curl/openssl and AvPlayer all allocate from it. */
int _newlib_heap_size_user = 128 * 1024 * 1024;

#define NET_POOL (1 * 1024 * 1024)

static void net_init(void) {
  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  SceNetInitParam p;
  static char pool[NET_POOL] __attribute__((aligned(64)));
  p.memory = pool;
  p.size = NET_POOL;
  p.flags = 0;
  sceNetInit(&p);
  sceNetCtlInit();
}

int main(void) {
  /* Run the CPU/GPU at full speed: JPEG decoding and the UI benefit. */
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  SceAppUtilInitParam ip;
  SceAppUtilBootParam bp;
  memset(&ip, 0, sizeof(ip));
  memset(&bp, 0, sizeof(bp));
  sceAppUtilInit(&ip, &bp);

  net_init();
  http_global_init();
  player_sysinit();

  /* 4x MSAA: smooth edges on all the vector chrome (circles, arcs, rounded rects). */
  if (vita2d_init_advanced_with_msaa(4 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X) < 0) vita2d_init_advanced(4 * 1024 * 1024);
  vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
  vita2d_set_vblank_wait(1);

  config_load();
  srand((unsigned)sceKernelGetProcessTimeWide());
  log_init();
  applog("VitaFin starting");
  input_init();
  ui_init();
  sfx_init();
  jf_apply_config();
  images_init();
  screens_init();

  for (;;) {
    input_update();
    ui_begin_frame();
    images_frame();

    vita2d_start_drawing();
    vita2d_clear_screen();
    screens_frame();
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
  }

  images_shutdown();
  ui_shutdown();
  vita2d_fini();
  http_global_cleanup();
  sceNetCtlTerm();
  sceNetTerm();
  sceKernelExitProcess(0);
  return 0;
}
