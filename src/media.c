#include "media.h"
#include "http.h"
#include "log.h"
#include "ui.h"
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/videodec.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_W 1280
#define MAX_H 720
#define NUM_SLOTS 4
#define AQ_SIZE 512            /* ~10 s of AAC frames */
#define AQ_FRAME_MAX 3072
#define TS_PKT 188
#define READ_CHUNK (TS_PKT * 348)  /* ~64 KB */

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define NO_PTS (-1LL)

typedef enum { SLOT_FREE, SLOT_READY, SLOT_SHOWN, SLOT_RETIRED } slot_state;

typedef struct {
  vita2d_texture *tex;
  slot_state state;
  int64_t pts;        /* 90 kHz */
  unsigned retired_at;
  int w, h;
} slot;

typedef struct {
  int64_t pts;
  uint32_t len;
  uint8_t data[AQ_FRAME_MAX];
} aframe;

typedef struct {
  int64_t pts;
  uint8_t *data;
  uint32_t len;
} vau;

#define VQ_SIZE 360            /* ~15 s of video access units */
#define VQ_BYTES (16 * 1024 * 1024)

typedef struct {
  int pid;
  uint8_t *buf;
  uint32_t len, cap;
  int started;
} pes_buf;

struct media {
  char *url;
  int audio_only;
  http_stream *stream;
  volatile int quit;
  int state;
  char error[160];

  pthread_t demux_th, audio_th, video_th;
  int demux_started, audio_started, video_started;
  pthread_mutex_t lock;
  pthread_cond_t cond;

  /* demux */
  uint64_t read_pos;
  int pmt_pid, video_pid, audio_pid;
  pes_buf vpes, apes;
  int eof;

  /* video */
  vau vq[VQ_SIZE];
  int vq_head, vq_tail, vq_count;
  uint32_t vq_bytes;
  int video_done;
  int vdec_lib, vdec_ok;
  SceAvcdecCtrl vdec;
  SceUID vdec_mem;
  slot slots[NUM_SLOTS];
  int pic_w, pic_h;
  int64_t first_pts;     /* first presentation timestamp seen (any stream) */
  int vframes;

  /* audio */
  aframe *aq;
  int aq_head, aq_tail, aq_count;
  int adec_lib, adec_ok;
  SceAudiodecCtrl adec;
  SceAudiodecInfo ainfo;
  uint8_t *es_buf, *pcm_buf;
  int port, rate, channels;
  int audio_done;
  int64_t adts_next_pts;  /* raw ADTS mode synthetic clock */

  /* clock */
  int paused;
  int clock_started;
  int64_t clock_pts;      /* pts (90k) of the audio frame now playing */
  uint64_t clock_tick;    /* process time (us) when it started */
  int64_t clock_frame_us;
  uint64_t pause_tick;
  int64_t wall_base_pts;  /* video-only fallback */
  int64_t last_clock_ms;
  uint64_t wall_base_tick;
};

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

void media_sysinit(void) {}

static void fail(media *m, const char *msg, int code) {
  if (m->state == MEDIA_ERROR) return;
  if (code) snprintf(m->error, sizeof(m->error), "%s (0x%08X)", msg, (unsigned)code);
  else snprintf(m->error, sizeof(m->error), "%s", msg);
  applog("media error: %s", m->error);
  m->state = MEDIA_ERROR;
}

/* ------------------------------------------------------------------ */
/* Video                                                               */
/* ------------------------------------------------------------------ */

