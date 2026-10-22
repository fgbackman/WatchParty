/* wp_json.c — see wp_json.h */
#include "wp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ parsing */

typedef struct { const char *p; wpj_obj *o; } P;

static void skip_ws(P *s)
{
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') s->p++;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static int arena_put(wpj_obj *o, char c)
{
    if (o->used + 1 >= WPJ_ARENA) return 0;
    o->arena[o->used++] = c;
    return 1;
}

static int put_utf8(wpj_obj *o, unsigned cp)
{
    if (cp < 0x80) return arena_put(o, (char)cp);
    if (cp < 0x800) return arena_put(o, (char)(0xC0 | (cp >> 6))) && arena_put(o, (char)(0x80 | (cp & 0x3F)));
    if (cp < 0x10000) return arena_put(o, (char)(0xE0 | (cp >> 12))) && arena_put(o, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
                             arena_put(o, (char)(0x80 | (cp & 0x3F)));
    return arena_put(o, (char)(0xF0 | (cp >> 18))) && arena_put(o, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
           arena_put(o, (char)(0x80 | ((cp >> 6) & 0x3F))) && arena_put(o, (char)(0x80 | (cp & 0x3F)));
}

/* Length of a valid UTF-8 sequence starting at p (1..4), or 0 if invalid. */
static int utf8_len(const unsigned char *p)
{
    unsigned char c = p[0];
    if (c < 0x80) return 1;
    int n; unsigned cp;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; if (c < 0xC2) return 0; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; if (c > 0xF4) return 0; }
    else return 0;
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
    if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
    return n;
}

/* Parses a JSON string starting at the opening quote; writes the unescaped
 * text into the arena (NUL-terminated). Returns pointer to it, or NULL. */
static const char *parse_string(P *s)
{
    if (*s->p != '"') return NULL;
    s->p++;
    size_t start = s->o->used;
    for (;;) {
        unsigned char c = (unsigned char)*s->p;
        if (c == 0) return NULL;
        if (c == '"') { s->p++; break; }
        if (c == '\\') {
            s->p++;
            char e = *s->p;
            unsigned cp;
            switch (e) {
            case '"':  if (!arena_put(s->o, '"')) return NULL; s->p++; break;
            case '\\': if (!arena_put(s->o, '\\')) return NULL; s->p++; break;
            case '/':  if (!arena_put(s->o, '/')) return NULL; s->p++; break;
            case 'b':  if (!arena_put(s->o, '\b')) return NULL; s->p++; break;
            case 'f':  if (!arena_put(s->o, '\f')) return NULL; s->p++; break;
            case 'n':  if (!arena_put(s->o, '\n')) return NULL; s->p++; break;
            case 'r':  if (!arena_put(s->o, '\r')) return NULL; s->p++; break;
            case 't':  if (!arena_put(s->o, '\t')) return NULL; s->p++; break;
            case 'u':
                if (!hex4(s->p + 1, &cp)) return NULL;
                s->p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF) {            /* surrogate pair */
                    unsigned lo;
                    if (s->p[0] == '\\' && s->p[1] == 'u' && hex4(s->p + 2, &lo) &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        s->p += 6;
                    } else cp = 0xFFFD;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
                if (cp == 0) cp = 0xFFFD;                     /* no embedded NULs */
                if (!put_utf8(s->o, cp)) return NULL;
                break;
            default: return NULL;
            }
            continue;
        }
        if (c < 0x20) return NULL;                            /* control chars must be escaped */
        int n = utf8_len((const unsigned char *)s->p);
        if (n == 0) { if (!arena_put(s->o, '?')) return NULL; s->p++; continue; }
        for (int i = 0; i < n; i++) if (!arena_put(s->o, s->p[i])) return NULL;
        s->p += n;
    }
    if (!arena_put(s->o, 0)) return NULL;
    return s->o->arena + start;
}

/* Skip a nested array/object (bracket matching, string-aware). */
static int skip_nested(P *s)
{
    int depth = 0;
    for (;;) {
        char c = *s->p;
        if (c == 0) return 0;
        if (c == '"') {
            s->p++;
            while (*s->p && *s->p != '"') { if (*s->p == '\\' && s->p[1]) s->p++; s->p++; }
            if (*s->p != '"') return 0;
            s->p++;
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { depth--; if (depth == 0) { s->p++; return 1; } if (depth < 0) return 0; }
        s->p++;
    }
}

static int parse_value(P *s, wpj_field *f)
{
    char c = *s->p;
    if (c == '"') {
        const char *str = parse_string(s);
        if (!str) return 0;
        if (f) { f->type = WPJ_STR; f->str = str; }
        return 1;
    }
    if (c == '{' || c == '[') {
        if (!skip_nested(s)) return 0;
        if (f) { f->type = WPJ_OTHER; }
        return 1;
    }
    if (strncmp(s->p, "true", 4) == 0)  { s->p += 4; if (f) { f->type = WPJ_BOOL; f->num = 1; } return 1; }
    if (strncmp(s->p, "false", 5) == 0) { s->p += 5; if (f) { f->type = WPJ_BOOL; f->num = 0; } return 1; }
    if (strncmp(s->p, "null", 4) == 0)  { s->p += 4; if (f) { f->type = WPJ_NULL; } return 1; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end = NULL;
        double v = strtod(s->p, &end);
        if (!end || end == s->p) return 0;
        s->p = end;
        if (f) { f->type = WPJ_NUM; f->num = v; }
        return 1;
    }
    return 0;
}

int wpj_parse(const char *text, wpj_obj *o)
{
    if (!text || !o) return 0;
    memset(o, 0, sizeof *o);
    P s = { text, o };
    skip_ws(&s);
    if (*s.p != '{') return 0;
    s.p++;
    skip_ws(&s);
    if (*s.p == '}') { s.p++; skip_ws(&s); return *s.p == 0; }
    for (;;) {
        skip_ws(&s);
        size_t mark = o->used;
        const char *key = parse_string(&s);
        if (!key) return 0;
        skip_ws(&s);
        if (*s.p != ':') return 0;
        s.p++;
        skip_ws(&s);
        wpj_field tmp; memset(&tmp, 0, sizeof tmp);
        wpj_field *dst = (o->n < WPJ_MAX_FIELDS) ? &o->f[o->n] : &tmp;
        size_t klen = strlen(key);
        if (klen >= WPJ_KEY_MAX) klen = WPJ_KEY_MAX - 1;
        memcpy(dst->key, key, klen);
        dst->key[klen] = 0;
        o->used = mark;                       /* key text no longer needed in arena */
        /* but the key bytes may be overwritten by the value: copy done above */
        if (!parse_value(&s, dst)) return 0;
        if (dst != &tmp) o->n++;
        skip_ws(&s);
        if (*s.p == ',') { s.p++; continue; }
        if (*s.p == '}') { s.p++; break; }
        return 0;
    }
    skip_ws(&s);
    return *s.p == 0;
}

static const wpj_field *find(const wpj_obj *o, const char *key)
{
    if (!o || !key) return NULL;
    for (int i = 0; i < o->n; i++) if (strcmp(o->f[i].key, key) == 0) return &o->f[i];
    return NULL;
}

const char *wpj_str(const wpj_obj *o, const char *key)
{
    const wpj_field *f = find(o, key);
    return (f && f->type == WPJ_STR) ? f->str : NULL;
}

double wpj_num(const wpj_obj *o, const char *key, double dflt)
{
    const wpj_field *f = find(o, key);
    return (f && (f->type == WPJ_NUM || f->type == WPJ_BOOL)) ? f->num : dflt;
}

int wpj_bool(const wpj_obj *o, const char *key, int dflt)
{
    const wpj_field *f = find(o, key);
    if (!f) return dflt;
    if (f->type == WPJ_BOOL || f->type == WPJ_NUM) return f->num != 0;
    return dflt;
}

int wpj_has(const wpj_obj *o, const char *key) { return find(o, key) != NULL; }

/* ------------------------------------------------------------ writing */

static int w_reserve(wpj_w *w, size_t extra)
{
    if (w->err) return 0;
    if (w->len + extra + 1 <= w->cap) return 1;
    size_t ncap = w->cap ? w->cap * 2 : 256;
    while (ncap < w->len + extra + 1) ncap *= 2;
    if (ncap > (1u << 20)) { w->err = 1; return 0; }
    char *nb = (char *)realloc(w->buf, ncap);
    if (!nb) { w->err = 1; return 0; }
    w->buf = nb;
    w->cap = ncap;
    return 1;
}

static void w_putc(wpj_w *w, char c)
{
    if (!w_reserve(w, 1)) return;
    w->buf[w->len++] = c;
    w->buf[w->len] = 0;
}

static void w_puts(wpj_w *w, const char *s)
{
    size_t n = strlen(s);
    if (!w_reserve(w, n)) return;
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

static void w_string(wpj_w *w, const char *s)
{
    w_putc(w, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; ) {
            unsigned char c = *p;
            switch (c) {
            case '"':  w_puts(w, "\\\""); p++; break;
            case '\\': w_puts(w, "\\\\"); p++; break;
            case '\n': w_puts(w, "\\n"); p++; break;
            case '\r': w_puts(w, "\\r"); p++; break;
            case '\t': w_puts(w, "\\t"); p++; break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof tmp, "\\u%04x", c);
                    w_puts(w, tmp);
                    p++;
                } else if (c < 0x80) {
                    w_putc(w, (char)c); p++;
                } else {
                    int n = utf8_len(p);
                    if (n == 0) { w_putc(w, '?'); p++; }
                    else { for (int i = 0; i < n; i++) w_putc(w, (char)p[i]); p += n; }
                }
            }
        }
    }
    w_putc(w, '"');
}

static void w_key(wpj_w *w, const char *key)
{
    if (w->count++) w_putc(w, ',');
    w_string(w, key);
    w_putc(w, ':');
}

void wpj_w_init(wpj_w *w)
{
    memset(w, 0, sizeof *w);
    w_putc(w, '{');
}

void wpj_w_str(wpj_w *w, const char *key, const char *val)
{
    w_key(w, key);
    w_string(w, val ? val : "");
}

void wpj_w_num(wpj_w *w, const char *key, double v)
{
    char tmp[64];
    if (v != v) v = 0;                                   /* NaN guard */
    snprintf(tmp, sizeof tmp, "%.3f", v);
    w_key(w, key);
    w_puts(w, tmp);
}

void wpj_w_int(wpj_w *w, const char *key, long long v)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v);
    w_key(w, key);
    w_puts(w, tmp);
}

void wpj_w_bool(wpj_w *w, const char *key, int v)
{
    w_key(w, key);
    w_puts(w, v ? "true" : "false");
}

const char *wpj_w_end(wpj_w *w)
{
    w_putc(w, '}');
    if (w->err) { free(w->buf); w->buf = NULL; w->len = w->cap = 0; w_putc(w, '{'); w_putc(w, '}'); }
    return w->buf ? w->buf : "{}";
}

void wpj_w_free(wpj_w *w)
{
    free(w->buf);
    memset(w, 0, sizeof *w);
}
