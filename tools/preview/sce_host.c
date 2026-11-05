#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/rtc.h>
#include <psp2/net/netctl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
unsigned host_buttons; int host_touch, host_tx, host_ty;
unsigned long long host_sim_us = 1000000; unsigned long long sceKernelGetProcessTimeWide(void) { return host_sim_us; }
int sceCtrlSetSamplingMode(SceCtrlPadInputMode m) { return 0; }
int sceCtrlPeekBufferPositive(int p, SceCtrlData *d, int n) { memset(d, 0, sizeof(*d)); d->buttons = host_buttons; d->lx = d->ly = d->rx = d->ry = 128; return 1; }
int sceTouchSetSamplingState(SceUInt32 p, SceTouchSamplingState s) { return 0; }
int sceTouchEnableTouchForce(SceUInt32 p) { return 0; }
int sceTouchPeek(SceUInt32 p, SceTouchData *d, SceUInt32 n) { memset(d, 0, sizeof(*d)); if (host_touch) { d->reportNum = 1; d->report[0].x = host_tx * 2; d->report[0].y = host_ty * 2; } return 1; }
int sceAppUtilSystemParamGetInt(unsigned int id, int *v) { *v = 1; return 0; }
int scePowerGetBatteryLifePercent(void) { return 78; }
int scePowerIsBatteryCharging(void) { return 0; }
int sceNetCtlInetGetState(int *s) { *s = SCE_NETCTL_STATE_CONNECTED; return 0; }
int sceNetCtlInetGetInfo(int c, SceNetCtlInfo *i) { i->rssi_percentage = 80; return 0; }
int sceRtcGetCurrentClockLocalTime(SceDateTime *t) { memset(t, 0, sizeof(*t)); t->hour = 11; t->minute = 51; return 0; }
int sceRtcGetCurrentTick(SceRtcTick *t) { t->tick = 1; return 0; }
int sceIoMkdir(const char *p, int m) { return 0; }
/* ime / msg / player / log */
int ime_open(const char *a, const char *b, int c, int d) { return -1; }
int ime_poll(char *o, int l) { return -1; }
int msg_open(const char *t, int y) { printf("[msg] %s\n", t); return 0; }
int msg_poll(void) { return 1; }
int dialog_active(void) { return 0; }
void player_start(const void *it, long long r) {}
int player_frame(void) { return 0; }
void applog(const char *f, ...) { va_list a; va_start(a, f); printf("[log] "); vprintf(f, a); printf("\n"); va_end(a); }
#include <psp2/audioout.h>
int sceAudioOutOpenPort(SceAudioOutPortType t, int l, int f, SceAudioOutMode m) { return 1; }
int sceAudioOutSetVolume(int p, SceAudioOutChannelFlag c, int *v) { return 0; }
int sceAudioOutOutput(int p, const void *b) { struct timespec ts = { 0, 5333333 }; nanosleep(&ts, NULL); return 0; }
int sceKernelDelayThread(unsigned us) { struct timespec ts = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&ts, NULL); return 0; }