static int video_init(media *m) {
  static const int refs_try[] = { 8, 5, 4 };
  SceVideodecQueryInitInfoHwAvcdec init;
  int ret = -1, refs = 4;
  for (int i = 0; i < 3; i++) {
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = MAX_W;
    init.vertical = MAX_H;
    init.numOfRefFrames = refs_try[i];
    init.numOfStreams = 1;
    ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (ret >= 0) { refs = refs_try[i]; break; }
  }
  if (ret < 0) { fail(m, "The video decoder could not be started.", ret); return -1; }
  m->vdec_lib = 1;

  SceAvcdecQueryDecoderInfo q = { MAX_W, MAX_H, refs };
  SceAvcdecDecoderInfo out = { 0 };
  ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &out);
  if (ret < 0) { fail(m, "Video decoder memory query failed.", ret); return -1; }
  uint32_t sz = ALIGN(out.frameMemSize, 0x100000);
  m->vdec_mem = sceKernelAllocMemBlock("vdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
  if (m->vdec_mem < 0) { fail(m, "Not enough memory for the video decoder.", m->vdec_mem); return -1; }
  memset(&m->vdec, 0, sizeof(m->vdec));
  m->vdec.frameBuf.size = sz;
  sceKernelGetMemBlockBase(m->vdec_mem, &m->vdec.frameBuf.pBuf);
  ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &m->vdec, &q);
  if (ret < 0) { fail(m, "The video decoder could not be created.", ret); return -1; }
  m->vdec_ok = 1;

  for (int i = 0; i < NUM_SLOTS; i++) {
    m->slots[i].tex = vita2d_create_empty_texture_format(MAX_W, MAX_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (!m->slots[i].tex) { fail(m, "Not enough video memory.", 0); return -1; }
    vita2d_texture_set_filters(m->slots[i].tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    m->slots[i].state = SLOT_FREE;
  }
  applog("video decoder ready (refs=%d, mem=%u KB)", refs, (unsigned)(sz / 1024));
  return 0;
}

static void decode_video_au(media *m, const uint8_t *data, uint32_t len, int64_t pts) {
  if (!m->vdec_ok || len == 0) return;

  /* wait for a free output slot (back-pressure: the demuxer pauses while frames queue up) */
  pthread_mutex_lock(&m->lock);
  slot *s = NULL;
  while (!m->quit) {
    for (int i = 0; i < NUM_SLOTS; i++)
      if (m->slots[i].state == SLOT_FREE) { s = &m->slots[i]; break; }
    if (s) break;
    pthread_cond_wait(&m->cond, &m->lock);
  }
  pthread_mutex_unlock(&m->lock);
  if (!s) return;

  SceAvcdecAu au;
  memset(&au, 0, sizeof(au));
  au.es.pBuf = (void *)data;
  au.es.size = len;
  if (pts == NO_PTS) {
    au.pts.upper = au.pts.lower = 0xFFFFFFFF;
  } else {
    au.pts.upper = (uint32_t)((uint64_t)pts >> 32);
    au.pts.lower = (uint32_t)pts;
  }
  au.dts.upper = au.dts.lower = 0xFFFFFFFF;

  SceAvcdecPicture pic;
  SceAvcdecPicture *pics[1] = { &pic };
  SceAvcdecArrayPicture arr;
  memset(&pic, 0, sizeof(pic));
  memset(&arr, 0, sizeof(arr));
  pic.size = sizeof(pic);
  pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
  pic.frame.framePitch = vita2d_texture_get_stride(s->tex) / 4;
  pic.frame.frameWidth = MAX_W;
  pic.frame.frameHeight = MAX_H;
  pic.frame.opt.rgba.alpha = 0xFF;
  pic.frame.pPicture[0] = vita2d_texture_get_datap(s->tex);
  arr.numOfElm = 1;
  arr.pPicture = pics;

  int ret = sceAvcdecDecode(&m->vdec, &au, &arr);
  if (ret < 0) {
    static int warned;
    if (warned++ < 8) applog("sceAvcdecDecode 0x%08X (len %u)", (unsigned)ret, (unsigned)len);
    return;
  }
  if (arr.numOfOutput < 1) return;

  int64_t opts = ((int64_t)pic.info.pts.upper << 32) | pic.info.pts.lower;
  if (pic.info.pts.upper == 0xFFFFFFFF) opts = pts;
  int w = pic.frame.horizontalSize ? (int)pic.frame.horizontalSize : m->pic_w;
  int h = pic.frame.verticalSize ? (int)pic.frame.verticalSize : m->pic_h;
  int crop_w = w - (int)pic.frame.frameCropLeftOffset - (int)pic.frame.frameCropRightOffset;
  int crop_h = h - (int)pic.frame.frameCropTopOffset - (int)pic.frame.frameCropBottomOffset;
  if (crop_w > 16 && crop_w <= MAX_W) w = crop_w;
  if (crop_h > 16 && crop_h <= MAX_H) h = crop_h;
  if (w <= 0 || w > MAX_W) w = MAX_W;
  if (h <= 0 || h > MAX_H) h = MAX_H;

  pthread_mutex_lock(&m->lock);
  s->pts = opts;
  s->w = w;
  s->h = h;
  s->state = SLOT_READY;
  if (m->vframes++ == 0) applog("first video frame %dx%d pts=%lld", w, h, (long long)opts);
  pthread_mutex_unlock(&m->lock);
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

static const int k_aac_rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

static int adts_len(const uint8_t *h, uint32_t avail) {
  if (avail < 7 || h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) return -1;
  int len = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
  return len >= 7 ? len : -1;
}

static void aq_push(media *m, const uint8_t *d, uint32_t len, int64_t pts) {
  if (len > AQ_FRAME_MAX) return;
  pthread_mutex_lock(&m->lock);
  while (!m->quit && m->aq_count >= AQ_SIZE) pthread_cond_wait(&m->cond, &m->lock);
  if (!m->quit) {
    aframe *f = &m->aq[m->aq_tail];
    memcpy(f->data, d, len);
    f->len = len;
    f->pts = pts;
    m->aq_tail = (m->aq_tail + 1) % AQ_SIZE;
    m->aq_count++;
    pthread_cond_broadcast(&m->cond);
  }
  pthread_mutex_unlock(&m->lock);
}

/* Split a PES (or raw chunk) of ADTS frames into individual frames. */
static void push_adts(media *m, const uint8_t *d, uint32_t len, int64_t pts) {
  uint32_t off = 0;
  while (off + 7 <= len) {
    int fl = adts_len(d + off, len - off);
    if (fl < 0) { off++; continue; }
    if (off + fl > len) break;
    int rate = k_aac_rates[(d[off + 2] >> 2) & 0xF];
    if (pts == NO_PTS) pts = m->adts_next_pts;
    aq_push(m, d + off, fl, pts);
    pts += 1024LL * 90000 / (rate ? rate : 48000);
    m->adts_next_pts = pts;
    off += fl;
  }
}

static int audio_init(media *m, const uint8_t *adts) {
  int rate = k_aac_rates[(adts[2] >> 2) & 0xF];
  int ch = ((adts[2] & 1) << 2) | (adts[3] >> 6);
  if (ch < 1 || ch > 2) ch = 2;
  m->rate = rate ? rate : 48000;
  m->channels = ch;

  SceAudiodecInitParam ip;
  memset(&ip, 0, sizeof(ip));
  ip.size = sizeof(ip.aac);
  ip.aac.size = sizeof(ip.aac);
  ip.aac.totalStreams = 1;
  int ret = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &ip);
  if (ret < 0) { applog("sceAudiodecInitLibrary 0x%08X", (unsigned)ret); return -1; }
  m->adec_lib = 1;

  memset(&m->ainfo, 0, sizeof(m->ainfo));
  m->ainfo.size = sizeof(m->ainfo.aac);
  m->ainfo.aac.size = sizeof(m->ainfo.aac);
  m->ainfo.aac.isAdts = 1;
  m->ainfo.aac.ch = ch;
  m->ainfo.aac.samplingRate = m->rate;
  m->ainfo.aac.isSbr = 0;
  memset(&m->adec, 0, sizeof(m->adec));
  m->adec.size = sizeof(m->adec);
  m->adec.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
  m->adec.pInfo = &m->ainfo;
  ret = sceAudiodecCreateDecoder(&m->adec, SCE_AUDIODEC_TYPE_AAC);
  if (ret < 0) { applog("sceAudiodecCreateDecoder 0x%08X", (unsigned)ret); return -1; }
  m->adec_ok = 1;

  uint32_t es = SCE_AUDIODEC_ROUND_UP(m->adec.maxEsSize > AQ_FRAME_MAX ? m->adec.maxEsSize : AQ_FRAME_MAX);
  uint32_t pcm = SCE_AUDIODEC_ROUND_UP(m->adec.maxPcmSize ? m->adec.maxPcmSize : 2048 * 2 * 2);
  m->es_buf = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, es);
  m->pcm_buf = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, pcm);
  m->adec.maxPcmSize = pcm;

  /* MAIN, not BGM: the system keeps BGM ports quieter (it's meant for background music). */
  m->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, m->rate,
                                ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
  if (m->port < 0) { applog("sceAudioOutOpenPort 0x%08X", (unsigned)m->port); return -1; }
  int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
  sceAudioOutSetVolume(m->port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
  applog("audio: AAC %d Hz, %d ch", m->rate, ch);
  return 0;
}

