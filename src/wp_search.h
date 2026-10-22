/* wp_search.h — YouTube search without an API key (reads the public results
 * page) and a tiny HTTP fetch for thumbnails. Shared by the Mac and iPhone apps. */
#ifndef WP_SEARCH_H
#define WP_SEARCH_H

#include <stddef.h>
#include "wp_util.h"

#define WP_MAX_RESULTS 8

typedef struct {
    char id[WP_VIDEO_ID_LEN + 1];
    char title[200];
    char meta[224];          /* "Channel  ·  12:34  ·  1.2M views  ·  3 days ago" */
} wp_result;

/* Pure C: pull up to `max` video results out of a results page. */
int wp_yt_parse(const char *html, size_t len, wp_result *out, int max);

/* Fetch + parse. cb runs on the main queue; ok = 0 means the request failed. */
typedef void (*wp_search_cb)(int gen, const wp_result *results, int n, int ok);
void wp_yt_search(const char *query, int gen, wp_search_cb cb);

/* GET a URL; cb gets the bytes (NULL on failure) on the main queue, valid during the call. */
typedef void (*wp_fetch_cb)(int tag, int gen, const void *bytes, size_t len);
void wp_fetch(const char *url, int tag, int gen, wp_fetch_cb cb);

/* Headless check: WatchParty --search "lofi hip hop" */
int wp_search_main(const char *query);

#endif
