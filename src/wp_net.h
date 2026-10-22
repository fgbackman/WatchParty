/* wp_net.h — peer transport for WatchParty.
 *
 * Built on Apple's Network.framework (C API), on macOS and iOS:
 *   - The ROOM channel: TLS 1.2 with a pre-shared key derived from the room's
 *     invite code. Nobody without the key can connect, read or inject messages.
 *   - The LOBBY channel ("ask to join"): a plain TCP door next to the room where a
 *     guest without the key can knock. Nothing secret crosses it in the clear —
 *     the room key is only ever sent sealed with an ECDH key (see wp_lobby.h).
 *   - Length-prefixed JSON frames on both.
 *   - Bonjour: rooms advertise _watchparty._tcp (and _wplobby._tcp for knocking)
 *     and are listed together by name.
 *
 * Threading: every callback is delivered on the main dispatch queue, and all
 * public functions must be called from the main thread. */
#ifndef WP_NET_H
#define WP_NET_H

#include <stddef.h>
#include <stdint.h>

#define WP_MAX_PEERS     32
#define WP_MAX_LOBBY     6
#define WP_MAX_ROOMS     32
#define WP_MAX_FRAME     (64 * 1024)
#define WP_MAX_LOBBY_FRAME 4096
#define WP_DEFAULT_PORT  47321
#define WP_SERVICE_TYPE  "_watchparty._tcp"
#define WP_LOBBY_TYPE    "_wplobby._tcp"

typedef enum {
    WP_CLIENT_CONNECTING,
    WP_CLIENT_WAITING,     /* no route yet (network down?) — still trying */
    WP_CLIENT_READY,       /* connected (TLS established for the room) */
    WP_CLIENT_FAILED,
    WP_CLIENT_CLOSED
} wp_client_state;

typedef struct {
    /* hosting */
    void (*host_state)(int listening, uint16_t port, const char *error);
    void (*peer_joined)(int peer_id);        /* TLS handshake succeeded */
    void (*peer_left)(int peer_id);
    /* both roles; peer_id is 0 for messages from the host (client side) */
    void (*message)(int peer_id, const char *json);
    /* joining */
    void (*client_state)(wp_client_state state, const char *error);
    /* discovery */
    void (*rooms_changed)(void);
    /* lobby, host side: someone opened the door / spoke / left */
    void (*lobby_joined)(int lobby_id);
    void (*lobby_message)(int lobby_id, const char *json);
    void (*lobby_left)(int lobby_id);
    void (*lobby_ready)(uint16_t port);       /* the door is open */
    /* lobby, guest side: our knock connection */
    void (*knock_state)(wp_client_state state, const char *error);
    void (*knock_message)(const char *json);
} wp_net_callbacks;

void wp_net_init(const wp_net_callbacks *cb);
void wp_net_shutdown(void);

/* ---- hosting ---- */
int      wp_net_host_start(const uint8_t *psk, size_t psk_len, const char *room_name,
                           uint16_t port /* 0 = any */, int advertise);
void     wp_net_host_stop(void);              /* also closes the lobby */
int      wp_net_hosting(void);
uint16_t wp_net_host_port(void);
int      wp_net_peer_count(void);
void     wp_net_send_to(int peer_id, const char *json);
void     wp_net_broadcast(const char *json, int except_peer_id);
void     wp_net_kick(int peer_id);

/* ---- lobby (host) ---- */
int      wp_net_lobby_start(const char *room_name);   /* any port; advertised if named */
void     wp_net_lobby_stop(void);
uint16_t wp_net_lobby_port(void);
void     wp_net_lobby_send(int lobby_id, const char *json);
void     wp_net_lobby_close(int lobby_id);

/* ---- joining ---- */
int  wp_net_connect_host(const uint8_t *psk, size_t psk_len, const char *host, const char *port);
int  wp_net_connect_room(const uint8_t *psk, size_t psk_len, int room_index);
void wp_net_send_host(const char *json);
void wp_net_disconnect(void);
int  wp_net_connected(void);

/* ---- knocking (guest) ---- */
int  wp_net_knock_room(int room_index);               /* 0 if the room has no lobby */
int  wp_net_knock_host(const char *host, const char *port);
void wp_net_knock_send(const char *json);
void wp_net_knock_close(void);

/* ---- discovery ---- */
void        wp_net_browse_start(void);
void        wp_net_browse_stop(void);
int         wp_net_room_count(void);
const char *wp_net_room_name(int index);
int         wp_net_room_has_lobby(int index);
int         wp_net_room_find(const char *name);       /* index or -1 */

#endif
