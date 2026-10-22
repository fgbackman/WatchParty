/* wp_net.c — see wp_net.h */
#include "wp_net.h"

#include <Network/Network.h>
#include <Security/Security.h>
#include <dispatch/dispatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* TLS 1.2 PSK cipher suites (RFC 5487). These are not in the tls_ciphersuite_t
 * enum, so they are given by their IANA values. */
#define WP_TLS_PSK_WITH_AES_256_GCM_SHA384  0x00A9
#define WP_TLS_PSK_WITH_AES_128_GCM_SHA256  0x00A8
#define WP_PSK_IDENTITY "WatchParty"

enum { CH_ROOM = 0, CH_LOBBY = 1, N_CH = 2 };

typedef struct {
    int id;                 /* 0 = free slot */
    int chan;
    nw_connection_t conn;
    int ready;
} peer_t;

typedef struct {
    char name[64];
    nw_endpoint_t ep;       /* the room (TLS) */
    nw_endpoint_t lep;      /* its lobby (plain), if any */
} room_t;

typedef struct {
    nw_listener_t l;
    int gen;
    uint16_t port;
    int ready;
} lst_t;

typedef struct {
    nw_connection_t conn;
    int ready;
    int gen;
} cli_t;

static wp_net_callbacks CB;
static dispatch_queue_t net_q;
static dispatch_queue_t main_q;

static lst_t  lst[N_CH];
static peer_t peers[WP_MAX_PEERS + WP_MAX_LOBBY];
#define N_SLOTS ((int)(sizeof peers / sizeof peers[0]))
static int    next_peer_id = 1;
static cli_t  cli[N_CH];

static nw_browser_t browser[N_CH];
static int          browser_gen[N_CH];
static room_t       rooms[WP_MAX_ROOMS];
static int          room_slots;          /* entries in use (some may be lobby-only) */

/* ----------------------------------------------------------- helpers */

static void copy_out(dispatch_data_t d, void *dst, size_t cap)
{
    dispatch_data_apply(d, ^bool(dispatch_data_t region, size_t offset, const void *buffer, size_t size) {
        (void)region;
        if (offset >= cap) return false;
        size_t n = size;
        if (offset + n > cap) n = cap - offset;
        memcpy((char *)dst + offset, buffer, n);
        return true;
    });
}

static void describe_error(nw_error_t error, char *out, size_t cap)
{
    if (!error) { out[0] = 0; return; }
    int code = nw_error_get_error_code(error);
    switch (nw_error_get_error_domain(error)) {
    case nw_error_domain_tls:
        snprintf(out, cap, "secure handshake failed (wrong invite code?) [TLS %d]", code);
        break;
    case nw_error_domain_dns:
        snprintf(out, cap, "could not find that address [DNS %d]", code);
        break;
    case nw_error_domain_posix:
        snprintf(out, cap, "%s [%d]", strerror(code), code);
        break;
    default:
        snprintf(out, cap, "network error %d", code);
        break;
    }
}

static peer_t *peer_by_id(int id, int chan)
{
    if (id <= 0) return NULL;
    for (int i = 0; i < N_SLOTS; i++) if (peers[i].id == id && peers[i].chan == chan) return &peers[i];
    return NULL;
}

static void tcp_opts(nw_protocol_options_t tcp_options)
{
    nw_tcp_options_set_no_delay(tcp_options, true);
    nw_tcp_options_set_enable_keepalive(tcp_options, true);
    nw_tcp_options_set_keepalive_idle_time(tcp_options, 15);
    nw_tcp_options_set_connection_timeout(tcp_options, 15);
}