/* Jellyfin's stereo downmix of 5.1 sources is quiet. Apply +7 dB with a soft knee so
 * peaks round off instead of clipping. */
static int16_t g_boost[65536];
static int g_boost_ready;

static void boost_init(void) {
  const float gain = 2.25f, knee = 0.6f;
  for (int i = 0; i < 65536; i++) {
    float x = (i - 32768) / 32768.0f * gain, a = fabsf(x), y;
    if (a <= knee) y = a;
    else y = knee + (1 - knee) * tanhf((a - knee) / (1 - knee));
    y = copysignf(fminf(y, 1.0f), x);
    g_boost[i] = (int16_t)(y * 32767);
  }
  g_boost_ready = 1;
}

static void boost(uint8_t *pcm, int n) {
  if (!g_boost_ready) boost_init();
  int16_t *s = (int16_t *)pcm;
  for (int i = 0; i < n; i++) s[i] = g_boost[(int)s[i] + 32768];
}

static void *audio_thread(void *arg) {
  media *m = arg;
  int grain = 1024;
  /* Wait until there's a first picture (or no video at all) so A/V start together. */
  for (;;) {
    pthread_mutex_lock(&m->lock);
    int have_video = m->audio_only || m->vframes > 0 || m->video_done || (m->pmt_pid >= 0 && m->video_pid < 0 && m->aq_count > 8);
    int ready = m->aq_count > 0 && (have_video || m->aq_count > AQ_SIZE / 2);
    int done = m->quit || (m->eof && m->aq_count == 0);
    pthread_mutex_unlock(&m->lock);
    if (done || ready) break;
    sceKernelDelayThread(5000);
  }

  while (!m->quit) {
    pthread_mutex_lock(&m->lock);
    while (!m->quit && (m->paused || m->aq_count == 0) && !(m->eof && m->aq_count == 0))
      pthread_cond_wait(&m->cond, &m->lock);
    if (m->quit || (m->aq_count == 0 && m->eof)) { pthread_mutex_unlock(&m->lock); break; }
    aframe f = m->aq[m->aq_head];  /* copy out; frees the queue slot */
    m->aq_head = (m->aq_head + 1) % AQ_SIZE;
    m->aq_count--;
    pthread_cond_broadcast(&m->cond);
    pthread_mutex_unlock(&m->lock);

    if (!m->adec_ok && audio_init(m, f.data) < 0) {
      /* no audio possible: fall back to wall clock */
      m->audio_done = 1;
      break;
    }
    memcpy(m->es_buf, f.data, f.len);
    m->adec.pEs = m->es_buf;
    m->adec.pPcm = m->pcm_buf;
    int ret = sceAudiodecDecode(&m->adec);
    if (ret < 0) {
      static int warned;
      if (warned++ < 8) applog("sceAudiodecDecode 0x%08X", (unsigned)ret);
      continue;
    }
    int samples = m->adec.outputPcmSize / (2 * m->channels);
    if (samples <= 0) continue;
    if (samples != grain) {
      sceAudioOutSetConfig(m->port, samples, m->rate, m->channels == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
      grain = samples;
    }
    boost(m->pcm_buf, samples * m->channels);
    sceAudioOutOutput(m->port, m->pcm_buf);
    /* When Output returns, the previous buffer finished: this frame is now audible. */
    pthread_mutex_lock(&m->lock);
    m->clock_pts = f.pts;
    m->clock_tick = now_us();
    m->clock_frame_us = (int64_t)samples * 1000000 / m->rate;
    if (!m->clock_started) {
      m->clock_started = 1;
      if (m->first_pts == NO_PTS) m->first_pts = f.pts;
    }
    pthread_mutex_unlock(&m->lock);
  }
  if (m->port >= 0 && m->adec_ok) sceAudioOutOutput(m->port, NULL);
  pthread_mutex_lock(&m->lock);
  m->audio_done = 1;
  pthread_mutex_unlock(&m->lock);
  return NULL;
}

/* Compressed video is queued so the demuxer never blocks on decoded-frame slots
 * (which only free up once audio has started the clock). */
static void vq_push(media *m, const uint8_t *d, uint32_t len, int64_t pts) {
  if (!m->vdec_ok || len == 0) return;
  uint8_t *copy = malloc(len + 64);
  if (!copy) return;
  memcpy(copy, d, len);
  memset(copy + len, 0, 64);
  pthread_mutex_lock(&m->lock);
  while (!m->quit && (m->vq_count >= VQ_SIZE || m->vq_bytes + len > VQ_BYTES)) pthread_cond_wait(&m->cond, &m->lock);
  if (m->quit) { pthread_mutex_unlock(&m->lock); free(copy); return; }
  vau *v = &m->vq[m->vq_tail];
  v->pts = pts;
  v->data = copy;
  v->len = len;
  m->vq_tail = (m->vq_tail + 1) % VQ_SIZE;
  m->vq_count++;
  m->vq_bytes += len;
  pthread_cond_broadcast(&m->cond);
  pthread_mutex_unlock(&m->lock);
}

static void *video_thread(void *arg) {
  media *m = arg;
  for (;;) {
    pthread_mutex_lock(&m->lock);
    while (!m->quit && m->vq_count == 0 && !m->eof) pthread_cond_wait(&m->cond, &m->lock);
    if (m->quit || (m->vq_count == 0 && m->eof)) { pthread_mutex_unlock(&m->lock); break; }
    vau v = m->vq[m->vq_head];
    m->vq_head = (m->vq_head + 1) % VQ_SIZE;
    m->vq_count--;
    m->vq_bytes -= v.len;
    pthread_cond_broadcast(&m->cond);
    pthread_mutex_unlock(&m->lock);
    decode_video_au(m, v.data, v.len, v.pts);
    free(v.data);
  }
  pthread_mutex_lock(&m->lock);
  m->video_done = 1;
  pthread_cond_broadcast(&m->cond);
  pthread_mutex_unlock(&m->lock);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* MPEG-TS demux                                                       */
/* ------------------------------------------------------------------ */

static void pes_append(pes_buf *p, const uint8_t *d, uint32_t n) {
  if (p->len + n + 64 > p->cap) {
    uint32_t nc = p->cap ? p->cap * 2 : 256 * 1024;
    while (nc < p->len + n + 64) nc *= 2;
    uint8_t *nb = realloc(p->buf, nc);
    if (!nb) return;
    p->buf = nb;
    p->cap = nc;
  }
  memcpy(p->buf + p->len, d, n);
  p->len += n;
}

static int64_t read_pts(const uint8_t *b) {
  return ((int64_t)((b[0] >> 1) & 7) << 30) | ((int64_t)b[1] << 22) | ((int64_t)(b[2] >> 1) << 15) |
         ((int64_t)b[3] << 7) | (b[4] >> 1);
}

static void flush_pes(media *m, pes_buf *p, int is_video) {
  if (!p->started || p->len < 9) { p->len = 0; return; }
  uint8_t *b = p->buf;
  p->started = 0;
  if (b[0] != 0 || b[1] != 0 || b[2] != 1) { p->len = 0; return; }
  int hlen = b[8];
  int64_t pts = NO_PTS;
  if ((b[7] & 0x80) && p->len >= 14) pts = read_pts(b + 9);
  uint32_t off = 9 + hlen;
  if (off >= p->len) { p->len = 0; return; }
  if (pts != NO_PTS) {
    pthread_mutex_lock(&m->lock);
    if (m->first_pts == NO_PTS || (!m->clock_started && pts < m->first_pts)) m->first_pts = pts;
    pthread_mutex_unlock(&m->lock);
  }
  if (is_video) {
    vq_push(m, b + off, p->len - off, pts);
  } else {
    push_adts(m, b + off, p->len - off, pts);
  }
  p->len = 0;
}

static void parse_psi(media *m, const uint8_t *p, int len, int pusi, int is_pat) {
  if (!pusi || len < 1) return;
  int ptr = p[0];
  const uint8_t *s = p + 1 + ptr;
  int avail = len - 1 - ptr;
  if (avail < 12) return;
  int sec_len = ((s[1] & 0x0F) << 8) | s[2];
  int end = 3 + sec_len - 4;
  if (end > avail) end = avail;
  if (is_pat && s[0] == 0x00) {
    for (int i = 8; i + 4 <= end; i += 4) {
      int prog = (s[i] << 8) | s[i + 1];
      if (prog != 0) { m->pmt_pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3]; break; }
    }
  } else if (!is_pat && s[0] == 0x02) {
    int pil = ((s[10] & 0x0F) << 8) | s[11];
    for (int i = 12 + pil; i + 5 <= end;) {
      int type = s[i], pid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
      int esl = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
      if (type == 0x1B && m->video_pid < 0) m->video_pid = pid;
      if (type == 0x0F && m->audio_pid < 0) m->audio_pid = pid;
      i += 5 + esl;
    }
    if (m->video_pid >= 0 || m->audio_pid >= 0)
      applog("PMT: video pid %d, audio pid %d", m->video_pid, m->audio_pid);
  }
}

static void ts_packet(media *m, const uint8_t *pk) {
  int pid = ((pk[1] & 0x1F) << 8) | pk[2];
  int pusi = pk[1] & 0x40;
  int afc = (pk[3] >> 4) & 3;
  int off = 4;
  if (afc & 2) off += 1 + pk[4];
  if (!(afc & 1) || off >= TS_PKT) return;
  const uint8_t *pl = pk + off;
  int plen = TS_PKT - off;

  if (pid == 0) { parse_psi(m, pl, plen, pusi, 1); return; }
  if (pid == m->pmt_pid) { parse_psi(m, pl, plen, pusi, 0); return; }
  pes_buf *p = pid == m->video_pid ? &m->vpes : pid == m->audio_pid ? &m->apes : NULL;
  if (!p) return;
  if (pusi) {
    flush_pes(m, p, p == &m->vpes);
    p->started = 1;
  }
  if (p->started) pes_append(p, pl, plen);
}

static void *demux_thread(void *arg) {
  media *m = arg;
  uint8_t *buf = malloc(READ_CHUNK + TS_PKT);
  uint32_t have = 0;
  int mode = 0; /* 0 = detect, 1 = TS, 2 = raw ADTS */

  m->stream = http_stream_open_live(m->url);
  if (!m->stream) { fail(m, "Could not connect to the server.", 0); goto out; }
  applog("stream opened (seekable=%d)", http_stream_seekable(m->stream));

  if (!m->audio_only && video_init(m) < 0) goto out;

  while (!m->quit) {
    int n = http_stream_read(m->stream, buf + have, m->read_pos, READ_CHUNK - have);
    if (n <= 0) {
      int ce = 0, hs = http_stream_status(m->stream, &ce);
      if (!m->quit) applog("stream ended at %llu bytes (HTTP %d, curl %d)", (unsigned long long)m->read_pos, hs, ce);
      if (!m->quit && m->read_pos == 0) {
        if (hs >= 400) {
          char msg[96];
          snprintf(msg, sizeof(msg), "The server could not stream this video (HTTP %d).", hs);
          fail(m, msg, 0);
        }
        else fail(m, "Could not connect to the server.", ce);
      }
      break;
    }
    m->read_pos += n;
    have += n;

    if (mode == 0 && have >= TS_PKT * 2) {
      if (buf[0] == 0x47 && buf[TS_PKT] == 0x47) mode = 1;
      else if (adts_len(buf, have) > 0) mode = 2;
      else {
        /* resync: search for TS sync */
        uint32_t i = 0;
        while (i + TS_PKT < have && !(buf[i] == 0x47 && buf[i + TS_PKT] == 0x47)) i++;
        if (i + TS_PKT < have) { memmove(buf, buf + i, have - i); have -= i; mode = 1; }
        else { fail(m, "The server sent data in an unexpected format.", 0); break; }
      }
      applog("container: %s", mode == 1 ? "MPEG-TS" : "ADTS");
    }

    if (mode == 1) {
      uint32_t i = 0;
      while (i + TS_PKT <= have) {
        if (buf[i] != 0x47) { i++; continue; }
        ts_packet(m, buf + i);
        i += TS_PKT;
        if (m->quit) break;
      }
      memmove(buf, buf + i, have - i);
      have -= i;
    } else if (mode == 2) {
      uint32_t i = 0;
      while (i + 7 <= have) {
        int fl = adts_len(buf + i, have - i);
        if (fl < 0) { i++; continue; }
        if (i + fl > have) break;
        push_adts(m, buf + i, fl, NO_PTS);
        i += fl;
      }
      memmove(buf, buf + i, have - i);
      have -= i;
    }
  }
  if (!m->quit) {
    flush_pes(m, &m->vpes, 1);
    flush_pes(m, &m->apes, 0);
  }
out:
  free(buf);
  pthread_mutex_lock(&m->lock);
  m->eof = 1;
  pthread_cond_broadcast(&m->cond);
  pthread_mutex_unlock(&m->lock);
  applog("demux finished (%llu bytes, %d frames)", (unsigned long long)m->read_pos, m->vframes);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static pthread_t spawn(void *(*fn)(void *), void *arg, int stack) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, stack);
  pthread_create(&t, &a, fn, arg);
  pthread_attr_destroy(&a);
  return t;
}

