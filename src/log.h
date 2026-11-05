#ifndef LOG_H
#define LOG_H

/* Appends to ux0:data/VitaFin/log.txt (truncated at each launch). */
void log_init(void);
void applog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