/* TLS-PSK parameters for the room. */
static nw_parameters_t make_params(const uint8_t *psk, size_t psk_len)
{
    nw_parameters_t params = nw_parameters_create_secure_tcp(
        ^(nw_protocol_options_t tls_options) {
            sec_protocol_options_t sec = nw_tls_copy_sec_protocol_options(tls_options);
            dispatch_data_t key = dispatch_data_create(psk, psk_len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            dispatch_data_t ident = dispatch_data_create(WP_PSK_IDENTITY, strlen(WP_PSK_IDENTITY), NULL,
                                                         DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            sec_protocol_options_add_pre_shared_key(sec, key, ident);
            sec_protocol_options_append_tls_ciphersuite(sec, (tls_ciphersuite_t)WP_TLS_PSK_WITH_AES_256_GCM_SHA384);
            sec_protocol_options_append_tls_ciphersuite(sec, (tls_ciphersuite_t)WP_TLS_PSK_WITH_AES_128_GCM_SHA256);
            sec_protocol_options_set_min_tls_protocol_version(sec, tls_protocol_version_TLSv12);
            sec_protocol_options_set_max_tls_protocol_version(sec, tls_protocol_version_TLSv12);
            dispatch_release(key);
            dispatch_release(ident);
            sec_release(sec);
        },
        ^(nw_protocol_options_t tcp_options) { tcp_opts(tcp_options); });
    nw_parameters_set_include_peer_to_peer(params, true);
    return params;
}

/* Plain TCP for the lobby. */
static nw_parameters_t make_plain_params(void)
{
    nw_parameters_t params = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL,
        ^(nw_protocol_options_t tcp_options) { tcp_opts(tcp_options); });
    nw_parameters_set_include_peer_to_peer(params, true);
    return params;
}

/* ------------------------------------------------------------ framing */

static void send_frame(nw_connection_t conn, const char *json, size_t max)
{
    if (!conn || !json) return;
    size_t len = strlen(json);
    if (len == 0 || len > max) return;
    uint8_t *buf = (uint8_t *)malloc(4 + len);
    if (!buf) return;
    buf[0] = (uint8_t)(len >> 24);
    buf[1] = (uint8_t)(len >> 16);
    buf[2] = (uint8_t)(len >> 8);
    buf[3] = (uint8_t)(len);
    memcpy(buf + 4, json, len);
    dispatch_data_t d = dispatch_data_create(buf, 4 + len, net_q, DISPATCH_DATA_DESTRUCTOR_FREE);
    nw_connection_send(conn, d, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^(nw_error_t error) {
        (void)error;   /* a failed send is followed by a state change we handle */
    });
    dispatch_release(d);
}

static size_t max_frame(int chan) { return chan == CH_LOBBY ? WP_MAX_LOBBY_FRAME : WP_MAX_FRAME; }

static void deliver(nw_connection_t conn, int peer_id, int is_host, int chan, char *json)
{
    if (is_host) {
        peer_t *p = peer_by_id(peer_id, chan);
        if (!p || p->conn != conn || !p->ready) return;
        if (chan == CH_ROOM) { if (CB.message) CB.message(peer_id, json); }
        else if (CB.lobby_message) CB.lobby_message(peer_id, json);
    } else {
        if (conn != cli[chan].conn || !cli[chan].ready) return;
        if (chan == CH_ROOM) { if (CB.message) CB.message(0, json); }
        else if (CB.knock_message) CB.knock_message(json);
    }
}

/* The receive chain holds one retain on the connection from start to finish. */
static void recv_fail(nw_connection_t conn)
{
    dispatch_async(main_q, ^{
        nw_connection_cancel(conn);
        nw_release(conn);
    });
}

static void recv_next(nw_connection_t conn, int peer_id, int is_host, int chan)
{
    nw_connection_receive(conn, 4, 4,
        ^(dispatch_data_t header, nw_content_context_t ctx, bool is_complete, nw_error_t error) {
            (void)ctx; (void)is_complete;
            if (error || !header || dispatch_data_get_size(header) < 4) { recv_fail(conn); return; }
            uint8_t h[4];
            copy_out(header, h, 4);
            uint32_t len = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
            if (len == 0 || len > max_frame(chan)) { recv_fail(conn); return; }
            nw_connection_receive(conn, len, len,
                ^(dispatch_data_t body, nw_content_context_t ctx2, bool is_complete2, nw_error_t error2) {
                    (void)ctx2; (void)is_complete2;
                    if (error2 || !body || dispatch_data_get_size(body) < len) { recv_fail(conn); return; }
                    char *buf = (char *)malloc(len + 1);
                    if (!buf) { recv_fail(conn); return; }
                    copy_out(body, buf, len);
                    buf[len] = 0;
                    dispatch_async(main_q, ^{
                        deliver(conn, peer_id, is_host, chan, buf);
                        free(buf);
                    });
                    recv_next(conn, peer_id, is_host, chan);
                });
        });
}

static void recv_loop(nw_connection_t conn, int peer_id, int is_host, int chan)
{
    nw_retain(conn);
    recv_next(conn, peer_id, is_host, chan);
}

/* ------------------------------------------------ connection lifecycle */

static void client_report(int chan, wp_client_state s, const char *err)
{
    if (chan == CH_ROOM) { if (CB.client_state) CB.client_state(s, err); }
    else if (CB.knock_state) CB.knock_state(s, err);
}

static void client_fail(int chan, nw_connection_t conn, const char *err)
{
    if (conn != cli[chan].conn) return;
    cli[chan].conn = NULL;
    cli[chan].ready = 0;
    nw_connection_cancel(conn);                   /* cancelled follows and releases */
    client_report(chan, WP_CLIENT_FAILED, err);
}

static void on_conn_state(nw_connection_t conn, int peer_id, int is_host, int chan,
                          nw_connection_state_t state, int err_domain, const char *err)
{
    if (is_host) {
        peer_t *p = peer_by_id(peer_id, chan);
        if (!p || p->conn != conn) {
            if (state == nw_connection_state_cancelled) nw_release(conn);
            return;
        }
        switch (state) {
        case nw_connection_state_ready:
            p->ready = 1;
            recv_loop(conn, peer_id, 1, chan);
            if (chan == CH_ROOM) { if (CB.peer_joined) CB.peer_joined(peer_id); }
            else if (CB.lobby_joined) CB.lobby_joined(peer_id);
            break;
        case nw_connection_state_failed:
            nw_connection_cancel(conn);           /* cancelled follows */
            break;
        case nw_connection_state_cancelled: {
            int was_ready = p->ready;
            p->conn = NULL; p->ready = 0; p->id = 0;
            nw_release(conn);
            if (was_ready) {
                if (chan == CH_ROOM) { if (CB.peer_left) CB.peer_left(peer_id); }
                else if (CB.lobby_left) CB.lobby_left(peer_id);
            }
            break;
        }
        default:
            break;
        }
    } else {
        cli_t *c = &cli[chan];
        if (conn != c->conn) {
            if (state == nw_connection_state_cancelled) nw_release(conn);
            return;
        }
        switch (state) {
        case nw_connection_state_preparing:
            client_report(chan, WP_CLIENT_CONNECTING, NULL);
            break;
        case nw_connection_state_waiting:
            /* "waiting" means the framework would retry when the network changes.
             * A TLS failure will never get better: the invite code is wrong. */
            if (err_domain == (int)nw_error_domain_tls) { client_fail(chan, conn, err); break; }
            client_report(chan, WP_CLIENT_WAITING, err);
            {
                char *why = strdup(err ? err : "no response");
                int gen = c->gen;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), main_q, ^{
                    if (gen == cli[chan].gen && conn == cli[chan].conn && !cli[chan].ready) {
                        char msg[200];
                        snprintf(msg, sizeof msg, "could not reach the host (%s)", why);
                        client_fail(chan, conn, msg);
                    }
                    free(why);
                });
            }
            break;
        case nw_connection_state_ready:
            c->ready = 1;
            recv_loop(conn, 0, 0, chan);
            client_report(chan, WP_CLIENT_READY, NULL);
            break;
        case nw_connection_state_failed:
            client_fail(chan, conn, err);
            break;
        case nw_connection_state_cancelled: {
            int was_ready = c->ready;
            c->conn = NULL;
            c->ready = 0;
            nw_release(conn);
            client_report(chan, WP_CLIENT_CLOSED, was_ready ? NULL : "connection closed");
            break;
        }
        default:
            break;
        }
    }
}

