/* wp_search_parse.c — the pure-C half of wp_search.h (unit-tested anywhere). */
#define _GNU_SOURCE                 /* memmem on glibc; always present on Apple */
#include "wp_search.h"
#include "wp_json.h"

#include <stdio.h>
#include <string.h>

static void clean(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < cap; p++) {
        if (*p < 0x20 || *p == 0x7f) { dst[o++] = ' '; continue; }
        dst[o++] = (char)*p;
    }
    dst[o] = 0;
    while (o > 0 && ((unsigned char)dst[o - 1] & 0xC0) == 0x80) {   /* no half characters */
        size_t k = o - 1;
        while (k > 0 && ((unsigned char)dst[k] & 0xC0) == 0x80) k--;
        unsigned char lead = (unsigned char)dst[k];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (o - k >= need) break;
        dst[k] = 0; o = k;
    }
    wp_trim(dst);
}

/* Copy the JSON string that follows `needle` (within [p, end)) and unescape it. */
static int grab_field(const char *p, const char *end, const char *needle, char *out, size_t cap)
{
    size_t nlen = strlen(needle);
    if (p >= end) return 0;
    const char *q = memmem(p, (size_t)(end - p), needle, nlen);
    if (!q) return 0;
    q += nlen;
    const char *e = q;
    while (e < end && *e != '"') { if (*e == '\\' && e + 1 < end) e++; e++; }
    if (e >= end) return 0;
    char raw[700];
    size_t rl = (size_t)(e - q);
    if (rl > sizeof raw - 10) rl = sizeof raw - 10;
    while (rl > 0 && q[rl - 1] == '\\') rl--;          /* never end on a dangling escape */
    memcpy(raw, "{\"t\":\"", 6);                       /* let the JSON parser unescape */
    memcpy(raw + 6, q, rl);
    memcpy(raw + 6 + rl, "\"}", 3);
    wpj_obj o;
    const char *t = wpj_parse(raw, &o) ? wpj_str(&o, "t") : NULL;
    if (!t) return 0;
    clean(out, cap, t);
    return out[0] != 0;
}

int wp_yt_parse(const char *html, size_t len, wp_result *out, int max)
{
    static const char marker[] = "\"videoRenderer\":{\"videoId\":\"";
    const char *p = html, *end = html + len;
    int n = 0;
    if (!html) return 0;
    while (n < max && p < end) {
        p = memmem(p, (size_t)(end - p), marker, sizeof marker - 1);
        if (!p) break;
        p += sizeof marker - 1;
        if (end - p < WP_VIDEO_ID_LEN) break;
        char vid[WP_VIDEO_ID_LEN + 1];
        memcpy(vid, p, WP_VIDEO_ID_LEN);
        vid[WP_VIDEO_ID_LEN] = 0;
        if (!wp_is_video_id(vid)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (strcmp(out[i].id, vid) == 0) dup = 1;
        if (dup) continue;
        /* this video's fields live before the next videoRenderer */
        const char *next = memmem(p, (size_t)(end - p), marker, sizeof marker - 1);
        const char *stop = next ? next : end;
        if (stop - p > 20000) stop = p + 20000;
        char title[200] = "", owner[80] = "", length[24] = "", views[48] = "", age[48] = "";
        if (!grab_field(p, stop, "\"title\":{\"runs\":[{\"text\":\"", title, sizeof title)) continue;
        grab_field(p, stop, "\"ownerText\":{\"runs\":[{\"text\":\"", owner, sizeof owner);
        const char *lt = memmem(p, (size_t)(stop - p), "\"lengthText\":", 13);
        if (lt) grab_field(lt, stop, "\"simpleText\":\"", length, sizeof length);   /* the compact "12:34" */
        grab_field(p, stop, "\"viewCountText\":{\"simpleText\":\"", views, sizeof views);
        grab_field(p, stop, "\"publishedTimeText\":{\"simpleText\":\"", age, sizeof age);
        wp_strlcpy(out[n].id, vid, sizeof out[n].id);
        wp_strlcpy(out[n].title, title, sizeof out[n].title);
        snprintf(out[n].meta, sizeof out[n].meta, "%s%s%s%s%s%s%s",
                 owner, length[0] ? "  ·  " : "", length, views[0] ? "  ·  " : "", views, age[0] ? "  ·  " : "", age);
        n++;
    }
    return n;
}
