#include "ime.h"
#include <psp2/ime_dialog.h>
#include <psp2/message_dialog.h>
#include <psp2/common_dialog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IME_MAX 256

static uint16_t g_title[128];
static uint16_t g_init[IME_MAX + 1];
static uint16_t g_buf[IME_MAX + 1];
static int g_ime_open, g_msg_open;
static char g_msg_text[512];

static void to_utf16(uint16_t *dst, int cap, const char *s) {
  int n = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p && n < cap - 2) {
    unsigned cp;
    if (*p < 0x80) cp = *p++;
    else if ((*p & 0xE0) == 0xC0) { cp = (*p++ & 0x1F) << 6; cp |= *p ? (*p++ & 0x3F) : 0; }
    else if ((*p & 0xF0) == 0xE0) { cp = (*p++ & 0x0F) << 12; for (int i = 1; i >= 0 && *p; i--) cp |= (*p++ & 0x3F) << (6 * i); }
    else { cp = (*p++ & 0x07) << 18; for (int i = 2; i >= 0 && *p; i--) cp |= (*p++ & 0x3F) << (6 * i); }
    if (cp >= 0x10000) { cp -= 0x10000; dst[n++] = 0xD800 | (cp >> 10); dst[n++] = 0xDC00 | (cp & 0x3FF); }
    else dst[n++] = cp;
  }
  dst[n] = 0;
}

static void to_utf8(char *dst, int cap, const uint16_t *s) {
  int n = 0;
  for (; *s && n < cap - 5; s++) {
    unsigned cp = *s;
    if (cp >= 0xD800 && cp < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[1] - 0xDC00); s++; }
    if (cp < 0x80) dst[n++] = cp;
    else if (cp < 0x800) { dst[n++] = 0xC0 | (cp >> 6); dst[n++] = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { dst[n++] = 0xE0 | (cp >> 12); dst[n++] = 0x80 | ((cp >> 6) & 0x3F); dst[n++] = 0x80 | (cp & 0x3F); }
    else { dst[n++] = 0xF0 | (cp >> 18); dst[n++] = 0x80 | ((cp >> 12) & 0x3F); dst[n++] = 0x80 | ((cp >> 6) & 0x3F); dst[n++] = 0x80 | (cp & 0x3F); }
  }
  dst[n] = 0;
}

int ime_open(const char *title, const char *initial, int max_len, int kind) {
  if (g_ime_open || g_msg_open) return -1;
  if (max_len > IME_MAX) max_len = IME_MAX;
  to_utf16(g_title, 128, title);
  to_utf16(g_init, IME_MAX + 1, initial ? initial : "");
  memset(g_buf, 0, sizeof(g_buf));

  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  p.supportedLanguages = 0;  /* system language */
  p.languagesForced = SCE_FALSE;
  p.type = kind == IME_URL ? SCE_IME_TYPE_URL : SCE_IME_TYPE_BASIC_LATIN;
  p.option = 0;
  p.textBoxMode = kind == IME_PASSWORD ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
  p.title = g_title;
  p.maxTextLength = max_len;
  p.initialText = g_init;
  p.inputTextBuffer = g_buf;
  if (sceImeDialogInit(&p) < 0) return -1;
  g_ime_open = 1;
  return 0;
}

int ime_poll(char *out, int out_len) {
  if (!g_ime_open) return -1;
  if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
  SceImeDialogResult r;
  memset(&r, 0, sizeof(r));
  sceImeDialogGetResult(&r);
  int ok = r.button == SCE_IME_DIALOG_BUTTON_ENTER;
  if (ok) to_utf8(out, out_len, g_buf);
  sceImeDialogTerm();
  g_ime_open = 0;
  return ok ? 1 : -1;
}

int msg_open(const char *text, int yes_no) {
  if (g_ime_open || g_msg_open) return -1;
  snprintf(g_msg_text, sizeof(g_msg_text), "%s", text);
  SceMsgDialogUserMessageParam up;
  memset(&up, 0, sizeof(up));
  up.buttonType = yes_no ? SCE_MSG_DIALOG_BUTTON_TYPE_YESNO : SCE_MSG_DIALOG_BUTTON_TYPE_OK;
  up.msg = (SceChar8 *)g_msg_text;
  SceMsgDialogParam p;
  sceMsgDialogParamInit(&p);
  p.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
  p.userMsgParam = &up;
  if (sceMsgDialogInit(&p) < 0) return -1;
  g_msg_open = 1;
  return 0;
}

int msg_poll(void) {
  if (!g_msg_open) return -1;
  if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
  SceMsgDialogResult r;
  memset(&r, 0, sizeof(r));
  sceMsgDialogGetResult(&r);
  sceMsgDialogTerm();
  g_msg_open = 0;
  return (r.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES || r.buttonId == SCE_MSG_DIALOG_BUTTON_ID_OK) ? 1 : -1;
}

int dialog_active(void) { return g_ime_open || g_msg_open; }
