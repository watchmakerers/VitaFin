#ifndef MEDIA_H
#define MEDIA_H

#include <vita2d.h>
#include <stdint.h>

/* Streaming playback engine: HTTP -> MPEG-TS (or raw ADTS) demux ->
 * hardware H.264 (sceAvcdec) + hardware AAC (sceAudiodec) -> screen/speaker.
 * Audio is the master clock. */

typedef struct media media;

enum { MEDIA_BUFFERING = 0, MEDIA_PLAYING = 1, MEDIA_ENDED = 2, MEDIA_ERROR = -1 };

void media_sysinit(void);
media *media_open(const char *url, int audio_only);
int media_state(media *m);
const char *media_error(media *m);
/* Playback position in ms relative to the start of this stream. */
int64_t media_clock_ms(media *m);
/* Call once per frame on the main thread. Returns the frame to show (or NULL);
 * *w / *h receive the visible picture size inside the texture. */
vita2d_texture *media_video_frame(media *m, int *w, int *h);
void media_set_paused(media *m, int paused);
void media_close(media *m);

#endif
