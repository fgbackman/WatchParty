/* wp_json.h — tiny JSON reader/writer for flat message objects.
 *
 * Messages exchanged between peers are flat objects whose values are
 * strings, numbers, booleans or null. Nested arrays/objects are tolerated
 * (skipped) so an unknown field never breaks parsing. Input is untrusted:
 * every path is bounds-checked and strings are validated as UTF-8. */
#ifndef WP_JSON_H
#define WP_JSON_H

#include <stddef.h>

#define WPJ_MAX_FIELDS 24
#define WPJ_KEY_MAX    32
#define WPJ_ARENA      (64 * 1024 + 64)   /* >= WP_MAX_FRAME: any accepted frame parses */

typedef enum { WPJ_STR, WPJ_NUM, WPJ_BOOL, WPJ_NULL, WPJ_OTHER } wpj_type;

typedef struct {
    char key[WPJ_KEY_MAX];
    wpj_type type;
    const char *str;   /* WPJ_STR: points into arena */
    double num;        /* WPJ_NUM, WPJ_BOOL (0/1) */
} wpj_field;

typedef struct {
    int n;
    wpj_field f[WPJ_MAX_FIELDS];
    char arena[WPJ_ARENA];
    size_t used;
} wpj_obj;

/* Parse `text` into `o`. Returns 1 on success, 0 on malformed input. */
int wpj_parse(const char *text, wpj_obj *o);

const char *wpj_str(const wpj_obj *o, const char *key);              /* NULL if absent */
double      wpj_num(const wpj_obj *o, const char *key, double dflt);
int         wpj_bool(const wpj_obj *o, const char *key, int dflt);
int         wpj_has(const wpj_obj *o, const char *key);

/* Writer: builds "{...}" in a growable buffer. */
typedef struct {
    char *buf;
    size_t len, cap;
    int count;
    int err;
} wpj_w;

void        wpj_w_init(wpj_w *w);
void        wpj_w_str(wpj_w *w, const char *key, const char *val);
void        wpj_w_num(wpj_w *w, const char *key, double v);      /* 3 decimals */
void        wpj_w_int(wpj_w *w, const char *key, long long v);
void        wpj_w_bool(wpj_w *w, const char *key, int v);
const char *wpj_w_end(wpj_w *w);   /* closes the object, returns buffer (owned by w) */
void        wpj_w_free(wpj_w *w);

#endif
