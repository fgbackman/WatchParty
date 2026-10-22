/* wp_lobby.h — "ask to join": how a host hands the room key to a guest it
 * approved, without an invite code being typed.
 *
 *  1. guest -> host  {"t":"knock","name":..,"pub":<guest P-256 public key>}
 *  2. host  -> guest {"t":"knocked","room":..,"pub":<host P-256 public key>}
 *     Both sides now share an ECDH secret and show the same 4-digit check code,
 *     so the host can confirm it is really talking to that person.
 *  3. host  -> guest {"t":"allow","box":<room key sealed with the shared key>}
 *              or     {"t":"deny"}
 *  4. guest opens the box and joins the room's TLS channel as usual.
 *
 * This file holds the pure parts (key derivation, check code, sealing, base64)
 * so they can be unit-tested anywhere; wp_ec.c does the P-256 maths with
 * Apple's Security framework. */
#ifndef WP_LOBBY_H
#define WP_LOBBY_H

#include <stddef.h>
#include <stdint.h>
#include "wp_util.h"

#define WP_EC_PUB_LEN   65                      /* uncompressed X9.63 point: 04 || X || Y */
#define WP_LOBBY_BOX_LEN (WP_SECRET_LEN + 16)   /* encrypted room key + tag */

/* key = SHA-256("WatchParty lobby v1" || shared || guest_pub || host_pub) */
void wp_lobby_key(const uint8_t shared[32], const uint8_t guest_pub[WP_EC_PUB_LEN],
                  const uint8_t host_pub[WP_EC_PUB_LEN], uint8_t key[32]);

/* 4 digits both screens show, e.g. "0427" */
void wp_lobby_check_code(const uint8_t key[32], char out[5]);

/* Seal / open the 15-byte room secret. open returns 1 if the tag is valid. */
void wp_lobby_seal(const uint8_t key[32], const uint8_t secret[WP_SECRET_LEN], uint8_t box[WP_LOBBY_BOX_LEN]);
int  wp_lobby_open(const uint8_t key[32], const uint8_t box[WP_LOBBY_BOX_LEN], uint8_t secret[WP_SECRET_LEN]);

/* Base64 (standard alphabet, with padding). decode returns bytes written or -1. */
void wp_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
int  wp_b64_decode(const char *in, uint8_t *out, size_t cap);

/* ---- P-256 ECDH (wp_ec.c, Apple platforms) ---- */
typedef struct { void *priv; uint8_t pub[WP_EC_PUB_LEN]; } wp_eckey;

int  wp_ec_generate(wp_eckey *k);                                     /* 1 = ok */
int  wp_ec_shared(const wp_eckey *k, const uint8_t peer_pub[WP_EC_PUB_LEN], uint8_t shared[32]);  /* 1 = ok */
void wp_ec_free(wp_eckey *k);

#endif
