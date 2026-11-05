#ifndef IMAGES_H
#define IMAGES_H

#include <vita2d.h>

void images_init(void);
void images_shutdown(void);
/* Per-frame housekeeping (eviction). Call once per frame from the main thread. */
void images_frame(void);
/* Returns the texture if ready, otherwise queues it and returns NULL.
 * *alpha (optional) gets a 0..1 fade-in value. */
vita2d_texture *images_get(const char *url, float *alpha);
/* Drop all queued (not yet started) requests, e.g. when leaving a screen. */
void images_cancel_pending(void);

#endif