static void start_connection(nw_connection_t conn, int peer_id, int is_host, int chan)
{
    nw_connection_set_queue(conn, net_q);
    nw_connection_set_state_changed_handler(conn, ^(nw_connection_state_t state, nw_error_t error) {
        char *err = NULL;
        int domain = 0;
        if (error) {
            char tmp[160];
            describe_error(error, tmp, sizeof tmp);
            err = strdup(tmp);
            domain = (int)nw_error_get_error_domain(error);
        }
        dispatch_async(main_q, ^{
            on_conn_state(conn, peer_id, is_host, chan, state, domain, err);
            free(err);
        });
    });
    nw_connection_start(conn);
}

/* ------------------------------------------------------------- public */

void wp_net_init(const wp_net_callbacks *cb)
{
    memset(&CB, 0, sizeof CB);
    if (cb) CB = *cb;
    if (!net_q) net_q = dispatch_queue_create("app.watchparty.net", DISPATCH_QUEUE_SERIAL);
    main_q = dispatch_get_main_queue();
}

void wp_net_shutdown(void)
{
    wp_net_host_stop();
    wp_net_disconnect();
    wp_net_knock_close();
    wp_net_browse_stop();
}

/* ---- listeners (room and lobby) ---- */

static int listen_start(int chan, nw_parameters_t params, const char *name, const char *type, uint16_t port)
{
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    nw_listener_t l = nw_listener_create_with_port(port_str, params);
    nw_release(params);
    if (!l) return 0;

    int gen = ++lst[chan].gen;
    lst[chan].l = l;
    lst[chan].port = 0;
    lst[chan].ready = 0;

    if (name && *name) {
        nw_advertise_descriptor_t ad = nw_advertise_descriptor_create_bonjour_service(name, type, NULL);
        if (ad) {
            nw_listener_set_advertise_descriptor(l, ad);
            nw_release(ad);
        }
    }

    nw_listener_set_queue(l, net_q);
    nw_listener_set_state_changed_handler(l, ^(nw_listener_state_t state, nw_error_t error) {
        char *err = NULL;
        if (error) {
            char tmp[160];
            describe_error(error, tmp, sizeof tmp);
            err = strdup(tmp);
        }
        uint16_t p = (state == nw_listener_state_ready) ? nw_listener_get_port(l) : 0;
        dispatch_async(main_q, ^{
            if (state == nw_listener_state_cancelled) {
                nw_release(l);
            } else if (gen == lst[chan].gen) {
                if (state == nw_listener_state_ready) {
                    lst[chan].port = p;
                    lst[chan].ready = 1;
                    if (chan == CH_ROOM && CB.host_state) CB.host_state(1, p, NULL);
                    if (chan == CH_LOBBY && CB.lobby_ready) CB.lobby_ready(p);
                } else if (state == nw_listener_state_failed) {
                    lst[chan].ready = 0;
                    if (chan == CH_ROOM && CB.host_state) CB.host_state(0, 0, err ? err : "listener failed");
                }
            }
            free(err);
        });
    });
    nw_listener_set_new_connection_handler(l, ^(nw_connection_t conn) {
        nw_retain(conn);
        dispatch_async(main_q, ^{
            int slot = -1;
            if (gen == lst[chan].gen) {
                int used = 0;
                for (int i = 0; i < N_SLOTS; i++) if (peers[i].id && peers[i].chan == chan) used++;
                int limit = chan == CH_ROOM ? WP_MAX_PEERS : WP_MAX_LOBBY;
                if (used < limit) for (int i = 0; i < N_SLOTS; i++) if (peers[i].id == 0) { slot = i; break; }
            }
            if (slot < 0) {
                nw_connection_cancel(conn);
                nw_release(conn);
                return;
            }
            peers[slot].id = next_peer_id++;
            peers[slot].chan = chan;
            peers[slot].conn = conn;     /* owns the retain */
            peers[slot].ready = 0;
            start_connection(conn, peers[slot].id, 1, chan);
        });
    });
    nw_listener_start(l);
    return 1;
}

