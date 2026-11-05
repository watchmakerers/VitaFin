#ifndef INPUT_H
#define INPUT_H

#include <psp2/ctrl.h>

typedef struct {
  unsigned held, pressed, released, repeat; /* repeat = pressed + auto-repeat */
  int lx, ly, rx, ry;                       /* analog, centered at 0 (-128..127) */

  /* front touch, in screen pixels (960x544) */
  int touching;        /* finger currently down */
  int touch_down;      /* finger went down this frame */
  int touch_up;        /* finger lifted this frame */
  int tap;             /* lifted without dragging (a tap) */
  int tx, ty;          /* current / last position */
  int sx, sy;          /* where the touch started */
  int dx, dy;          /* movement since last frame */
  int dragging;        /* moved beyond tap slop */
  int consumed;        /* a widget claimed this touch */
  unsigned claim;      /* id of the widget that claimed it (0 = none) */

  unsigned enter, back; /* SCE_CTRL_CROSS / CIRCLE, swapped for Japanese systems */
} input_state;

extern input_state g_in;

void input_init(void);
void input_update(void);

#define BTN_PRESSED(b) (g_in.pressed & (b))
#define BTN_REPEAT(b)  (g_in.repeat & (b))
#define BTN_HELD(b)    (g_in.held & (b))

/* Is the active touch inside rect (start point)? */
int touch_started_in(int x, int y, int w, int h);
int point_in(int px, int py, int x, int y, int w, int h);

#endif
