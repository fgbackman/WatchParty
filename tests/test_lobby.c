/* Tests for the pure lobby parts — cc -I../src test_lobby.c ../src/wp_lobby.c ../src/wp_util.c */
#include "../src/wp_lobby.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    /* base64 (RFC 4648 vectors) */
    const char *plain[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    const char *enc[]   = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    for (int i = 0; i < 7; i++) {
        char out[32]; uint8_t back[32];
        wp_b64_encode((const uint8_t *)plain[i], strlen(plain[i]), out, sizeof out);
        CHECK(strcmp(out, enc[i]) == 0);
        int n = wp_b64_decode(enc[i], back, sizeof back);
        CHECK(n == (int)strlen(plain[i]) && memcmp(back, plain[i], (size_t)n) == 0);
    }
    { uint8_t b[4]; CHECK(wp_b64_decode("Zm9v!", b, sizeof b) == -1); CHECK(wp_b64_decode("Zm9vYmFy", b, 4) == -1); }
    {   /* a 65-byte public key survives a round trip */
        uint8_t pub[WP_EC_PUB_LEN], back[WP_EC_PUB_LEN + 4];
        char s[128];
        for (int i = 0; i < WP_EC_PUB_LEN; i++) pub[i] = (uint8_t)(i * 7 + 4);
        wp_b64_encode(pub, sizeof pub, s, sizeof s);
        CHECK(strlen(s) == 88);
        CHECK(wp_b64_decode(s, back, sizeof back) == WP_EC_PUB_LEN && memcmp(pub, back, WP_EC_PUB_LEN) == 0);
    }

    /* derivation / check code / seal */
    uint8_t shared[32], gp[WP_EC_PUB_LEN], hp[WP_EC_PUB_LEN], k1[32], k2[32], k3[32];
    for (int i = 0; i < 32; i++) shared[i] = (uint8_t)i;
    memset(gp, 0x11, sizeof gp); gp[0] = 4;
    memset(hp, 0x22, sizeof hp); hp[0] = 4;
    wp_lobby_key(shared, gp, hp, k1);
    wp_lobby_key(shared, gp, hp, k2);
    wp_lobby_key(shared, hp, gp, k3);                 /* roles matter */
    CHECK(memcmp(k1, k2, 32) == 0);
    CHECK(memcmp(k1, k3, 32) != 0);

    char c1[5], c2[5];
    wp_lobby_check_code(k1, c1);
    wp_lobby_check_code(k2, c2);
    CHECK(strlen(c1) == 4 && strcmp(c1, c2) == 0);
    for (int i = 0; i < 4; i++) CHECK(c1[i] >= '0' && c1[i] <= '9');

    uint8_t secret[WP_SECRET_LEN], box[WP_LOBBY_BOX_LEN], out[WP_SECRET_LEN];
    for (int i = 0; i < WP_SECRET_LEN; i++) secret[i] = (uint8_t)(200 - i);
    wp_lobby_seal(k1, secret, box);
    CHECK(memcmp(box, secret, WP_SECRET_LEN) != 0);  /* actually encrypted */
    CHECK(wp_lobby_open(k2, box, out) && memcmp(out, secret, WP_SECRET_LEN) == 0);
    CHECK(!wp_lobby_open(k3, box, out));              /* wrong key */
    box[3] ^= 1;
    CHECK(!wp_lobby_open(k1, box, out));              /* tampered */
    box[3] ^= 1;
    box[WP_LOBBY_BOX_LEN - 1] ^= 0x80;
    CHECK(!wp_lobby_open(k1, box, out));              /* tampered tag */

    if (fails) { printf("test_lobby: %d failure(s)\n", fails); return 1; }
    printf("test_lobby: all passed\n");
    return 0;
}