static void listen_stop(int chan)
{
    if (lst[chan].l) {
        nw_listener_t l = lst[chan].l;
        lst[chan].l = NULL;
        lst[chan].ready = 0;
        lst[chan].port = 0;
        lst[chan].gen++;
        nw_listener_cancel(l);        /* released when the cancelled state arrives */
    }
    for (int i = 0; i < N_SLOTS; i++) {
        if (peers[i].id && peers[i].chan == chan && peers[i].conn) nw_connection_cancel(peers[i].conn);
    }
}

/* ---- hosting ---- */

int wp_net_host_start(const uint8_t *psk, size_t psk_len, const char *room_name,
                      uint16_t port, int advertise)
{
    listen_stop(CH_ROOM);
    return listen_start(CH_ROOM, make_params(psk, psk_len), advertise ? room_name : NULL, WP_SERVICE_TYPE, port);
}

void wp_net_host_stop(void)
{
    listen_stop(CH_ROOM);
    listen_stop(CH_LOBBY);
}

int wp_net_hosting(void) { return lst[CH_ROOM].l != NULL; }
uint16_t wp_net_host_port(void) { return lst[CH_ROOM].ready ? lst[CH_ROOM].port : 0; }

int wp_net_peer_count(void)
{
    int n = 0;
    for (int i = 0; i < N_SLOTS; i++) if (peers[i].id && peers[i].chan == CH_ROOM && peers[i].ready) n++;
    return n;
}

void wp_net_send_to(int peer_id, const char *json)
{
    peer_t *p = peer_by_id(peer_id, CH_ROOM);
    if (p && p->ready) send_frame(p->conn, json, WP_MAX_FRAME);
}

