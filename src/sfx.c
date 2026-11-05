#include "sfx.h"
#include "log.h"
#include "config.h"
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define GRAIN 256
#define VOICES 6

typedef struct { int16_t *pcm; int len; } sound;
typedef struct { const sound *s; int pos; } voice;

static sound g_snd[SFX_COUNT];
static voice g_voice[VOICES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_port = -1;

static float frand(unsigned *seed) { *seed = *seed * 1664525u + 1013904223u; return ((*seed >> 9) / 4194304.0f) - 1.0f; }

/* A pitched blip: sine sweeping f0 -> f1 with a fast attack and exponential decay,
 * plus a little noise transient at the front for the "tick". */
static void blip(sound *s, float ms, float f0, float f1, float decay, float noise, float gain) {
  int n = (int)(RATE * ms / 1000);
  s->pcm = calloc(n, sizeof(int16_t));
  s->len = n;
  float ph = 0;
  unsigned seed = 1234;
  for (int i = 0; i < n; i++) {
    float t = (float)i / RATE, k = (float)i / n;
    float f = f0 * powf(f1 / f0, k);
    ph += 2 * (float)M_PI * f / RATE;
    float env = (1 - expf(-t * 2500)) * expf(-t * decay);
    float v = sinf(ph) * 0.8f + sinf(ph * 2) * 0.12f;
    v += frand(&seed) * noise * expf(-t * 900);
    float o = v * env * gain;
    s->pcm[i] = (int16_t)(fmaxf(-1, fminf(1, o)) * 32000);
  }
}

/* Airy swish for page transitions: band-limited noise with a soft swell. */
static void swish(sound *s, float ms, float gain) {
  int n = (int)(RATE * ms / 1000);
  s->pcm = calloc(n, sizeof(int16_t));
  s->len = n;
  unsigned seed = 99;
  float lp = 0, hp = 0, prev = 0;
  for (int i = 0; i < n; i++) {
    float k = (float)i / n;
    float cut = 0.05f + 0.25f * sinf(k * (float)M_PI);
    float x = frand(&seed);
    lp += (x - lp) * cut;
    hp = lp - prev;
    prev = lp;
    float env = sinf(k * (float)M_PI);
    env *= env;
    s->pcm[i] = (int16_t)(fmaxf(-1, fminf(1, (lp * 0.6f + hp * 1.5f) * env * gain)) * 32000);
  }
}

static void *mixer(void *arg) {
  (void)arg;
  int16_t buf[2][GRAIN * 2];
  int cur = 0;
  for (;;) {
    int active = 0;
    int32_t mix[GRAIN];
    memset(mix, 0, sizeof(mix));
    pthread_mutex_lock(&g_lock);
    for (int v = 0; v < VOICES; v++) {
      voice *vo = &g_voice[v];
      if (!vo->s) continue;
      active = 1;
      int n = vo->s->len - vo->pos;
      if (n > GRAIN) n = GRAIN;
      for (int i = 0; i < n; i++) mix[i] += vo->s->pcm[vo->pos + i];
      vo->pos += n;
      if (vo->pos >= vo->s->len) vo->s = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    if (!active) { sceKernelDelayThread(4000); continue; }
    for (int i = 0; i < GRAIN; i++) {
      int32_t m = mix[i];
      if (m > 32767) m = 32767;
      if (m < -32768) m = -32768;
      buf[cur][i * 2] = buf[cur][i * 2 + 1] = (int16_t)m;
    }
    sceAudioOutOutput(g_port, buf[cur]);
    cur ^= 1;
  }
  return NULL;
}

/* ---- user-supplied sounds: ux0:data/VitaFin/sounds/<name>.wav ---- */

static const char *k_names[SFX_COUNT] = { "move", "select", "back", "open", "menu", "error" };

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* Any PCM WAV (8/16/24/32-bit int, any rate, any channel count) -> 48 kHz mono int16. */
static int load_wav(const char *path, sound *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 44 || n > 8 * 1024 * 1024) { fclose(f); return 0; }
  uint8_t *b = malloc(n);
  fread(b, 1, n, f);
  fclose(f);
  int ok = 0;
  if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) goto done;
  int ch = 0, rate = 0, bits = 0, fmt = 0;
  const uint8_t *data = NULL;
  uint32_t dlen = 0;
  for (long o = 12; o + 8 <= n;) {
    uint32_t len = rd32(b + o + 4);
    if (!memcmp(b + o, "fmt ", 4) && len >= 16) {
      fmt = rd16(b + o + 8); ch = rd16(b + o + 10); rate = rd32(b + o + 12); bits = rd16(b + o + 22);
      if (fmt == 0xFFFE && len >= 40) fmt = rd16(b + o + 32);  /* WAVE_FORMAT_EXTENSIBLE */
    } else if (!memcmp(b + o, "data", 4)) {
      data = b + o + 8;
      dlen = len;
      if (o + 8 + dlen > (uint32_t)n) dlen = n - o - 8;
    }
    o += 8 + len + (len & 1);
  }
  if (fmt != 1 || !data || ch < 1 || rate < 4000 || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
    applog("sfx: %s: unsupported WAV (format %d, %d bit, %d ch)", path, fmt, bits, ch);
    goto done;
  }
  int bps = bits / 8, frames = dlen / (bps * ch);
  int outn = (int)((int64_t)frames * RATE / rate);
  out->pcm = malloc(outn * sizeof(int16_t));
  out->len = outn;
  for (int i = 0; i < outn; i++) {
    float src = (float)i * rate / RATE;
    int i0 = (int)src;
    float fr = src - i0, acc = 0;
    for (int k = 0; k < 2; k++) {
      int fi = i0 + k;
      if (fi >= frames) fi = frames - 1;
      float w = k ? fr : 1 - fr, m = 0;
      for (int c = 0; c < ch; c++) {
        const uint8_t *p = data + ((size_t)fi * ch + c) * bps;
        float v = bps == 1 ? (p[0] - 128) / 128.0f : bps == 2 ? (int16_t)rd16(p) / 32768.0f :
                  bps == 3 ? ((int32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.0f :
                  (int32_t)rd32(p) / 2147483648.0f;
        m += v;
      }
      acc += w * m / ch;
    }
    out->pcm[i] = (int16_t)(fmaxf(-1, fminf(1, acc)) * 32767);
  }
  ok = 1;
done:
  free(b);
  return ok;
}

/* ---- the console's own UI sounds ----
 * The system UI sound effects are VAG (PS-ADPCM) clips stored in the firmware's
 * common PAF resource. We read them from the console at runtime; nothing of Sony's
 * ships with the app. Mapping from that file's soundindextable:
 *   01_0725 = "decide" for buttons, check boxes, links and the second corner button
 *   03_0725 = "decide" for the first corner button (back)
 *   04_0725 = short press tick (used for focus moves here) */

#define SYS_RCO "vs0:data/external/common/common_resource.rco"

static const int k_vag_coef[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };

static int decode_vag(const uint8_t *v, size_t avail, sound *out) {
  if (avail < 64 || memcmp(v, "VAGp", 4)) return 0;
  uint32_t size = (uint32_t)v[12] << 24 | v[13] << 16 | v[14] << 8 | v[15];
  uint32_t rate = (uint32_t)v[16] << 24 | v[17] << 16 | v[18] << 8 | v[19];
  if (size + 48 > avail) size = avail - 48;
  int frames = size / 16;
  int16_t *pcm = malloc(frames * 28 * sizeof(int16_t));
  int n = 0, s1 = 0, s2 = 0;
  for (int f = 0; f < frames; f++) {
    const uint8_t *b = v + 48 + f * 16;
    int shift = b[0] & 0x0F, pred = (b[0] >> 4) & 0x0F;
    if (b[1] == 7) break;
    if (pred > 4) pred = 0;
    for (int j = 0; j < 14; j++) {
      for (int h = 0; h < 2; h++) {
        int nib = h ? b[2 + j] >> 4 : b[2 + j] & 0x0F;
        int x = (int16_t)(nib << 12) >> shift;
        x += (s1 * k_vag_coef[pred][0] + s2 * k_vag_coef[pred][1]) >> 6;  /* floor, as the hardware does */
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        pcm[n++] = (int16_t)x;
        s2 = s1;
        s1 = x;
      }
    }
  }
  if (rate != RATE && rate > 0) {  /* resample to the mixer rate */
    int outn = (int)((int64_t)n * RATE / rate);
    int16_t *r = malloc(outn * sizeof(int16_t));
    for (int i = 0; i < outn; i++) {
      int si = (int)((int64_t)i * rate / RATE);
      r[i] = pcm[si < n ? si : n - 1];
    }
    free(pcm);
    pcm = r;
    n = outn;
  }
  out->pcm = pcm;
  out->len = n;
  return n > 0;
}

/* Find a VAG clip by its header name (e.g. "01_0725") inside a resource file. */
static int find_vag(const uint8_t *b, size_t n, const char *name, sound *out) {
  for (size_t o = 0; o + 64 <= n; o++) {
    if (b[o] != 'V' || memcmp(b + o, "VAGp", 4)) continue;
    if (strncmp((const char *)b + o + 32, name, 16)) continue;
    return decode_vag(b + o, n - o, out);
  }
  return 0;
}

static int load_system_sounds(void) {
  FILE *f = fopen(SYS_RCO, "rb");
  if (!f) { applog("sfx: %s not readable, using built-in sounds", SYS_RCO); return 0; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 8 * 1024 * 1024) { fclose(f); return 0; }
  uint8_t *b = malloc(n);
  fread(b, 1, n, f);
  fclose(f);
  static const struct { sfx_id id; const char *vag; } map[] = {
    { SFX_SELECT, "01_0725" }, { SFX_BACK, "03_0725" }, { SFX_MOVE, "04_0725" }, { SFX_MENU, "01_0725" },
  };
  int got = 0;
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    sound s2;
    if (find_vag(b, n, map[i].vag, &s2)) {
      free(g_snd[map[i].id].pcm);
      g_snd[map[i].id] = s2;
      got++;
    } else {
      applog("sfx: system sound %s not found", map[i].vag);
    }
  }
  free(b);
  if (got) {
    /* the system has no sound of its own for page swings or our error cue */
    free(g_snd[SFX_OPEN].pcm);
    g_snd[SFX_OPEN].pcm = NULL;
    g_snd[SFX_OPEN].len = 0;
    applog("sfx: using %d system UI sounds from %s", got, SYS_RCO);
  }
  return got;
}

/* ---- one-time map of the firmware's resource folders (to locate the system sounds) ---- */

static void list_dir(FILE *out, const char *path, int depth) {
  if (depth > 6) return;
  SceUID d = sceIoDopen(path);
  if (d < 0) { fprintf(out, "%s  <cannot open 0x%08X>\n", path, (unsigned)d); return; }
  SceIoDirent e;
  memset(&e, 0, sizeof(e));
  while (sceIoDread(d, &e) > 0) {
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", path, e.d_name);
    if (SCE_S_ISDIR(e.d_stat.st_mode)) {
      fprintf(out, "%s/\n", full);
      list_dir(out, full, depth + 1);
    } else {
      fprintf(out, "%s  %lld\n", full, (long long)e.d_stat.st_size);
    }
    memset(&e, 0, sizeof(e));
  }
  sceIoDclose(d);
}

static __attribute__((unused)) void map_firmware(void) {
  const char *outp = DATA_DIR "/vs0_list.txt";
  SceIoStat st;
  if (sceIoGetstat(outp, &st) >= 0) return;  /* already written */
  FILE *f = fopen(outp, "w");
  if (!f) return;
  list_dir(f, "vs0:vsh", 0);
  list_dir(f, "vs0:data/internal", 0);
  list_dir(f, "vs0:app", 0);
  fclose(f);
  applog("sfx: wrote firmware listing to %s", outp);
}

void sfx_init(void) {
  blip(&g_snd[SFX_MOVE], 28, 2600, 2200, 160, 0.25f, 0.16f);
  blip(&g_snd[SFX_SELECT], 70, 1500, 900, 60, 0.35f, 0.42f);
  blip(&g_snd[SFX_BACK], 80, 900, 560, 50, 0.30f, 0.40f);
  swish(&g_snd[SFX_OPEN], 220, 0.30f);
  blip(&g_snd[SFX_MENU], 90, 700, 1250, 40, 0.20f, 0.35f);
  blip(&g_snd[SFX_ERROR], 160, 260, 200, 18, 0.10f, 0.40f);

  /* precedence: user WAVs > the console's own UI sounds > synthesised fallback */
  load_system_sounds();
  for (int i = 0; i < SFX_COUNT; i++) {
    char path[256];
    snprintf(path, sizeof(path), DATA_DIR "/sounds/%s.wav", k_names[i]);
    sound s2;
    if (load_wav(path, &s2)) {
      free(g_snd[i].pcm);
      g_snd[i] = s2;
      applog("sfx: using %s (%d samples)", path, s2.len);
    }
  }
  sceIoMkdir(DATA_DIR "/sounds", 0777);

  g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, RATE, SCE_AUDIO_OUT_MODE_STEREO);
  if (g_port < 0) { applog("sfx: audio port failed 0x%08X", (unsigned)g_port); return; }
  int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
  sceAudioOutSetVolume(g_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 32 * 1024);
  pthread_create(&t, &a, mixer, NULL);
  pthread_attr_destroy(&a);
}

void sfx_play(sfx_id id) {
  if (g_port < 0 || id < 0 || id >= SFX_COUNT || !g_snd[id].len) return;
  pthread_mutex_lock(&g_lock);
  int slot = -1;
  for (int v = 0; v < VOICES; v++) if (!g_voice[v].s) { slot = v; break; }
  if (slot < 0) slot = 0;
  /* a new focus tick replaces a playing one rather than stacking */
  if (id == SFX_MOVE)
    for (int v = 0; v < VOICES; v++) if (g_voice[v].s == &g_snd[SFX_MOVE]) { slot = v; break; }
  g_voice[slot].s = &g_snd[id];
  g_voice[slot].pos = 0;
  pthread_mutex_unlock(&g_lock);
}
