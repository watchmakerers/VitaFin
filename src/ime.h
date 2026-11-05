#ifndef IME_H
#define IME_H

/* System on-screen keyboard (SceImeDialog). */
#define IME_TEXT 0
#define IME_URL 1
#define IME_PASSWORD 2
int ime_open(const char *title, const char *initial, int max_len, int kind);
/* 0 = still open, 1 = confirmed (out filled), -1 = cancelled */
int ime_poll(char *out, int out_len);

/* System message dialog (SceMsgDialog). */
int msg_open(const char *text, int yes_no);
/* 0 = still open, 1 = OK/Yes, -1 = No/cancel */
int msg_poll(void);

int dialog_active(void);

#endif
