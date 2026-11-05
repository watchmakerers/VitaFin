#include "images.h"
#include "http.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 192
#define KEEP_FRAMES 90     /* textures unseen for this long may be evicted */
#define SOFT_LIMIT  110    /* start evicting above this many textures */
#define QUEUE_MAX   48

typedef enum { E_FREE, E_QUEUED, E_LOADING, E_READY, E_FAILED } estate;

typedef struct {
  estate state;
  unsigned hash;
  char *url;
  vita2d_texture *tex;
  unsigned last_seen;
  unsigned ready_frame;
  unsigned queued_at;
} entry;

static entry g_e[MAX_ENTRIES];
static unsigned g_frame;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static volatile int g_quit;

static unsigned hash_str(const char *s) {
  unsigned h = 2166136261u;
  while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
  return h;
}

static entry *lookup(const char *url, unsigned h) {
  for (int i = 0; i < MAX_ENTRIES; i++)
    if (g_e[i].state != E_FREE && g_e[i].hash == h && !strcmp(g_e[i].url, url)) return &g_e[i];
  return NULL;
}

static vita2d_texture *decode(const uint8_t *d, size_t n) {
  if (n > 3 && d[0] == 0xFF && d[1] == 0xD8) return vita2d_load_JPEG_buffer(d, n);
  if (n > 8 && d[0] == 0x89 && d[1] == 'P') return vita2d_load_PNG_buffer(d);
  if (n > 2 && d[0] == 'B' && d[1] == 'M') return vita2d_load_BMP_buffer(d);
  return NULL;
}

static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_lock);
  while (!g_quit) {
    /* LIFO: the most recently requested image is most likely on screen. */
    entry *pick = NULL;
    for (int i = 0; i < MAX_ENTRIES; i++)
      if (g_e[i].state == E_QUEUED && (!pick || g_e[i].queued_at > pick->queued_at)) pick = &g_e[i];
    if (!pick) { pthread_cond_wait(&g_cond, &g_lock); continue; }
    pick->state = E_LOADING;
    char *url = strdup(pick->url);
    pthread_mutex_unlock(&g_lock);

    http_resp r;
    vita2d_texture *tex = NULL;
    if (http_request("GET", url, NULL, &r) == 0 && r.status == 200) tex = decode((uint8_t *)r.data, r.size);
    http_resp_free(&r);
    if (tex) vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);

    pthread_mutex_lock(&g_lock);
    entry *e = lookup(url, hash_str(url));
    free(url);
    if (e && e->state == E_LOADING) {
      e->tex = tex;
      e->state = tex ? E_READY : E_FAILED;
      e->ready_frame = g_frame;
    } else if (tex) {
      /* entry was dropped meanwhile; GPU never saw this texture */
      vita2d_free_texture(tex);
    }
  }
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

void images_init(void) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 * 1024);
  pthread_create(&g_thread, &a, worker, NULL);
  pthread_attr_destroy(&a);
}

void images_shutdown(void) {
  pthread_mutex_lock(&g_lock);
  g_quit = 1;
  pthread_cond_broadcast(&g_cond);
  pthread_mutex_unlock(&g_lock);
  pthread_join(g_thread, NULL);
  vita2d_wait_rendering_done();
  for (int i = 0; i < MAX_ENTRIES; i++) {
    if (g_e[i].tex) vita2d_free_texture(g_e[i].tex);
    free(g_e[i].url);
  }
  memset(g_e, 0, sizeof(g_e));
}

static void drop(entry *e) {
  free(e->url);
  memset(e, 0, sizeof(*e));
}

void images_frame(void) {
  g_frame++;
  pthread_mutex_lock(&g_lock);
  int ready = 0, queued = 0;
  for (int i = 0; i < MAX_ENTRIES; i++) {
    if (g_e[i].state == E_READY) ready++;
    if (g_e[i].state == E_QUEUED) queued++;
  }
  /* Stale queued requests (scrolled past) are dropped. */
  for (int i = 0; i < MAX_ENTRIES && queued > QUEUE_MAX; i++)
    if (g_e[i].state == E_QUEUED && g_frame - g_e[i].last_seen > 2) { drop(&g_e[i]); queued--; }
  if (ready > SOFT_LIMIT) {
    int freed_any = 0;
    for (int pass = 0; pass < MAX_ENTRIES && ready > SOFT_LIMIT; pass++) {
      entry *old = NULL;
      for (int i = 0; i < MAX_ENTRIES; i++)
        if (g_e[i].state == E_READY && g_frame - g_e[i].last_seen > KEEP_FRAMES && (!old || g_e[i].last_seen < old->last_seen))
          old = &g_e[i];
      if (!old) break;
      if (!freed_any) { vita2d_wait_rendering_done(); freed_any = 1; }
      vita2d_free_texture(old->tex);
      drop(old);
      ready--;
    }
  }
  pthread_mutex_unlock(&g_lock);
}

vita2d_texture *images_get(const char *url, float *alpha) {
  if (alpha) *alpha = 0;
  if (!url || !url[0]) return NULL;
  unsigned h = hash_str(url);
  vita2d_texture *tex = NULL;
  pthread_mutex_lock(&g_lock);
  entry *e = lookup(url, h);
  if (!e) {
    for (int i = 0; i < MAX_ENTRIES; i++)
      if (g_e[i].state == E_FREE) { e = &g_e[i]; break; }
    if (!e) {
      /* Table full: recycle the oldest failed/queued entry. */
      for (int i = 0; i < MAX_ENTRIES; i++)
        if ((g_e[i].state == E_FAILED || g_e[i].state == E_QUEUED) && (!e || g_e[i].last_seen < e->last_seen)) e = &g_e[i];
      if (e) drop(e);
    }
    if (e) {
      e->state = E_QUEUED;
      e->hash = h;
      e->url = strdup(url);
      e->queued_at = g_frame;
      pthread_cond_signal(&g_cond);
    }
  }
  if (e) {
    e->last_seen = g_frame;
    if (e->state == E_QUEUED) e->queued_at = g_frame;
    if (e->state == E_READY) {
      tex = e->tex;
      if (alpha) {
        float a = (g_frame - e->ready_frame) / 12.0f;
        *alpha = a > 1 ? 1 : a;
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
  return tex;
}

void images_cancel_pending(void) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_ENTRIES; i++)
    if (g_e[i].state == E_QUEUED || g_e[i].state == E_FAILED) drop(&g_e[i]);
  pthread_mutex_unlock(&g_lock);
}
