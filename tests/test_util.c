/* Unit tests for wp_util — build: cc -I../src test_util.c ../src/wp_util.c */
#include "../src/wp_util.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
}

int main(void)
{
    uint8_t d[32];
    char h[65];

    /* SHA-256 test vectors (FIPS 180-2) */
    wp_sha256("abc", 3, d); hex(d, 32, h);
    CHECK(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    wp_sha256("", 0, d); hex(d, 32, h);
    CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    wp_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d); hex(d, 32, h);
    CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    {   /* one million 'a' */
        static char m[1000000];
        memset(m, 'a', sizeof m);
        wp_sha256(m, sizeof m, d); hex(d, 32, h);
        CHECK(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    }
    {   /* 64-byte message exercises the two-block padding path */
        char m[64]; memset(m, 'a', 64);
        wp_sha256(m, 64, d); hex(d, 32, h);
        CHECK(strcmp(h, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb") == 0);
    }

    /* HMAC-SHA-256 (RFC 4231 test cases 1, 2, 6) */
    {
        uint8_t key[20]; memset(key, 0x0b, 20);
        wp_hmac_sha256(key, 20, "Hi There", 8, d); hex(d, 32, h);
        CHECK(strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") == 0);
    }
    wp_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d); hex(d, 32, h);
    CHECK(strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);
    {
        uint8_t key[131]; memset(key, 0xaa, sizeof key);
        const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
        wp_hmac_sha256(key, sizeof key, msg, strlen(msg), d); hex(d, 32, h);
        CHECK(strcmp(h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") == 0);
    }

    /* invite code round trip */
    for (int iter = 0; iter < 200; iter++) {
        uint8_t s[WP_SECRET_LEN], back[WP_SECRET_LEN];
        char code[WP_CODE_STRLEN];
        wp_random_bytes(s, sizeof s);
        wp_code_encode(s, code);
        CHECK(strlen(code) == WP_CODE_STRLEN - 1);
        CHECK(code[4] == '-' && code[9] == '-' && code[29] == 0);
        CHECK(wp_code_decode(code, back));
        CHECK(memcmp(s, back, WP_SECRET_LEN) == 0);
    }
    {
        uint8_t s[WP_SECRET_LEN], back[WP_SECRET_LEN];
        char code[WP_CODE_STRLEN], messy[80];
        for (int i = 0; i < WP_SECRET_LEN; i++) s[i] = (uint8_t)(i * 37 + 11);
        wp_code_encode(s, code);
        /* upper case, spaces instead of dashes, trailing newline */
        int o = 0;
        for (const char *p = code; *p; p++) {
            char c = *p;
            if (c == '-') c = ' ';
            else if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            messy[o++] = c;
        }
        messy[o++] = '\n'; messy[o] = 0;
        CHECK(wp_code_decode(messy, back));
        CHECK(memcmp(s, back, WP_SECRET_LEN) == 0);
        CHECK(!wp_code_decode("abcd-efgh", back));             /* too short */
        CHECK(!wp_code_decode("abcd-efgh-ijkl-mnop-qrst-uvw9", back)); /* bad char */
        CHECK(!wp_code_decode("", back));
        CHECK(!wp_code_decode(NULL, back));
        /* look-alike fixes: 0->o, 1->l */
        char fixed[WP_CODE_STRLEN];
        strcpy(fixed, code);
        int changed = 0;
        for (char *p = fixed; *p; p++) { if (*p == 'o') { *p = '0'; changed = 1; } else if (*p == 'l') { *p = '1'; changed = 1; } }
        CHECK(wp_code_decode(fixed, back));
        CHECK(memcmp(s, back, WP_SECRET_LEN) == 0);
        (void)changed;
    }

    /* PSK derivation is deterministic and differs per secret */
    {
        uint8_t a[WP_SECRET_LEN] = {0}, b[WP_SECRET_LEN] = {1};
        uint8_t pa[32], pa2[32], pb[32];
        wp_derive_psk(a, pa); wp_derive_psk(a, pa2); wp_derive_psk(b, pb);
        CHECK(memcmp(pa, pa2, 32) == 0);
        CHECK(memcmp(pa, pb, 32) != 0);
    }

    /* YouTube id parsing */
    {
        char id[12];
        CHECK(wp_parse_youtube_id("https://www.youtube.com/watch?v=dQw4w9WgXcQ", id) && strcmp(id, "dQw4w9WgXcQ") == 0);
        CHECK(wp_parse_youtube_id("https://www.youtube.com/watch?feature=share&v=dQw4w9WgXcQ&t=42s", id) && strcmp(id, "dQw4w9WgXcQ") == 0);
        CHECK(wp_parse_youtube_id("https://youtu.be/dQw4w9WgXcQ?t=10", id) && strcmp(id, "dQw4w9WgXcQ") == 0);
        CHECK(wp_parse_youtube_id("https://www.youtube.com/shorts/aBc_-1234xy", id) && strcmp(id, "aBc_-1234xy") == 0);
        CHECK(wp_parse_youtube_id("https://www.youtube.com/embed/dQw4w9WgXcQ?rel=0", id) && strcmp(id, "dQw4w9WgXcQ") == 0);
        CHECK(wp_parse_youtube_id("https://www.youtube.com/live/dQw4w9WgXcQ", id) && strcmp(id, "dQw4w9WgXcQ") == 0);
        CHECK(wp_parse_youtube_id("https://m.youtube.com/watch?v=dQw4w9WgXcQ", id) && strcmp(id, "dQw4w9WgXcQ") == 0);
        CHECK(wp_parse_youtube_id("  dQw4w9WgXcQ \n", id) && strcmp(id, "dQw4w9WgXcQ") == 0);
        CHECK(!wp_parse_youtube_id("https://www.youtube.com/", id));
        CHECK(!wp_parse_youtube_id("https://example.com/watch?v=short", id));
        CHECK(!wp_parse_youtube_id("not a url at all", id));
        CHECK(!wp_parse_youtube_id("", id));
        CHECK(!wp_parse_youtube_id("https://www.youtube.com/watch?v=toolongtoolongid", id));
        CHECK(wp_is_video_id("dQw4w9WgXcQ"));
        CHECK(!wp_is_video_id("dQw4w9WgXc\""));
    }

    /* trim / strlcpy */
    {
        char s[32] = "  \t hello world \n";
        wp_trim(s);
        CHECK(strcmp(s, "hello world") == 0);
        char t[4];
        wp_strlcpy(t, "abcdef", sizeof t);
        CHECK(strcmp(t, "abc") == 0);
        wp_strlcpy(t, NULL, sizeof t);
        CHECK(t[0] == 0);
    }

    /* monotonic clock */
    {
        uint64_t a = wp_now_ms(), b = wp_now_ms();
        CHECK(b >= a);
    }

    /* local ip: just must not crash; may legitimately fail in sandboxes */
    {
        char ip[64];
        int ok = wp_local_ipv4(ip, sizeof ip);
        printf("local ipv4: %s\n", ok ? ip : "(none)");
    }

    if (fails) { printf("test_util: %d failure(s)\n", fails); return 1; }
    printf("test_util: all passed\n");
    return 0;
}