void wp_net_broadcast(const char *json, int except_peer_id)
{
    for (int i = 0; i < N_SLOTS; i++) {
        if (peers[i].id && peers[i].chan == CH_ROOM && peers[i].ready && peers[i].id != except_peer_id)
            send_frame(peers[i].conn, json, WP_MAX_FRAME);
    }
}

void wp_net_kick(int peer_id)
{
    peer_t *p = peer_by_id(peer_id, CH_ROOM);
    if (p && p->conn) nw_connection_cancel(p->conn);
}

/* ---- lobby (host) ---- */

int wp_net_lobby_start(const char *room_name)
{
    listen_stop(CH_LOBBY);
    return listen_start(CH_LOBBY, make_plain_params(), room_name, WP_LOBBY_TYPE, 0);
}

void wp_net_lobby_stop(void) { listen_stop(CH_LOBBY); }
uint16_t wp_net_lobby_port(void) { return lst[CH_LOBBY].ready ? lst[CH_LOBBY].port : 0; }

void wp_net_lobby_send(int lobby_id, const char *json)
{
    peer_t *p = peer_by_id(lobby_id, CH_LOBBY);
    if (p && p->ready) send_frame(p->conn, json, WP_MAX_LOBBY_FRAME);
}

void wp_net_lobby_close(int lobby_id)
{
    peer_t *p = peer_by_id(lobby_id, CH_LOBBY);
    if (p && p->conn) nw_connection_cancel(p->conn);
}

/* ---- client connections (room and knock) ---- */

static void client_close(int chan)
{
    if (cli[chan].conn) {
        nw_connection_t c = cli[chan].conn;
        cli[chan].conn = NULL;
        cli[chan].ready = 0;
        nw_connection_cancel(c);      /* released when the cancelled state arrives */
    }
}

static int client_open(int chan, nw_endpoint_t ep, nw_parameters_t params)
{
    client_close(chan);
    nw_connection_t c = nw_connection_create(ep, params);
    nw_release(params);
    if (!c) return 0;
    cli[chan].conn = c;
    cli[chan].ready = 0;
    cli[chan].gen++;
    start_connection(c, 0, 0, chan);
    return 1;
}

static int open_host(int chan, nw_parameters_t params, const char *host, const char *port)
{
    if (!host || !*host) { nw_release(params); return 0; }
    nw_endpoint_t ep = nw_endpoint_create_host(host, (port && *port) ? port : "47321");
    if (!ep) { nw_release(params); return 0; }
    int ok = client_open(chan, ep, params);
    nw_release(ep);
    return ok;
}

/* nth room that can be joined (has a room endpoint) */
static room_t *nth_room(int index)
{
    int n = 0;
    for (int i = 0; i < room_slots; i++) {
        if (!rooms[i].ep) continue;
        if (n == index) return &rooms[i];
        n++;
    }
    return NULL;
}

int wp_net_connect_host(const uint8_t *psk, size_t psk_len, const char *host, const char *port)
{
    return open_host(CH_ROOM, make_params(psk, psk_len), host, port);
}

int wp_net_connect_room(const uint8_t *psk, size_t psk_len, int room_index)
{
    room_t *r = nth_room(room_index);
    if (!r) return 0;
    return client_open(CH_ROOM, r->ep, make_params(psk, psk_len));
}

void wp_net_send_host(const char *json)
{
    if (cli[CH_ROOM].conn && cli[CH_ROOM].ready) send_frame(cli[CH_ROOM].conn, json, WP_MAX_FRAME);
}

void wp_net_disconnect(void) { client_close(CH_ROOM); }
int wp_net_connected(void) { return cli[CH_ROOM].conn != NULL && cli[CH_ROOM].ready; }

int wp_net_knock_room(int room_index)
{
    room_t *r = nth_room(room_index);
    if (!r || !r->lep) return 0;
    return client_open(CH_LOBBY, r->lep, make_plain_params());
}

int wp_net_knock_host(const char *host, const char *port)
{
    return open_host(CH_LOBBY, make_plain_params(), host, port);
}

void wp_net_knock_send(const char *json)
{
    if (cli[CH_LOBBY].conn && cli[CH_LOBBY].ready) send_frame(cli[CH_LOBBY].conn, json, WP_MAX_LOBBY_FRAME);
}

void wp_net_knock_close(void) { client_close(CH_LOBBY); }

/* ---- discovery ---- */

