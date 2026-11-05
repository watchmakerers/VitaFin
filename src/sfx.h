#ifndef SFX_H
#define SFX_H

/* Short synthesised UI sounds in the spirit of the system's: soft ticks and pops.
 * Mixed on their own audio port so they work over video playback too. */
typedef enum {
  SFX_MOVE,      /* focus moves */
  SFX_SELECT,    /* tap / confirm */
  SFX_BACK,      /* go back */
  SFX_OPEN,      /* page swings in */
  SFX_MENU,      /* options menu opens */
  SFX_ERROR,
  SFX_COUNT
} sfx_id;

void sfx_init(void);
void sfx_play(sfx_id id);

#endif
