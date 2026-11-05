#include "input.h"
#include <psp2/touch.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

input_state g_in;

static unsigned g_hold_frames[32];
#define REPEAT_DELAY 24
#define REPEAT_RATE  5
#define TAP_SLOP     14

void input_init(void) {
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceTouchEnableTouchForce(SCE_TOUCH_PORT_FRONT);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
  memset(&g_in, 0, sizeof(g_in));

  int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
  g_in.enter = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
  g_in.back = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
}

void input_update(void) {
  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  sceCtrlPeekBufferPositive(0, &pad, 1);

  unsigned held = pad.buttons;
  /* Treat the left stick as a d-pad for menu navigation. */
  int lx = (int)pad.lx - 128, ly = (int)pad.ly - 128;
  if (ly < -80) held |= SCE_CTRL_UP;
  if (ly > 80) held |= SCE_CTRL_DOWN;
  if (lx < -80) held |= SCE_CTRL_LEFT;
  if (lx > 80) held |= SCE_CTRL_RIGHT;

  g_in.pressed = held & ~g_in.held;
  g_in.released = ~held & g_in.held;
  g_in.held = held;
  g_in.repeat = g_in.pressed;
  for (int b = 0; b < 32; b++) {
    if (held & (1u << b)) {
      g_hold_frames[b]++;
      if (g_hold_frames[b] > REPEAT_DELAY && (g_hold_frames[b] - REPEAT_DELAY) % REPEAT_RATE == 0) g_in.repeat |= 1u << b;
    } else {
      g_hold_frames[b] = 0;
    }
  }
  g_in.lx = lx; g_in.ly = ly;
  g_in.rx = (int)pad.rx - 128; g_in.ry = (int)pad.ry - 128;

  SceTouchData t;
  memset(&t, 0, sizeof(t));
  sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
  int was = g_in.touching;
  g_in.touch_down = g_in.touch_up = g_in.tap = 0;
  g_in.dx = g_in.dy = 0;
  if (t.reportNum > 0) {
    /* Front panel reports 1920x1088 coordinates. */
    int x = t.report[0].x / 2, y = t.report[0].y / 2;
    if (!was) {
      static int logged;
      if (logged++ < 5) applog("touch down at %d,%d (raw %d,%d)", x, y, t.report[0].x, t.report[0].y);
      g_in.touch_down = 1;
      g_in.sx = x; g_in.sy = y;
      g_in.dragging = 0;
      g_in.consumed = 0;
      g_in.claim = 0;
    } else {
      g_in.dx = x - g_in.tx;
      g_in.dy = y - g_in.ty;
    }
    g_in.tx = x; g_in.ty = y;
    g_in.touching = 1;
    if (abs(x - g_in.sx) > TAP_SLOP || abs(y - g_in.sy) > TAP_SLOP) g_in.dragging = 1;
  } else {
    if (was) {
      g_in.touch_up = 1;
      g_in.tap = !g_in.dragging;
    }
    g_in.touching = 0;
  }
}

int point_in(int px, int py, int x, int y, int w, int h) {
  return px >= x && px < x + w && py >= y && py < y + h;
}

int touch_started_in(int x, int y, int w, int h) {
  return point_in(g_in.sx, g_in.sy, x, y, w, h);
}
