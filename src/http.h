#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *data;   /* NUL-terminated for convenience */
  size_t size;
  long status;  /* HTTP status, 0 on transport failure */
  char error[128];
} http_resp;

void http_global_init(void);
void http_global_cleanup(void);

/* Extra header line added to every request (e.g. Jellyfin auth). */
void http_set_auth_header(const char *line);

/* method: "GET", "POST", "DELETE". body may be NULL. */
int http_request(const char *method, const char *url, const char *json_body, http_resp *out);
void http_resp_free(http_resp *r);

/* --- Ranged streaming reader used by the media player --- */
typedef struct http_stream http_stream;

http_stream *http_stream_open(const char *url);
/* Sequential stream (live transcode): no range probe, straight into the ring buffer. */
http_stream *http_stream_open_live(const char *url);
/* Total size; 0 if unknown (server didn't report length). */
uint64_t http_stream_size(http_stream *s);
int http_stream_seekable(http_stream *s);
/* HTTP status of the (live) transfer and curl error code, once known. */
int http_stream_status(http_stream *s, int *curl_error);
/* Read len bytes at pos. Returns bytes read, or <0 on error. */
int http_stream_read(http_stream *s, uint8_t *buf, uint64_t pos, uint32_t len);
/* Make all pending/future reads fail fast (call before tearing down the consumer). */
void http_stream_abort(http_stream *s);
void http_stream_close(http_stream *s);

#endif