static void room_set(const char *name, nw_endpoint_t ep, int chan)   /* takes ownership of ep (may be NULL) */
{
    int i;
    for (i = 0; i < room_slots; i++) if (strcmp(rooms[i].name, name) == 0) break;
    if (i == room_slots) {
        if (!ep) return;
        if (room_slots >= WP_MAX_ROOMS) { nw_release(ep); return; }
        memset(&rooms[i], 0, sizeof rooms[i]);
        snprintf(rooms[i].name, sizeof rooms[i].name, "%s", name);
        room_slots++;
    }
    nw_endpoint_t *slot = chan == CH_ROOM ? &rooms[i].ep : &rooms[i].lep;
    if (*slot) nw_release(*slot);
    *slot = ep;
    if (!rooms[i].ep && !rooms[i].lep) {                 /* both gone: drop the entry */
        for (int j = i; j + 1 < room_slots; j++) rooms[j] = rooms[j + 1];
        room_slots--;
        memset(&rooms[room_slots], 0, sizeof rooms[room_slots]);
    }
    if (CB.rooms_changed) CB.rooms_changed();
}

static void rooms_clear(void)
{
    for (int i = 0; i < room_slots; i++) {
        if (rooms[i].ep) nw_release(rooms[i].ep);
        if (rooms[i].lep) nw_release(rooms[i].lep);
    }
    memset(rooms, 0, sizeof rooms);
    room_slots = 0;
}

static void browse_one(int chan, const char *type)
{
    if (browser[chan]) return;
    nw_browse_descriptor_t desc = nw_browse_descriptor_create_bonjour_service(type, NULL);
    nw_parameters_t params = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL,
                                                             NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_parameters_set_include_peer_to_peer(params, true);
    nw_browser_t b = nw_browser_create(desc, params);
    nw_release(desc);
    nw_release(params);
    if (!b) return;

    int gen = ++browser_gen[chan];
    browser[chan] = b;
    nw_browser_set_queue(b, net_q);
    nw_browser_set_browse_results_changed_handler(b,
        ^(nw_browse_result_t old_result, nw_browse_result_t new_result, bool batch_complete) {
            (void)batch_complete;
            nw_browse_result_change_t ch = nw_browse_result_get_changes(old_result, new_result);
            if ((ch & nw_browse_result_change_result_added) && new_result) {
                nw_endpoint_t ep = nw_browse_result_copy_endpoint(new_result);
                const char *n = ep ? nw_endpoint_get_bonjour_service_name(ep) : NULL;
                if (!ep || !n) { if (ep) nw_release(ep); return; }
                char *name = strdup(n);
                dispatch_async(main_q, ^{
                    if (gen == browser_gen[chan]) room_set(name, ep, chan); else nw_release(ep);
                    free(name);
                });
            } else if ((ch & nw_browse_result_change_result_removed) && old_result) {
                nw_endpoint_t ep = nw_browse_result_copy_endpoint(old_result);
                const char *n = ep ? nw_endpoint_get_bonjour_service_name(ep) : NULL;
                char *name = n ? strdup(n) : NULL;
                if (ep) nw_release(ep);
                if (!name) return;
                dispatch_async(main_q, ^{
                    if (gen == browser_gen[chan]) room_set(name, NULL, chan);
                    free(name);
                });
            }
        });
    nw_browser_set_state_changed_handler(b, ^(nw_browser_state_t state, nw_error_t error) {
        (void)error;
        if (state == nw_browser_state_cancelled) {
            dispatch_async(main_q, ^{ nw_release(b); });
        }
    });
    nw_browser_start(b);
}

void wp_net_browse_start(void)
{
    browse_one(CH_ROOM, WP_SERVICE_TYPE);
    browse_one(CH_LOBBY, WP_LOBBY_TYPE);
}

void wp_net_browse_stop(void)
{
    for (int c = 0; c < N_CH; c++) {
        if (browser[c]) {
            nw_browser_t b = browser[c];
            browser[c] = NULL;
            browser_gen[c]++;
            nw_browser_cancel(b);      /* released when the cancelled state arrives */
        }
    }
    rooms_clear();
}

int wp_net_room_count(void)
{
    int n = 0;
    for (int i = 0; i < room_slots; i++) if (rooms[i].ep) n++;
    return n;
}

const char *wp_net_room_name(int index)
{
    room_t *r = nth_room(index);
    return r ? r->name : NULL;
}

int wp_net_room_has_lobby(int index)
{
    room_t *r = nth_room(index);
    return r && r->lep;
}

int wp_net_room_find(const char *name)
{
    if (!name) return -1;
    int n = wp_net_room_count();
    for (int i = 0; i < n; i++) if (strcmp(wp_net_room_name(i), name) == 0) return i;
    return -1;
}
