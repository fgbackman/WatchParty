/* wp_util.h — small portable helpers: time, randomness, SHA-256/HMAC,
 * invite codes (base32), YouTube URL parsing, local IP lookup.
 * Pure C99, no Apple frameworks, so it can be unit-tested anywhere. */
#ifndef WP_UTIL_H
#define WP_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define WP_SECRET_LEN 15          /* 120-bit room secret            */
#define WP_CODE_CHARS 24          /* base32 chars for 15 bytes       */
#define WP_CODE_STRLEN 30         /* "xxxx-xxxx-xxxx-xxxx-xxxx-xxxx" + NUL */
#define WP_PSK_LEN 32             /* derived TLS pre-shared key      */
#define WP_VIDEO_ID_LEN 11

/* Monotonic milliseconds (never goes backwards, arbitrary origin). */
uint64_t wp_now_ms(void);

/* Cryptographically strong random bytes. */
void wp_random_bytes(void *buf, size_t n);

/* SHA-256 / HMAC-SHA-256 (RFC 6234 / RFC 2104). */
void wp_sha256(const void *data, size_t len, uint8_t out[32]);
void wp_hmac_sha256(const void *key, size_t key_len,
                    const void *msg, size_t msg_len, uint8_t out[32]);

/* Invite codes: 15 random bytes <-> "abcd-efgh-ijkl-mnop-qrst-uvwx". */
void wp_code_encode(const uint8_t secret[WP_SECRET_LEN], char out[WP_CODE_STRLEN]);
int  wp_code_decode(const char *text, uint8_t out[WP_SECRET_LEN]); /* 1 = ok */

/* TLS pre-shared key derived from the room secret. */
void wp_derive_psk(const uint8_t secret[WP_SECRET_LEN], uint8_t psk[WP_PSK_LEN]);

/* Extract an 11-char YouTube video id from a URL or bare id. 1 = ok. */
int wp_parse_youtube_id(const char *input, char out[WP_VIDEO_ID_LEN + 1]);
int wp_is_video_id(const char *s);

/* First non-loopback IPv4 address of this machine ("192.168.1.5"). 1 = ok. */
int wp_local_ipv4(char *out, size_t cap);

/* Trim leading/trailing whitespace in place. */
void wp_trim(char *s);

/* Safe bounded copy (always NUL-terminates). */
void wp_strlcpy(char *dst, const char *src, size_t cap);

#endif
