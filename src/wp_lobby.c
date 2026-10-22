/* wp_lobby.c — see wp_lobby.h */
#include "wp_lobby.h"

#include <string.h>

void wp_lobby_key(const uint8_t shared[32], const uint8_t guest_pub[WP_EC_PUB_LEN],
                  const uint8_t host_pub[WP_EC_PUB_LEN], uint8_t key[32])
{
    static const char label[] = "WatchParty lobby v1";
    uint8_t buf[sizeof label - 1 + 32 + 2 * WP_EC_PUB_LEN];
    size_t o = 0;
    memcpy(buf + o, label, sizeof label - 1); o += sizeof label - 1;
    memcpy(buf + o, shared, 32); o += 32;
    memcpy(buf + o, guest_pub, WP_EC_PUB_LEN); o += WP_EC_PUB_LEN;
    memcpy(buf + o, host_pub, WP_EC_PUB_LEN); o += WP_EC_PUB_LEN;
    wp_sha256(buf, o, key);
    memset(buf, 0, sizeof buf);
}

void wp_lobby_check_code(const uint8_t key[32], char out[5])
{
    uint8_t h[32];
    wp_hmac_sha256(key, 32, "check", 5, h);
    unsigned v = (((unsigned)h[0] << 24) | ((unsigned)h[1] << 16) | ((unsigned)h[2] << 8) | h[3]) % 10000u;
    out[0] = (char)('0' + v / 1000);
    out[1] = (char)('0' + (v / 100) % 10);
    out[2] = (char)('0' + (v / 10) % 10);
    out[3] = (char)('0' + v % 10);
    out[4] = 0;
}

/* One message per key, so a fixed keystream is fine: ct = secret ^ HMAC(key,"enc"),
 * tag = HMAC(key, "mac" || ct) truncated to 16 bytes (encrypt-then-MAC). */
static void tag_of(const uint8_t key[32], const uint8_t ct[WP_SECRET_LEN], uint8_t tag[16])
{
    uint8_t msg[3 + WP_SECRET_LEN], h[32];
    memcpy(msg, "mac", 3);
    memcpy(msg + 3, ct, WP_SECRET_LEN);
    wp_hmac_sha256(key, 32, msg, sizeof msg, h);
    memcpy(tag, h, 16);
}

void wp_lobby_seal(const uint8_t key[32], const uint8_t secret[WP_SECRET_LEN], uint8_t box[WP_LOBBY_BOX_LEN])
{
    uint8_t ks[32];
    wp_hmac_sha256(key, 32, "enc", 3, ks);
    for (int i = 0; i < WP_SECRET_LEN; i++) box[i] = secret[i] ^ ks[i];
    tag_of(key, box, box + WP_SECRET_LEN);
    memset(ks, 0, sizeof ks);
}

int wp_lobby_open(const uint8_t key[32], const uint8_t box[WP_LOBBY_BOX_LEN], uint8_t secret[WP_SECRET_LEN])
{
    uint8_t tag[16], ks[32];
    tag_of(key, box, tag);
    unsigned diff = 0;
    for (int i = 0; i < 16; i++) diff |= (unsigned)(tag[i] ^ box[WP_SECRET_LEN + i]);   /* constant time */
    if (diff) return 0;
    wp_hmac_sha256(key, 32, "enc", 3, ks);
    for (int i = 0; i < WP_SECRET_LEN; i++) secret[i] = box[i] ^ ks[i];
    memset(ks, 0, sizeof ks);
    return 1;
}

/* ------------------------------------------------------------- base64 */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void wp_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    if (!cap) return;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        if (o + 4 >= cap) break;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

int wp_b64_decode(const char *in, uint8_t *out, size_t cap)
{
    if (!in) return -1;
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ') continue;
        const char *q = strchr(B64, c);
        if (!q || c == 0) return -1;
        acc = (acc << 6) | (uint32_t)(q - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    return (int)o;
}
