/* wp_search.c — the network half of wp_search.h (Foundation via the ObjC runtime). */
#include "objc_glue.h"
#include "wp_search.h"

#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);

#define SEARCH_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Safari/605.1.15"

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            out[o++] = (char)*p;
        else if (*p == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

static id make_request(const char *url, double timeout)
{
    id nsurl = MSG(id, id)(C("NSURL"), S("URLWithString:"), nsstr(url));
    if (!nsurl) return NULL;
    id req = MSG(id, id)(C("NSMutableURLRequest"), S("requestWithURL:"), nsurl);
    MSG(void, id, id)(req, S("setValue:forHTTPHeaderField:"), nsstr(SEARCH_UA), nsstr("User-Agent"));
    MSG(void, id, id)(req, S("setValue:forHTTPHeaderField:"), nsstr("en-US,en;q=0.9"), nsstr("Accept-Language"));
    MSG(void, id, id)(req, S("setValue:forHTTPHeaderField:"), nsstr("CONSENT=YES+1; SOCS=CAI"), nsstr("Cookie"));
    MSG(void, double)(req, S("setTimeoutInterval:"), timeout);
    return req;
}

/* Run a GET; the handler gets a malloc'd copy of the body (or NULL) on the main queue. */
static void get(const char *url, double timeout, size_t limit, void (^handler)(char *body, size_t len))
{
    id req = make_request(url, timeout);
    if (!req) { handler(NULL, 0); return; }
    void (^done)(id, id, id) = ^(id data, id response, id error) {
        long status = (response && responds(response, "statusCode")) ? MSG(long)(response, S("statusCode")) : 200;
        char *buf = NULL;
        size_t len = 0;
        if (!error && data && status == 200) {
            len = (size_t)MSG(unsigned long)(data, S("length"));
            if (len > limit) len = limit;
            buf = (char *)malloc(len + 1);
            if (buf) { memcpy(buf, MSG(const void *)(data, S("bytes")), len); buf[len] = 0; }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            void *pool = objc_autoreleasePoolPush();
            handler(buf, len);
            free(buf);
            objc_autoreleasePoolPop(pool);
        });
    };
    id session = MSG(id)(C("NSURLSession"), S("sharedSession"));
    id task = MSG(id, id, void *)(session, S("dataTaskWithRequest:completionHandler:"), req, (void *)done);
    MSG(void)(task, S("resume"));
}

void wp_yt_search(const char *query, int gen, wp_search_cb cb)
{
    char enc[1024], url[1200];
    url_encode(query ? query : "", enc, sizeof enc);
    snprintf(url, sizeof url, "https://www.youtube.com/results?search_query=%s&hl=en", enc);
    get(url, 15.0, 6u * 1024 * 1024, ^(char *body, size_t len) {
        wp_result r[WP_MAX_RESULTS];
        int n = body ? wp_yt_parse(body, len, r, WP_MAX_RESULTS) : 0;
        cb(gen, r, n, body != NULL);
    });
}

void wp_fetch(const char *url, int tag, int gen, wp_fetch_cb cb)
{
    get(url, 10.0, 4u * 1024 * 1024, ^(char *body, size_t len) { cb(tag, gen, body, len); });
}

int wp_search_main(const char *query)
{
    char enc[1024], url[1200];
    url_encode(query, enc, sizeof enc);
    snprintf(url, sizeof url, "https://www.youtube.com/results?search_query=%s&hl=en", enc);
    get(url, 15.0, 6u * 1024 * 1024, ^(char *body, size_t len) {
        if (!body) { printf("FAIL: request failed\n"); exit(1); }
        wp_result r[WP_MAX_RESULTS];
        int n = wp_yt_parse(body, len, r, WP_MAX_RESULTS);
        printf("%zu bytes, %d results\n", len, n);
        for (int i = 0; i < n; i++) printf("  %s  %s\n      %s\n", r[i].id, r[i].title, r[i].meta);
        exit(n > 0 ? 0 : 1);
    });
    dispatch_main();
    return 1;
}
