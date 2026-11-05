#include "http.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char g_auth[1024];

void http_global_init(void) { curl_global_init(CURL_GLOBAL_ALL); }
void http_global_cleanup(void) { curl_global_cleanup(); }

void http_set_auth_header(const char *line) {
  snprintf(g_auth, sizeof(g_auth), "%s", line ? line : "");
}

typedef struct { char *data; size_t size, cap; } membuf;

static size_t on_write(void *ptr, size_t sz, size_t n, void *ud) {
  membuf *b = ud;
  size_t add = sz * n;
  if (b->size + add + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 16384;
    while (nc < b->size + add + 1) nc *= 2;
    char *nd = realloc(b->data, nc);
    if (!nd) return 0;
    b->data = nd; b->cap = nc;
  }
  memcpy(b->data + b->size, ptr, add);
  b->size += add;
  b->data[b->size] = 0;
  return add;
}

static void common_opts(CURL *c, const char *url) {
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaFin/1.0 (PlayStation Vita)");
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
}

static struct curl_slist *base_headers(int json) {
  struct curl_slist *h = NULL;
  if (g_auth[0]) h = curl_slist_append(h, g_auth);
  if (json) h = curl_slist_append(h, "Content-Type: application/json");
  h = curl_slist_append(h, "Accept: application/json");
  return h;
}

int http_request(const char *method, const char *url, const char *json_body, http_resp *out) {
  memset(out, 0, sizeof(*out));
  CURL *c = curl_easy_init();
  if (!c) { snprintf(out->error, sizeof(out->error), "curl init failed"); return -1; }
  membuf b = {0};
  struct curl_slist *h = base_headers(json_body != NULL);
  common_opts(c, url);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  if (!strcmp(method, "POST")) {
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body ? json_body : "");
  } else if (strcmp(method, "GET")) {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
  }
  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK) {
    snprintf(out->error, sizeof(out->error), "%s", curl_easy_strerror(rc));
  } else {
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
  }
  curl_slist_free_all(h);
  curl_easy_cleanup(c);
  if (!b.data) { b.data = calloc(1, 1); }
  out->data = b.data;
  out->size = b.size;
  return rc == CURLE_OK ? 0 : -1;
}

void http_resp_free(http_resp *r) {
  free(r->data);
  r->data = NULL;
  r->size = 0;
}

/* ------------------------------------------------------------------ */
/* Streaming reader                                                    */
/*                                                                     */
/* Seekable (server answered 206): an LRU cache of fixed-size blocks,  */
/* each filled by one Range request, plus a prefetch thread that keeps */
/* the next few blocks warm.                                           */
/*                                                                     */
/* Not seekable (live transcode, 200 with no length): one long GET     */
/* feeding a ring buffer; reads block until data arrives.              */
/* ------------------------------------------------------------------ */

#define BLOCK_SIZE   (512 * 1024)
#define NUM_BLOCKS   12
#define PREFETCH     4
#define RING_SIZE    (24 * 1024 * 1024)

typedef struct {
  int64_t index;   /* block number, -1 = empty */
  uint32_t len;
  uint64_t last_use;
  int loading;
  uint8_t *data;
} block;

struct http_stream {
  char *url;
  uint64_t size;
  int seekable;
  volatile int quit;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  int thread_started;

  /* seekable mode */
  block blocks[NUM_BLOCKS];
  uint64_t tick;
  int64_t want_from;     /* prefetch hint */
  CURL *fg;              /* handle for foreground fetches (keep-alive) */

  /* ring mode */
  uint8_t *ring;
  uint64_t ring_base;    /* absolute offset of oldest byte kept */
  uint64_t ring_end;     /* absolute offset one past newest byte */
  int eof, failed;
  int http_status, curl_error;
};

typedef struct { uint8_t *dst; uint32_t cap, len; } rangebuf;