media *media_open(const char *url, int audio_only) {
  media *m = calloc(1, sizeof(*m));
  m->url = strdup(url);
  m->audio_only = audio_only;
  m->pmt_pid = m->video_pid = m->audio_pid = -1;
  m->first_pts = NO_PTS;
  m->port = -1;
  m->aq = malloc(sizeof(aframe) * AQ_SIZE);
  pthread_mutex_init(&m->lock, NULL);
  pthread_cond_init(&m->cond, NULL);
  applog("media_open %s", audio_only ? "(audio)" : "(video)");
  m->demux_th = spawn(demux_thread, m, 256 * 1024);
  m->demux_started = 1;
  m->audio_th = spawn(audio_thread, m, 64 * 1024);
  m->audio_started = 1;
  m->video_th = spawn(video_thread, m, 64 * 1024);
  m->video_started = 1;
  return m;
}

const char *media_error(media *m) { return m->error; }

static int64_t clock90(media *m) {
  /* caller holds lock */
  if (m->clock_started && !m->audio_done) {
    uint64_t t = m->paused ? m->pause_tick : now_us();
    int64_t el = (int64_t)(t - m->clock_tick);
    if (el > m->clock_frame_us) el = m->clock_frame_us;  /* starved: clock stops */
    if (el < 0) el = 0;
    return m->clock_pts + el * 9 / 100;
  }
  if (m->wall_base_tick) {
    uint64_t t = m->paused ? m->pause_tick : now_us();
    return m->wall_base_pts + (int64_t)(t - m->wall_base_tick) * 9 / 100;
  }
  return NO_PTS;
}

