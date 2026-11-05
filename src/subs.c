#include "subs.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- SRT parsing ---------------- */

static int parse_ts(const char *p, int64_t *ms, const char **end) {
  int h, m, s, f, n = 0;
  if (sscanf(p, "%d:%d:%d%*[,.]%d%n", &h, &m, &s, &f, &n) < 4 || n == 0) return 0;
  /* fraction may be 1-3 digits */
  const char *q = p;
  while (*q && *q != ',' && *q != '.') q++;
  int digits = 0;
  if (*q) for (const char *r = q + 1; *r >= '0' && *r <= '9'; r++) digits++;
  if (digits == 1) f *= 100;
  else if (digits == 2) f *= 10;
  *ms = ((int64_t)h * 3600 + m * 60 + s) * 1000 + f;
  *end = p + n;
  return 1;
}

/* Strip markup (<i>, <font ...>, {\an8} ASS overrides) and decode a few entities. */
static void clean_text(char *s) {
  char *w = s;
  for (char *r = s; *r;) {
    if (*r == '<') { char *e = strchr(r, '>'); if (e) { r = e + 1; continue; } }
    if (*r == '{' && r[1] == '\\') { char *e = strchr(r, '}'); if (e) { r = e + 1; continue; } }
    if (*r == '\\' && (r[1] == 'N' || r[1] == 'n')) { *w++ = '\n'; r += 2; continue; }
    if (*r == '&') {
      static const struct { const char *e; char c; } ents[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&nbsp;", ' ' } };
      int hit = 0;
      for (unsigned i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        size_t l = strlen(ents[i].e);
        if (!strncmp(r, ents[i].e, l)) { *w++ = ents[i].c; r += l; hit = 1; break; }
      }
      if (hit) continue;
    }
    if (*r == '\r') { r++; continue; }
    *w++ = *r++;
  }
  *w = 0;
  /* trim trailing whitespace / newlines */
  while (w > s && (w[-1] == '\n' || w[-1] == ' ')) *--w = 0;
}

static int cmp_cue(const void *a, const void *b) {
  int64_t x = ((const sub_cue *)a)->start, y = ((const sub_cue *)b)->start;
  return x < y ? -1 : x > y;
}

int subs_parse_srt(const char *data, size_t len, sub_track *out) {
  memset(out, 0, sizeof(*out));
  char *buf = malloc(len + 1);
  if (!buf) return 0;
  memcpy(buf, data, len);
  buf[len] = 0;
  char *p = buf;
  if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
  int cap = 0;
  while (*p) {
    /* find the next timing line */
    char *arrow = strstr(p, "-->");
    if (!arrow) break;
    char *ls = arrow;
    while (ls > p && ls[-1] != '\n') ls--;
    int64_t a, b;
    const char *e1, *e2;
    char *after = arrow + 3;
    while (*after == ' ') after++;
    if (!parse_ts(ls, &a, &e1) || !parse_ts(after, &b, &e2)) { p = arrow + 3; continue; }
    char *text = strchr(arrow, '\n');
    if (!text) break;
    text++;
    /* text runs until a blank line */
    char *end = text;
    while (*end) {
      char *nl = strchr(end, '\n');
      if (!nl) { end += strlen(end); break; }
      char *next = nl + 1;
      if (*next == '\n' || (*next == '\r' && next[1] == '\n')) { end = nl; break; }
      end = next;
    }
    size_t tl = end - text;
    char *t = malloc(tl + 1);
    memcpy(t, text, tl);
    t[tl] = 0;
    clean_text(t);
    if (t[0] && b > a) {
      if (out->n == cap) {
        cap = cap ? cap * 2 : 256;
        out->cues = realloc(out->cues, cap * sizeof(sub_cue));
      }
      out->cues[out->n].start = a;
      out->cues[out->n].end = b;
      out->cues[out->n].text = t;
      out->n++;
    } else {
      free(t);
    }
    p = *end ? end + 1 : end;
  }
  free(buf);
  qsort(out->cues, out->n, sizeof(sub_cue), cmp_cue);
  return out->n;
}

void subs_free(sub_track *t) {
  for (int i = 0; i < t->n; i++) free(t->cues[i].text);
  free(t->cues);
  memset(t, 0, sizeof(*t));
}

const char *subs_at(const sub_track *t, int64_t ms, char *buf, size_t cap) {
  if (!t || !t->n) return NULL;
  /* last cue starting at or before ms */
  int lo = 0, hi = t->n - 1, last = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (t->cues[mid].start <= ms) { last = mid; lo = mid + 1; } else hi = mid - 1;
  }
  if (last < 0) return NULL;
  buf[0] = 0;
  int shown = 0;
  /* look back a little for overlapping cues (songs, two speakers) */
  int first = last - 8 < 0 ? 0 : last - 8;
  for (int i = first; i <= last; i++) {
    if (t->cues[i].end <= ms) continue;
    size_t l = strlen(buf);
    snprintf(buf + l, cap - l, "%s%s", shown ? "\n" : "", t->cues[i].text);
    shown++;
  }
  return shown ? buf : NULL;
}

/* ---------------- drawing ---------------- */

#define SUB_PX 27
#define SUB_LINE 34
#define SUB_MAXW 880
#define SUB_MAXLINES 6

/* Word-wrap one source line into display lines. */
static int wrap_line(const char *src, char lines[][256], int max) {
  int n = 0;
  char cur[256] = "";
  const char *p = src;
  while (*p && n < max) {
    const char *ws = p;
    while (*p && *p != ' ') p++;
    char word[256];
    snprintf(word, sizeof(word), "%.*s", (int)(p - ws), ws);
    while (*p == ' ') p++;
    char trial[256];
    snprintf(trial, sizeof(trial), "%s%s%s", cur, cur[0] ? " " : "", word);
    if (cur[0] && ui_text_w(SUB_PX, trial) > SUB_MAXW) {
      snprintf(lines[n++], 256, "%s", cur);
      snprintf(cur, sizeof(cur), "%s", word);
    } else {
      snprintf(cur, sizeof(cur), "%s", trial);
    }
  }
  if (cur[0] && n < max) snprintf(lines[n++], 256, "%s", cur);
  return n;
}

void subs_draw(const char *text, float bottom_y, float alpha) {
  if (!text || !text[0] || alpha <= 0.01f) return;
  char lines[SUB_MAXLINES][256];
  int n = 0;
  const char *p = text;
  while (*p && n < SUB_MAXLINES) {
    const char *nl = strchr(p, '\n');
    size_t l = nl ? (size_t)(nl - p) : strlen(p);
    char one[256];
    snprintf(one, sizeof(one), "%.*s", (int)(l < 255 ? l : 255), p);
    n += wrap_line(one, lines + n, SUB_MAXLINES - n);
    if (!nl) break;
    p = nl + 1;
  }
  uint32_t edge = col_alpha(C_RGBA(0, 0, 0, 235), alpha), fill = col_alpha(COL_TEXT, alpha);
  static const int ox[8] = { -2, -1, 0, 1, 2, 1, 0, -1 }, oy[8] = { 0, -1, -2, -1, 0, 1, 2, 1 };
  for (int i = 0; i < n; i++) {
    float cy = bottom_y - (n - 1 - i) * SUB_LINE;
    float x = (SCREEN_W - ui_text_w(SUB_PX, lines[i])) / 2.0f;
    /* black edge like the system video player, then the white face */
    for (int k = 0; k < 8; k++) ui_text_vc(x + ox[k], cy + oy[k], SUB_PX, edge, lines[i]);
    ui_text_vc(x, cy, SUB_PX, fill, lines[i]);
  }
}
