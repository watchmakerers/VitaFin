#ifndef SUBS_H
#define SUBS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  int64_t start, end;  /* ms on the item's own timeline */
  char *text;          /* plain text, lines separated by '\n' */
} sub_cue;

typedef struct {
  sub_cue *cues;
  int n;
} sub_track;

/* Parse SubRip text. Returns number of cues (0 if none / not SRT). */
int subs_parse_srt(const char *data, size_t len, sub_track *out);
void subs_free(sub_track *t);
/* Text to show at ms (overlapping cues joined), or NULL. buf receives the result. */
const char *subs_at(const sub_track *t, int64_t ms, char *buf, size_t cap);
/* Draw centred, outlined subtitle text whose last line sits at bottom_y. */
void subs_draw(const char *text, float bottom_y, float alpha);

#endif
