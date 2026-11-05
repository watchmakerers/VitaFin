#ifndef PLAYER_H
#define PLAYER_H

#include "jellyfin.h"

void player_sysinit(void);
/* Begin playback of a (fully fetched) item. resume_ticks = 0 to start over. */
void player_start(const jf_item *it, int64_t resume_ticks);
/* Run one frame (input + draw). Returns 0 when the player has closed. */
int player_frame(void);

#endif
