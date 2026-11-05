#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct { const char *p; } jp;

static void ws(jp *s) { while (*s->p && isspace((unsigned char)*s->p)) s->p++; }

static jval *jnew(jtype t) {
  jval *v = calloc(1, sizeof(jval));
  if (v) v->type = t;
  return v;
}

static void put_utf8(char **o, unsigned cp) {
  char *d = *o;
  if (cp < 0x80) *d++ = cp;
  else if (cp < 0x800) { *d++ = 0xC0 | (cp >> 6); *d++ = 0x80 | (cp & 0x3F); }
  else if (cp < 0x10000) { *d++ = 0xE0 | (cp >> 12); *d++ = 0x80 | ((cp >> 6) & 0x3F); *d++ = 0x80 | (cp & 0x3F); }
  else { *d++ = 0xF0 | (cp >> 18); *d++ = 0x80 | ((cp >> 12) & 0x3F); *d++ = 0x80 | ((cp >> 6) & 0x3F); *d++ = 0x80 | (cp & 0x3F); }
  *o = d;
}

static unsigned hex4(const char *p) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i]; v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return 0xFFFFFFFF;
  }
  return v;
}

static char *pstr(jp *s) {
  if (*s->p != '"') return NULL;
  s->p++;
  const char *start = s->p;
  size_t n = 0;
  while (start[n] && start[n] != '"') { if (start[n] == '\\' && start[n + 1]) n++; n++; }
  char *out = malloc(n + 1), *o = out;
  if (!out) return NULL;
  while (*s->p && *s->p != '"') {
    char c = *s->p++;
    if (c != '\\') { *o++ = c; continue; }
    c = *s->p++;
    switch (c) {
      case 'n': *o++ = '\n'; break;
      case 't': *o++ = '\t'; break;
      case 'r': *o++ = '\r'; break;
      case 'b': *o++ = '\b'; break;
      case 'f': *o++ = '\f'; break;
      case 'u': {
        unsigned cp = hex4(s->p);
        if (cp == 0xFFFFFFFF) break;
        s->p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && s->p[0] == '\\' && s->p[1] == 'u') {
          unsigned lo = hex4(s->p + 2);
          if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); s->p += 6; }
        }
        put_utf8(&o, cp);
        break;
      }
      default: if (c) *o++ = c; else s->p--; break;
    }
  }
  if (*s->p == '"') s->p++;
  *o = 0;
  return out;
}

static jval *pval(jp *s, int depth);

static jval *pval(jp *s, int depth) {
  if (depth > 64) return NULL;
  ws(s);
  char c = *s->p;
  if (c == '{' || c == '[') {
    int obj = c == '{';
    jval *v = jnew(obj ? J_OBJ : J_ARR), *last = NULL;
    s->p++;
    for (;;) {
      ws(s);
      if (*s->p == (obj ? '}' : ']')) { s->p++; break; }
      char *key = NULL;
      if (obj) {
        key = pstr(s);
        if (!key) { json_free(v); return NULL; }
        ws(s);
        if (*s->p != ':') { free(key); json_free(v); return NULL; }
        s->p++;
      }
      jval *ch = pval(s, depth + 1);
      if (!ch) { free(key); json_free(v); return NULL; }
      ch->key = key;
      if (last) last->next = ch; else v->child = ch;
      last = ch;
      v->count++;
      ws(s);
      if (*s->p == ',') { s->p++; continue; }
      if (*s->p == (obj ? '}' : ']')) { s->p++; break; }
      json_free(v);
      return NULL;
    }
    return v;
  }
  if (c == '"') { jval *v = jnew(J_STR); v->str = pstr(s); return v; }
  if (!strncmp(s->p, "true", 4)) { s->p += 4; jval *v = jnew(J_BOOL); v->num = 1; return v; }
  if (!strncmp(s->p, "false", 5)) { s->p += 5; return jnew(J_BOOL); }
  if (!strncmp(s->p, "null", 4)) { s->p += 4; return jnew(J_NULL); }
  if (c == '-' || (c >= '0' && c <= '9')) {
    char *end;
    jval *v = jnew(J_NUM);
    v->num = strtod(s->p, &end);
    s->p = end;
    return v;
  }
  return NULL;
}

jval *json_parse(const char *text) {
  if (!text) return NULL;
  jp s = { text };
  return pval(&s, 0);
}

void json_free(jval *v) {
  while (v) {
    jval *n = v->next;
    json_free(v->child);
    free(v->key);
    free(v->str);
    free(v);
    v = n;
  }
}

jval *json_get(const jval *obj, const char *key) {
  if (!obj || obj->type != J_OBJ) return NULL;
  for (jval *c = obj->child; c; c = c->next)
    if (c->key && !strcmp(c->key, key)) return c;
  return NULL;
}

jval *json_at(const jval *arr, int idx) {
  if (!arr || arr->type != J_ARR) return NULL;
  jval *c = arr->child;
  while (c && idx-- > 0) c = c->next;
  return c;
}

const char *json_str(const jval *obj, const char *key, const char *def) {
  jval *v = json_get(obj, key);
  return (v && v->type == J_STR && v->str) ? v->str : def;
}

double json_num(const jval *obj, const char *key, double def) {
  jval *v = json_get(obj, key);
  return (v && (v->type == J_NUM || v->type == J_BOOL)) ? v->num : def;
}

int json_bool(const jval *obj, const char *key, int def) {
  jval *v = json_get(obj, key);
  return (v && v->type == J_BOOL) ? (int)v->num : def;
}

char *json_escape(const char *s) {
  size_t n = 0;
  for (const char *p = s; *p; p++) n += (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) ? 6 : 1;
  char *o = malloc(n + 1), *d = o;
  for (const char *p = s; *p; p++) {
    unsigned char c = *p;
    if (c == '"' || c == '\\') { *d++ = '\\'; *d++ = c; }
    else if (c < 0x20) { static const char hx[] = "0123456789abcdef"; *d++ = '\\'; *d++ = 'u'; *d++ = '0'; *d++ = '0'; *d++ = hx[c >> 4]; *d++ = hx[c & 15]; }
    else *d++ = c;
  }
  *d = 0;
  return o;
}