static size_t on_range(void *ptr, size_t sz, size_t n, void *ud) {
  rangebuf *r = ud;
  size_t add = sz * n, room = r->cap - r->len;
  if (add > room) add = room;
  memcpy(r->dst + r->len, ptr, add);
  r->len += add;
  return sz * n;
}

static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  (void)a; (void)b; (void)c; (void)d;
  return ((http_stream *)ud)->quit;
}

static void abortable(CURL *c, http_stream *s) {
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, s);
}

static int fetch_range(http_stream *s, CURL *c, const char *url, uint64_t from, uint32_t len, uint8_t *dst, uint32_t *got) {
  char range[64];
  rangebuf rb = { dst, len, 0 };
  struct curl_slist *h = NULL;
  if (g_auth[0]) h = curl_slist_append(h, g_auth);
  snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)from, (unsigned long long)(from + len - 1));
  curl_easy_reset(c);
  common_opts(c, url);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, NULL);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_RANGE, range);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_range);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &rb);
  abortable(c, s);
  CURLcode rc = curl_easy_perform(c);
  curl_slist_free_all(h);
  *got = rb.len;
  return rc == CURLE_OK ? 0 : -1;
}

static block *find_block(http_stream *s, int64_t idx) {
  for (int i = 0; i < NUM_BLOCKS; i++)
    if (s->blocks[i].index == idx) return &s->blocks[i];
  return NULL;
}

static block *victim(http_stream *s, int64_t protect) {
  block *best = NULL;
  for (int i = 0; i < NUM_BLOCKS; i++) {
    block *b = &s->blocks[i];
    if (b->loading) continue;
    if (b->index >= protect && b->index < protect + PREFETCH + 1) continue;
    if (b->index < 0) return b;
    if (!best || b->last_use < best->last_use) best = b;
  }
  return best;
}

/* Fill block idx. Called with lock held; drops it during the transfer. */
static int load_block(http_stream *s, CURL *c, int64_t idx, int64_t protect) {
  block *b = victim(s, protect);
  if (!b) return -1;
  uint64_t from = (uint64_t)idx * BLOCK_SIZE;
  if (from >= s->size) return -1;
  uint32_t len = BLOCK_SIZE;
  if (from + len > s->size) len = (uint32_t)(s->size - from);
  b->index = idx;
  b->loading = 1;
  b->len = 0;
  pthread_mutex_unlock(&s->lock);
  uint32_t got = 0;
  int rc = fetch_range(s, c, s->url, from, len, b->data, &got);
  pthread_mutex_lock(&s->lock);
  b->loading = 0;
  if (rc || got == 0) { b->index = -1; pthread_cond_broadcast(&s->cond); return -1; }
  b->len = got;
  b->last_use = ++s->tick;
  pthread_cond_broadcast(&s->cond);
  return 0;
}

static void *prefetch_thread(void *arg) {
  http_stream *s = arg;
  CURL *c = curl_easy_init();
  pthread_mutex_lock(&s->lock);
  while (!s->quit) {
    int64_t base = s->want_from, todo = -1;
    for (int64_t i = base; i < base + PREFETCH; i++) {
      if ((uint64_t)i * BLOCK_SIZE >= s->size) break;
      if (!find_block(s, i)) { todo = i; break; }
    }
    if (todo < 0) { pthread_cond_wait(&s->cond, &s->lock); continue; }
    load_block(s, c, todo, base);
  }
  pthread_mutex_unlock(&s->lock);
  curl_easy_cleanup(c);
  return NULL;
}