int64_t media_clock_ms(media *m) {
  pthread_mutex_lock(&m->lock);
  int64_t c = clock90(m);
  int64_t base = m->first_pts;
  pthread_mutex_unlock(&m->lock);
  /* after the audio drains the clock goes away; keep reporting the last position */
  if (c == NO_PTS || base == NO_PTS) return m->last_clock_ms;
  int64_t ms = (c - base) / 90;
  if (ms < 0) ms = 0;
  m->last_clock_ms = ms;
  return ms;
}

int media_state(media *m) {
  if (m->state == MEDIA_ERROR) return MEDIA_ERROR;
  pthread_mutex_lock(&m->lock);
  int ready = 0;
  for (int i = 0; i < NUM_SLOTS; i++) if (m->slots[i].state == SLOT_READY) ready++;
  int playing = m->clock_started || m->wall_base_tick;
  int ended = m->eof && m->audio_done && (m->video_done || m->audio_only) && !ready && m->aq_count == 0;
  int starving = m->clock_started && !m->paused && !m->audio_done &&
                 now_us() - m->clock_tick > (uint64_t)m->clock_frame_us + 250000;
  int nothing = m->eof && m->audio_done && (m->video_done || m->audio_only) && m->vframes == 0 && !m->clock_started;
  pthread_mutex_unlock(&m->lock);
  if (nothing) { fail(m, "This video cannot be played.", 0); return MEDIA_ERROR; }
  if (ended && (playing || m->vframes)) return MEDIA_ENDED;
  if (!playing || starving) return MEDIA_BUFFERING;
  return MEDIA_PLAYING;
}

