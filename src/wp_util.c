/* wp_util.c — see wp_util.h */
#define _GNU_SOURCE
#include "wp_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ------------------------------------------------------------------ time */

uint64_t wp_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ------------------------------------------------------------ randomness */

#ifdef __APPLE__
#include <stdlib.h>   /* arc4random_buf */
void wp_random_bytes(void *buf, size_t n) { arc4random_buf(buf, n); }
#else
void wp_random_bytes(void *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(buf, 1, n, f) : 0;
    if (f) fclose(f);
    if (got != n) { fprintf(stderr, "wp_random_bytes: no entropy source\n"); abort(); }
}
#endif

/* --------------------------------------------------------------- SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t st[8], const uint8_t blk[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4] << 24) | ((uint32_t)blk[i*4+1] << 16) |
               ((uint32_t)blk[i*4+2] << 8) | (uint32_t)blk[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i-15], 7) ^ ROTR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROTR(w[i-2], 17) ^ ROTR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void wp_sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint8_t *p = (const uint8_t *)data;
    size_t rem = len;
    while (rem >= 64) { sha256_block(st, p); p += 64; rem -= 64; }

    uint8_t tail[128];
    memset(tail, 0, sizeof tail);
    memcpy(tail, p, rem);
    tail[rem] = 0x80;
    size_t tail_len = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8ULL;
    for (int i = 0; i < 8; i++) tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(st, tail);
    if (tail_len == 128) sha256_block(st, tail + 64);

    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(st[i] >> 24);
        out[i*4+1] = (uint8_t)(st[i] >> 16);
        out[i*4+2] = (uint8_t)(st[i] >> 8);
        out[i*4+3] = (uint8_t)(st[i]);
    }
}

void wp_hmac_sha256(const void *key, size_t key_len,
                    const void *msg, size_t msg_len, uint8_t out[32])
{
    uint8_t k[64];
    memset(k, 0, sizeof k);
    if (key_len > 64) wp_sha256(key, key_len, k);
    else memcpy(k, key, key_len);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    /* inner = H(ipad || msg) */
    uint8_t *inner = (uint8_t *)malloc(64 + msg_len);
    if (!inner) abort();
    memcpy(inner, ipad, 64);
    memcpy(inner + 64, msg, msg_len);
    uint8_t ih[32];
    wp_sha256(inner, 64 + msg_len, ih);
    free(inner);

    /* out = H(opad || inner) */
    uint8_t outer[96];
    memcpy(outer, opad, 64);
    memcpy(outer + 64, ih, 32);
    wp_sha256(outer, 96, out);
}

/* ------------------------------------------------------- invite codes */

static const char B32[] = "abcdefghijklmnopqrstuvwxyz234567";

void wp_code_encode(const uint8_t secret[WP_SECRET_LEN], char out[WP_CODE_STRLEN])
{
    char raw[WP_CODE_CHARS + 1];
    uint32_t acc = 0;
    int bits = 0, n = 0;
    for (int i = 0; i < WP_SECRET_LEN; i++) {
        acc = (acc << 8) | secret[i];
        bits += 8;
        while (bits >= 5 && n < WP_CODE_CHARS) {
            bits -= 5;
            raw[n++] = B32[(acc >> bits) & 31];
        }
    }
    raw[n] = 0;
    int o = 0;
    for (int i = 0; i < WP_CODE_CHARS; i++) {
        if (i && (i % 4) == 0) out[o++] = '-';
        out[o++] = raw[i];
    }
    out[o] = 0;
}

int wp_code_decode(const char *text, uint8_t out[WP_SECRET_LEN])
{
    if (!text) return 0;
    uint8_t vals[WP_CODE_CHARS];
    int n = 0;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '.') continue;
        c = (unsigned char)tolower(c);
        if (c == '0') c = 'o';          /* friendly fixes for look-alikes */
        else if (c == '1') c = 'l';
        const char *q = strchr(B32, (int)c);
        if (!q) return 0;
        if (n >= WP_CODE_CHARS) return 0;
        vals[n++] = (uint8_t)(q - B32);
    }
    if (n != WP_CODE_CHARS) return 0;
    uint32_t acc = 0;
    int bits = 0, o = 0;
    for (int i = 0; i < WP_CODE_CHARS; i++) {
        acc = (acc << 5) | vals[i];
        bits += 5;
        if (bits >= 8 && o < WP_SECRET_LEN) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    return o == WP_SECRET_LEN;
}

void wp_derive_psk(const uint8_t secret[WP_SECRET_LEN], uint8_t psk[WP_PSK_LEN])
{
    static const char label[] = "WatchParty v1 TLS-PSK";
    wp_hmac_sha256(secret, WP_SECRET_LEN, label, sizeof(label) - 1, psk);
}

/* ------------------------------------------------------- youtube ids */

static int id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int wp_is_video_id(const char *s)
{
    if (!s || strlen(s) != WP_VIDEO_ID_LEN) return 0;
    for (int i = 0; i < WP_VIDEO_ID_LEN; i++) if (!id_char(s[i])) return 0;
    return 1;
}

static int take_id(const char *p, char out[WP_VIDEO_ID_LEN + 1])
{
    for (int i = 0; i < WP_VIDEO_ID_LEN; i++) if (!id_char(p[i])) return 0;
    if (id_char(p[WP_VIDEO_ID_LEN])) return 0;   /* longer token: not an id */
    memcpy(out, p, WP_VIDEO_ID_LEN);
    out[WP_VIDEO_ID_LEN] = 0;
    return 1;
}

int wp_parse_youtube_id(const char *input, char out[WP_VIDEO_ID_LEN + 1])
{
    if (!input) return 0;
    char buf[2048];
    wp_strlcpy(buf, input, sizeof buf);
    wp_trim(buf);
    if (wp_is_video_id(buf)) { memcpy(out, buf, WP_VIDEO_ID_LEN + 1); return 1; }

    static const char *markers[] = {
        "?v=", "&v=", "youtu.be/", "/shorts/", "/embed/", "/live/", "/v/", NULL
    };
    for (int i = 0; markers[i]; i++) {
        const char *p = strstr(buf, markers[i]);
        while (p) {
            if (take_id(p + strlen(markers[i]), out)) return 1;
            p = strstr(p + 1, markers[i]);
        }
    }
    return 0;
}

/* ----------------------------------------------------------- local ip */

int wp_local_ipv4(char *out, size_t cap)
{
    struct ifaddrs *list = NULL, *ifa;
    if (getifaddrs(&list) != 0) return 0;
    int found = 0;
    const char *prefer[] = { "en0", "en1", NULL };
    /* two passes: preferred interface names first, then anything */
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (ifa = list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            if (pass == 0) {
                int ok = 0;
                for (int i = 0; prefer[i]; i++) if (strcmp(ifa->ifa_name, prefer[i]) == 0) ok = 1;
                if (!ok) continue;
            }
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            uint32_t a = ntohl(sin->sin_addr.s_addr);
            if ((a >> 24) == 127 || (a >> 16) == 0xA9FE /* 169.254 link-local */) continue;
            if (inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)cap)) { found = 1; break; }
        }
    }
    freeifaddrs(list);
    return found;
}

/* ------------------------------------------------------------ strings */

void wp_trim(char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) s[--len] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, len - i + 1);
}

void wp_strlcpy(char *dst, const char *src, size_t cap)
{
    if (!cap) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}
