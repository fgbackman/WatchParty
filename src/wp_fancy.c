/* wp_fancy.c — see wp_fancy.h */
#include "wp_fancy.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "wp_fancy_table.h"

#define N_SWEARS (sizeof FANCY_SWEARS / sizeof FANCY_SWEARS[0])
#define N_SLURS  (sizeof FANCY_SLURS_ROT13 / sizeof FANCY_SLURS_ROT13[0])
#define N_WORDS  (N_SWEARS + N_SLURS)

typedef struct { char word[32]; const char *repl; } entry_t;
static entry_t WORDS[N_WORDS];
static int ready;

/* roots that are swapped even inside longer words (no innocent English word contains them) */
static const char *const ROOTS[][2] = {
    { "fuck", "flabbergast" }, { "shit", "sesquipedalian" }, { "bitch", "blatherskite" },
};

static void load(void)
{
    if (ready) return;
    size_t n = 0;
    for (size_t i = 0; i < N_SWEARS; i++, n++) {
        strncpy(WORDS[n].word, FANCY_SWEARS[i][0], sizeof WORDS[n].word - 1);
        WORDS[n].repl = FANCY_SWEARS[i][1];
    }
    for (size_t i = 0; i < N_SLURS; i++, n++) {
        const char *s = FANCY_SLURS_ROT13[i][0];
        size_t k = 0;
        for (; s[k] && k + 1 < sizeof WORDS[n].word; k++) {
            char c = s[k];
            if (c >= 'a' && c <= 'z') c = (char)('a' + (c - 'a' + 13) % 26);
            WORDS[n].word[k] = c;
        }
        WORDS[n].word[k] = 0;
        WORDS[n].repl = FANCY_SLURS_ROT13[i][1];
    }
    ready = 1;
}

/* One-to-one normalisation (so positions line up with the original):
 * lower-case, leetspeak to letters, mask characters to '*'. 0 = not a word char. */
static char norm(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    if (c >= 'a' && c <= 'z') return c;
    switch (c) {
    case '0': return 'o';  case '1': return 'i';  case '3': return 'e';  case '4': return 'a';
    case '5': return 's';  case '7': return 't';  case '8': return 'b';  case '9': return 'g';
    case '@': return 'a';  case '$': return 's';  case '!': return 'i';  case '|': return 'i';
    case '*': case '#': return '*';
    default: return 0;
    }
}

static int token_char(char c) { return norm(c) != 0; }

/* Does token t (normalised) spell word w? Each run of a letter in w must be
 * matched by at least as many of that letter in t (so "fuuuck" = "fuck" but
 * "as" != "ass"); '*' in t stands for one or more letters of w. */
static int match(const char *t, const char *w)
{
    if (!*t) return !*w;
    if (*t == '*') {
        for (size_t k = 1; k <= strlen(w); k++) if (match(t + 1, w + k)) return 1;
        return 0;
    }
    if (!*w || *t != *w) return 0;
    char ch = *w;
    size_t need = 0, have = 0;
    while (w[need] == ch) need++;
    while (t[have] == ch) have++;
    if (have < need) return 0;
    return match(t + have, w + need);
}

/* Copy replacement r into the output, following the case of the original src[0..n). */
static void put_cased(char **o, char *end, const char *src, size_t n, const char *r)
{
    int letters = 0, upper = 0;
    for (size_t i = 0; i < n; i++) if (isalpha((unsigned char)src[i])) { letters++; if (isupper((unsigned char)src[i])) upper++; }
    int all_caps = letters >= 2 && upper == letters;
    int first_cap = 0;
    for (size_t i = 0; i < n; i++) if (isalpha((unsigned char)src[i])) { first_cap = isupper((unsigned char)src[i]); break; }
    for (size_t i = 0; r[i] && *o < end; i++) {
        char c = r[i];
        if (all_caps) c = (char)toupper((unsigned char)c);
        else if (i == 0 && first_cap) c = (char)toupper((unsigned char)c);
        *(*o)++ = c;
    }
}