vita2d_texture *media_video_frame(media *m, int *w, int *h) {
  unsigned frame = ui_frame();
  vita2d_texture *out = NULL;
  pthread_mutex_lock(&m->lock);
  int freed = 0;
  for (int i = 0; i < NUM_SLOTS; i++)
    if (m->slots[i].state == SLOT_RETIRED && frame - m->slots[i].retired_at >= 2) { m->slots[i].state = SLOT_FREE; freed = 1; }

  /* video-only stream: start a wall clock at the first frame */
  /* no usable audio track: run the picture off a wall clock from the first frame */
  int no_audio = m->audio_pid < 0 || (m->audio_done && !m->clock_started);
  if (!m->audio_only && !m->clock_started && !m->wall_base_tick && no_audio && m->pmt_pid >= 0) {
    for (int i = 0; i < NUM_SLOTS; i++)
      if (m->slots[i].state == SLOT_READY) {
        m->wall_base_pts = m->slots[i].pts;
        m->wall_base_tick = now_us();
        break;
      }
  }

  int64_t clk = clock90(m);
  slot *shown = NULL, *pick = NULL;
  for (int i = 0; i < NUM_SLOTS; i++) {
    slot *s = &m->slots[i];
    if (s->state == SLOT_SHOWN) shown = s;
    if (s->state != SLOT_READY) continue;
    if (clk == NO_PTS) {
      /* not started yet: show the earliest frame as a poster */
      if (!shown && (!pick || s->pts < pick->pts)) pick = s;
    } else if (s->pts <= clk + 1500 /* ~16 ms early is fine */) {
      if (!pick || s->pts > pick->pts) pick = s;
    }
  }
  if (pick) {
    /* drop frames older than the one we're showing */
    for (int i = 0; i < NUM_SLOTS; i++) {
      slot *s = &m->slots[i];
      if (s != pick && s->state == SLOT_READY && s->pts < pick->pts && clk != NO_PTS) { s->state = SLOT_FREE; freed = 1; }
    }
    if (shown) { shown->state = SLOT_RETIRED; shown->retired_at = frame; }
    pick->state = SLOT_SHOWN;
    shown = pick;
  }
  if (shown) {
    out = shown->tex;
    if (w) *w = shown->w;
    if (h) *h = shown->h;
  }
  if (freed) pthread_cond_broadcast(&m->cond);
  pthread_mutex_unlock(&m->lock);
  return out;
}

