#include "log.h"
#include "config.h"
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

static FILE *g_log;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(void) {
  g_log = fopen(DATA_DIR "/log.txt", "w");
}

void applog(const char *fmt, ...) {
  if (!g_log) return;
  pthread_mutex_lock(&g_lock);
  unsigned ms = (unsigned)(sceKernelGetProcessTimeWide() / 1000);
  fprintf(g_log, "[%6u.%03u] ", ms / 1000, ms % 1000);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
  pthread_mutex_unlock(&g_lock);
}