static void put(char **o, char *end, const char *s, size_t n)
{
    for (size_t i = 0; i < n && *o < end; i++) *(*o)++ = s[i];
}

/* Handle one token src[0..n): exact list words first, then roots inside words. */
static int do_token(char **o, char *end, const char *src, size_t n)
{
    /* edge characters that are punctuation here (#tag, wow!!) stay as they are */
    size_t a = 0, b = n;
    while (a < b && (src[a] == '!' || src[a] == '#' || src[a] == '|')) a++;
    while (b > a && (src[b - 1] == '!' || src[b - 1] == '#' || src[b - 1] == '|')) b--;
    char t[96];
    size_t len = b - a;
    int letters = 0, stars = 0;
    if (len == 0 || len >= sizeof t) { put(o, end, src, n); return 0; }
    for (size_t i = 0; i < len; i++) {
        t[i] = norm(src[a + i]);
        if (isalpha((unsigned char)src[a + i])) letters++;
        if (t[i] == '*') stars++;
    }
    t[len] = 0;
    if (letters == 0) { put(o, end, src, n); return 0; }   /* plain numbers, ***, $$$ */

    for (size_t i = 0; i < N_WORDS; i++) {
        if (stars && (size_t)stars >= strlen(WORDS[i].word)) continue;
        if (stars && t[0] == '*') continue;                  /* need a real first letter */
        if (match(t, WORDS[i].word)) {
            put(o, end, src, a);
            put_cased(o, end, src + a, len, WORDS[i].repl);
            put(o, end, src + b, n - b);
            return 1;
        }
    }
    if (stars) { put(o, end, src, n); return 0; }

    /* roots inside longer words: exact letters only (no stretching), so
     * innocent words like "shiitake" are left alone */
    int hits = 0;
    put(o, end, src, a);
    size_t i = 0;
    while (i < len) {
        int done = 0;
        for (size_t r = 0; r < sizeof ROOTS / sizeof ROOTS[0]; r++) {
            size_t rl = strlen(ROOTS[r][0]);
            if (i + rl <= len && strncmp(t + i, ROOTS[r][0], rl) == 0) {
                put_cased(o, end, src + a + i, rl, ROOTS[r][1]);
                i += rl;
                hits++;
                done = 1;
                break;
            }
        }
        if (!done) { if (*o < end) *(*o)++ = src[a + i]; i++; }
    }
    put(o, end, src + b, n - b);
    return hits;
}

static int looks_like_link(const char *s, size_t n)
{
    for (size_t i = 0; i + 3 <= n; i++) if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') return 1;
    for (size_t i = 0; i + 4 <= n; i++) if (strncmp(s + i, "www.", 4) == 0) return 1;
    for (size_t i = 0; i + 5 <= n; i++) if (strncmp(s + i, "youtu", 5) == 0) return 1;
    return 0;
}

int wp_fancy(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    load();
    char *o = out, *end = out + cap - 1;
    int count = 0;
    const char *p = in ? in : "";
    while (*p && o < end) {
        if (isspace((unsigned char)*p)) { *o++ = *p++; continue; }
        /* a whitespace-separated chunk */
        const char *chunk = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t cn = (size_t)(p - chunk);
        if (looks_like_link(chunk, cn)) { put(&o, end, chunk, cn); continue; }
        /* tokens inside the chunk; hyphens, apostrophes and other punctuation split */
        const char *q = chunk, *ce = p;
        while (q < ce && o < end) {
            if (!token_char(*q)) { *o++ = *q++; continue; }
            const char *ts = q;
            while (q < ce && token_char(*q)) q++;
            count += do_token(&o, end, ts, (size_t)(q - ts));
        }
    }
    *o = 0;
    /* never leave half a UTF-8 character at a truncation point */
    size_t L = strlen(out);
    while (L > 0 && ((unsigned char)out[L - 1] & 0xC0) == 0x80) {
        size_t k = L - 1;
        while (k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
        unsigned char lead = (unsigned char)out[k];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (L - k >= need) break;
        out[k] = 0; L = k;
    }
    return count;
}