void media_set_paused(media *m, int paused) {
  pthread_mutex_lock(&m->lock);
  if (paused && !m->paused) m->pause_tick = now_us();
  if (!paused && m->paused) {
    uint64_t d = now_us() - m->pause_tick;
    m->clock_tick += d;
    if (m->wall_base_tick) m->wall_base_tick += d;
  }
  m->paused = paused;
  pthread_cond_broadcast(&m->cond);
  pthread_mutex_unlock(&m->lock);
}

void media_close(media *m) {
  if (!m) return;
  pthread_mutex_lock(&m->lock);
  m->quit = 1;
  pthread_cond_broadcast(&m->cond);
  pthread_mutex_unlock(&m->lock);
  /* the stream pointer is set by the demux thread; abort it if it exists yet */
  for (int i = 0; i < 100 && !m->stream && !m->eof; i++) sceKernelDelayThread(10000);
  if (m->stream) http_stream_abort(m->stream);
  if (m->demux_started) pthread_join(m->demux_th, NULL);
  if (m->audio_started) pthread_join(m->audio_th, NULL);
  if (m->video_started) pthread_join(m->video_th, NULL);
  while (m->vq_count > 0) { free(m->vq[m->vq_head].data); m->vq_head = (m->vq_head + 1) % VQ_SIZE; m->vq_count--; }

  if (m->vdec_ok) sceAvcdecDeleteDecoder(&m->vdec);
  if (m->vdec_mem > 0) sceKernelFreeMemBlock(m->vdec_mem);
  if (m->vdec_lib) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
  if (m->adec_ok) sceAudiodecDeleteDecoder(&m->adec);
  if (m->adec_lib) sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
  if (m->port >= 0) sceAudioOutReleasePort(m->port);

  vita2d_wait_rendering_done();
  for (int i = 0; i < NUM_SLOTS; i++) if (m->slots[i].tex) vita2d_free_texture(m->slots[i].tex);
  if (m->stream) http_stream_close(m->stream);
  free(m->vpes.buf);
  free(m->apes.buf);
  free(m->es_buf);
  free(m->pcm_buf);
  free(m->aq);
  free(m->url);
  pthread_mutex_destroy(&m->lock);
  pthread_cond_destroy(&m->cond);
  free(m);
  applog("media closed");
}