/* ring mode */
static size_t on_ring(void *ptr, size_t sz, size_t n, void *ud) {
  http_stream *s = ud;
  size_t total = sz * n, off = 0;
  pthread_mutex_lock(&s->lock);
  while (off < total && !s->quit) {
    /* Never overwrite data within 8 MB behind the newest byte AvPlayer asked for. */
    uint64_t used = s->ring_end - s->ring_base;
    if (used >= RING_SIZE) {
      if (s->want_from > (int64_t)(s->ring_base + 8 * 1024 * 1024)) {
        s->ring_base += 1024 * 1024;
        continue;
      }
      pthread_cond_wait(&s->cond, &s->lock);
      continue;
    }
    size_t chunk = total - off;
    size_t room = RING_SIZE - used;
    if (chunk > room) chunk = room;
    size_t at = s->ring_end % RING_SIZE;
    size_t first = chunk < RING_SIZE - at ? chunk : RING_SIZE - at;
    memcpy(s->ring + at, (uint8_t *)ptr + off, first);
    if (chunk > first) memcpy(s->ring, (uint8_t *)ptr + off + first, chunk - first);
    s->ring_end += chunk;
    off += chunk;
    pthread_cond_broadcast(&s->cond);
  }
  int quit = s->quit;
  pthread_mutex_unlock(&s->lock);
  return quit ? 0 : total;
}

static void *ring_thread(void *arg) {
  http_stream *s = arg;
  CURL *c = curl_easy_init();
  struct curl_slist *h = NULL;
  if (g_auth[0]) h = curl_slist_append(h, g_auth);
  common_opts(c, s->url);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, NULL);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_ring);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);  /* an HTTP error page is not video */
  abortable(c, s);
  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  s->http_status = (int)code;
  if (rc != CURLE_OK && !s->quit) s->curl_error = (int)rc;
  curl_slist_free_all(h);
  curl_easy_cleanup(c);
  pthread_mutex_lock(&s->lock);
  s->eof = 1;
  s->failed = rc != CURLE_OK && !s->quit;
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->lock);
  return NULL;
}

/* Abort probe bodies early: a server ignoring Range would otherwise stream the whole file. */
static size_t on_discard(void *p, size_t sz, size_t n, void *ud) {
  size_t *seen = ud;
  (void)p;
  *seen += sz * n;
  return *seen > 65536 ? 0 : sz * n;
}

static size_t on_header(char *buf, size_t sz, size_t n, void *ud) {
  uint64_t *total = ud;
  size_t len = sz * n;
  if (len > 14 && !strncasecmp(buf, "Content-Range:", 14)) {
    const char *slash = memchr(buf, '/', len);
    if (slash && slash[1] != '*') *total = strtoull(slash + 1, NULL, 10);
  }
  return len;
}

static pthread_t spawn(void *(*fn)(void *), void *arg) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 * 1024);
  pthread_create(&t, &a, fn, arg);
  pthread_attr_destroy(&a);
  return t;
}

http_stream *http_stream_open(const char *url) {
  http_stream *s = calloc(1, sizeof(*s));
  s->url = strdup(url);
  pthread_mutex_init(&s->lock, NULL);
  pthread_cond_init(&s->cond, NULL);

  /* Probe range support with a 1-byte request. */
  CURL *c = curl_easy_init();
  struct curl_slist *h = NULL;
  uint64_t total = 0;
  size_t seen = 0;
  long status = 0;
  if (g_auth[0]) h = curl_slist_append(h, g_auth);
  common_opts(c, url);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, NULL);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &total);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_discard);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &seen);
  /* For a non-ranged 200 we don't want to download the whole file here. */
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)4096);
  CURLcode rc = curl_easy_perform(c);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(h);

  if ((rc == CURLE_OK || rc == CURLE_FILESIZE_EXCEEDED) && status == 206 && total > 0) {
    s->seekable = 1;
    s->size = total;
    s->fg = c;
    for (int i = 0; i < NUM_BLOCKS; i++) {
      s->blocks[i].index = -1;
      s->blocks[i].data = malloc(BLOCK_SIZE);
    }
    s->thread = spawn(prefetch_thread, s);
    s->thread_started = 1;
    return s;
  }
  curl_easy_cleanup(c);
  if (rc != CURLE_OK && rc != CURLE_FILESIZE_EXCEEDED && rc != CURLE_WRITE_ERROR) {
    http_stream_close(s);
    return NULL;
  }
  if (status >= 400) { http_stream_close(s); return NULL; }

  s->seekable = 0;
  s->ring = malloc(RING_SIZE);
  if (!s->ring) { http_stream_close(s); return NULL; }
  /* Unknown length: advertise a large size; reads past EOF return 0. */
  s->size = 8ULL * 1024 * 1024 * 1024;
  s->thread = spawn(ring_thread, s);
  s->thread_started = 1;
  return s;
}

