/* Unit tests for wp_json — build: cc -I../src test_json.c ../src/wp_json.c */
#include "../src/wp_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void)
{
    wpj_obj o;

    /* basic object */
    CHECK(wpj_parse("{\"t\":\"play\",\"pos\":12.5,\"playing\":true,\"x\":null}", &o));
    CHECK(o.n == 4);
    CHECK(strcmp(wpj_str(&o, "t"), "play") == 0);
    CHECK(fabs(wpj_num(&o, "pos", 0) - 12.5) < 1e-9);
    CHECK(wpj_bool(&o, "playing", 0) == 1);
    CHECK(wpj_has(&o, "x"));
    CHECK(wpj_str(&o, "x") == NULL);
    CHECK(wpj_str(&o, "missing") == NULL);
    CHECK(wpj_num(&o, "missing", -1) == -1);

    /* whitespace, escapes, unicode */
    CHECK(wpj_parse(" { \"text\" : \"a\\\"b\\\\c\\n\\u00e9\\ud83d\\ude00\" , \"n\" : -3e2 } ", &o));
    CHECK(strcmp(wpj_str(&o, "text"), "a\"b\\c\n\xc3\xa9\xf0\x9f\x98\x80") == 0);
    CHECK(wpj_num(&o, "n", 0) == -300);

    /* nested values are skipped, not fatal */
    CHECK(wpj_parse("{\"a\":{\"b\":[1,2,{\"c\":\"}\"}]},\"d\":\"ok\",\"e\":[\"]\",\"x\"]}", &o));
    CHECK(strcmp(wpj_str(&o, "d"), "ok") == 0);
    CHECK(wpj_str(&o, "a") == NULL);

    /* empty object */
    CHECK(wpj_parse("{}", &o));
    CHECK(o.n == 0);

    /* malformed inputs */
    CHECK(!wpj_parse("", &o));
    CHECK(!wpj_parse("[1,2]", &o));
    CHECK(!wpj_parse("{\"a\":}", &o));
    CHECK(!wpj_parse("{\"a\":\"unterminated}", &o));
    CHECK(!wpj_parse("{\"a\":1,}", &o));
    CHECK(!wpj_parse("{\"a\":1} trailing", &o));
    CHECK(!wpj_parse("{\"a\":\"ctrl\x01char\"}", &o));
    CHECK(!wpj_parse("{\"a\":\"bad\\q\"}", &o));
    CHECK(!wpj_parse("{\"a\":[1,2}", &o));

    /* invalid UTF-8 bytes are replaced, not propagated */
    CHECK(wpj_parse("{\"a\":\"x\xffy\"}", &o));
    CHECK(strcmp(wpj_str(&o, "a"), "x?y") == 0);

    /* huge string overflowing the arena fails cleanly */
    {
        static char big[WPJ_ARENA + 64];
        strcpy(big, "{\"a\":\"");
        size_t len = strlen(big);
        memset(big + len, 'z', WPJ_ARENA);
        strcpy(big + len + WPJ_ARENA, "\"}");
        CHECK(!wpj_parse(big, &o));
    }

    /* too many fields: first WPJ_MAX_FIELDS kept, rest ignored */
    {
        char many[4096] = "{";
        for (int i = 0; i < WPJ_MAX_FIELDS + 5; i++) {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "%s\"k%d\":%d", i ? "," : "", i, i);
            strcat(many, tmp);
        }
        strcat(many, "}");
        CHECK(wpj_parse(many, &o));
        CHECK(o.n == WPJ_MAX_FIELDS);
        CHECK(wpj_num(&o, "k0", -1) == 0);
    }

    /* writer round trip */
    {
        wpj_w w;
        wpj_w_init(&w);
        wpj_w_str(&w, "t", "chat");
        wpj_w_str(&w, "text", "he said \"hi\"\n\ttab \\ \xf0\x9f\x98\x80 \x01");
        wpj_w_num(&w, "pos", 1.23456);
        wpj_w_int(&w, "ht", 1234567890123LL);
        wpj_w_bool(&w, "playing", 0);
        const char *s = wpj_w_end(&w);
        CHECK(strcmp(s, "{\"t\":\"chat\",\"text\":\"he said \\\"hi\\\"\\n\\ttab \\\\ \xf0\x9f\x98\x80 \\u0001\",\"pos\":1.235,\"ht\":1234567890123,\"playing\":false}") == 0);
        CHECK(wpj_parse(s, &o));
        CHECK(strcmp(wpj_str(&o, "text"), "he said \"hi\"\n\ttab \\ \xf0\x9f\x98\x80 \x01") == 0);
        CHECK(fabs(wpj_num(&o, "pos", 0) - 1.235) < 1e-9);
        CHECK(wpj_num(&o, "ht", 0) == 1234567890123.0);
        CHECK(wpj_bool(&o, "playing", 1) == 0);
        wpj_w_free(&w);
    }

    /* writer with long content grows the buffer */
    {
        wpj_w w;
        wpj_w_init(&w);
        char longtext[3000];
        memset(longtext, 'a', sizeof longtext - 1);
        longtext[sizeof longtext - 1] = 0;
        wpj_w_str(&w, "text", longtext);
        const char *s = wpj_w_end(&w);
        CHECK(strlen(s) == 3000 - 1 + strlen("{\"text\":\"\"}"));
        CHECK(wpj_parse(s, &o));
        CHECK(strlen(wpj_str(&o, "text")) == 2999);
        wpj_w_free(&w);
    }

    if (fails) { printf("test_json: %d failure(s)\n", fails); return 1; }
    printf("test_json: all passed\n");
    return 0;
}
