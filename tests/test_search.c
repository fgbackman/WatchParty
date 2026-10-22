/* Tests for the results-page parser — cc -I../src test_search.c ../src/wp_search_parse.c ../src/wp_json.c ../src/wp_util.c */
#include "../src/wp_search.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    const char *page =
        "<script>var ytInitialData = {\"contents\":[{"
        "\"videoRenderer\":{\"videoId\":\"dQw4w9WgXcQ\",\"thumbnail\":{},"
        "\"title\":{\"runs\":[{\"text\":\"Never \\\"Gonna\\\" \\u00e9 \\ud83c\\udf89\"}]},"
        "\"ownerText\":{\"runs\":[{\"text\":\"Rick Astley\"}]},"
        "\"lengthText\":{\"accessibility\":{\"accessibilityData\":{\"label\":\"3 minutes\"}},\"simpleText\":\"3:33\"},"
        "\"viewCountText\":{\"simpleText\":\"1.6B views\"},\"publishedTimeText\":{\"simpleText\":\"15 years ago\"}}},"
        "{\"videoRenderer\":{\"videoId\":\"dQw4w9WgXcQ\",\"title\":{\"runs\":[{\"text\":\"dup\"}]}}},"
        "{\"videoRenderer\":{\"videoId\":\"bad\\\"id\\\"xx\",\"title\":{\"runs\":[{\"text\":\"bad\"}]}}},"
        "{\"videoRenderer\":{\"videoId\":\"abcdefghijk\",\"title\":{\"runs\":[{\"text\":\"Live now\"}]}}},"
        "{\"videoRenderer\":{\"videoId\":\"notitle1234\",\"thumbnail\":{}}}"
        "]};</script>";
    wp_result r[WP_MAX_RESULTS];
    int n = wp_yt_parse(page, strlen(page), r, WP_MAX_RESULTS);
    CHECK(n == 2);
    CHECK(strcmp(r[0].id, "dQw4w9WgXcQ") == 0);
    CHECK(strcmp(r[0].title, "Never \"Gonna\" \xc3\xa9 \xf0\x9f\x8e\x89") == 0);
    CHECK(strcmp(r[0].meta, "Rick Astley  ·  3:33  ·  1.6B views  ·  15 years ago") == 0);
    CHECK(strcmp(r[1].id, "abcdefghijk") == 0 && strcmp(r[1].title, "Live now") == 0 && r[1].meta[0] == 0);
    CHECK(wp_yt_parse(page, strlen(page), r, 1) == 1);
    CHECK(wp_yt_parse("", 0, r, 8) == 0);
    CHECK(wp_yt_parse("\"videoRenderer\":{\"videoId\":\"abc", 31, r, 8) == 0);   /* truncated page */

    if (fails) { printf("test_search: %d failure(s)\n", fails); return 1; }
    printf("test_search: all passed\n");
    return 0;
}