http_stream *http_stream_open_live(const char *url) {
  http_stream *s = calloc(1, sizeof(*s));
  s->url = strdup(url);
  pthread_mutex_init(&s->lock, NULL);
  pthread_cond_init(&s->cond, NULL);
  s->seekable = 0;
  s->ring = malloc(RING_SIZE);
  if (!s->ring) { http_stream_close(s); return NULL; }
  s->size = 8ULL * 1024 * 1024 * 1024;
  s->thread = spawn(ring_thread, s);
  s->thread_started = 1;
  return s;
}

uint64_t http_stream_size(http_stream *s) { return s->size; }
int http_stream_status(http_stream *s, int *curl_error) { if (curl_error) *curl_error = s->curl_error; return s->http_status; }
int http_stream_seekable(http_stream *s) { return s->seekable; }

int http_stream_read(http_stream *s, uint8_t *buf, uint64_t pos, uint32_t len) {
  if (pos >= s->size) return 0;
  if (pos + len > s->size) len = (uint32_t)(s->size - pos);
  pthread_mutex_lock(&s->lock);

  if (!s->seekable) {
    if (pos < s->ring_base) { pthread_mutex_unlock(&s->lock); return -1; }
    s->want_from = pos;
    pthread_cond_broadcast(&s->cond);
    /* return as soon as anything is available, so low-bitrate streams don't stall */
    while (!s->quit && !s->eof && s->ring_end <= pos) pthread_cond_wait(&s->cond, &s->lock);
    if (s->ring_end <= pos) { pthread_mutex_unlock(&s->lock); return s->failed ? -1 : 0; }
    uint32_t n = s->ring_end - pos < len ? (uint32_t)(s->ring_end - pos) : len;
    for (uint32_t done = 0; done < n;) {
      size_t at = (pos + done) % RING_SIZE;
      size_t run = RING_SIZE - at < n - done ? RING_SIZE - at : n - done;
      memcpy(buf + done, s->ring + at, run);
      done += run;
    }
    pthread_mutex_unlock(&s->lock);
    return n;
  }

  uint32_t done = 0;
  while (done < len && !s->quit) {
    uint64_t p = pos + done;
    int64_t idx = p / BLOCK_SIZE;
    if (s->want_from != idx + 1) { s->want_from = idx + 1; pthread_cond_broadcast(&s->cond); }
    block *b = find_block(s, idx);
    if (!b) {
      if (load_block(s, s->fg, idx, idx) < 0) break;
      continue;
    }
    if (b->loading) { pthread_cond_wait(&s->cond, &s->lock); continue; }
    uint32_t off = p - (uint64_t)idx * BLOCK_SIZE;
    if (off >= b->len) break;
    uint32_t run = b->len - off < len - done ? b->len - off : len - done;
    memcpy(buf + done, b->data + off, run);
    b->last_use = ++s->tick;
    done += run;
  }
  pthread_mutex_unlock(&s->lock);
  return done ? (int)done : -1;
}

void http_stream_abort(http_stream *s) {
  if (!s) return;
  pthread_mutex_lock(&s->lock);
  s->quit = 1;
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->lock);
}

void http_stream_close(http_stream *s) {
  if (!s) return;
  http_stream_abort(s);
  if (s->thread_started) pthread_join(s->thread, NULL);
  if (s->fg) curl_easy_cleanup(s->fg);
  for (int i = 0; i < NUM_BLOCKS; i++) free(s->blocks[i].data);
  free(s->ring);
  free(s->url);
  pthread_mutex_destroy(&s->lock);
  pthread_cond_destroy(&s->cond);
  free(s);
}
