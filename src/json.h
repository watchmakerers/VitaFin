#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
  jtype type;
  char *key;          /* set when this value is an object member */
  char *str;          /* J_STR */
  double num;         /* J_NUM / J_BOOL */
  struct jval *child; /* first child (J_ARR / J_OBJ) */
  struct jval *next;  /* next sibling */
  int count;          /* number of children */
} jval;

jval *json_parse(const char *text);
void json_free(jval *v);

jval *json_get(const jval *obj, const char *key);
jval *json_at(const jval *arr, int idx);
const char *json_str(const jval *obj, const char *key, const char *def);
double json_num(const jval *obj, const char *key, double def);
int json_bool(const jval *obj, const char *key, int def);

/* Escape a string for embedding in JSON. Caller frees. */
char *json_escape(const char *s);

#endif
