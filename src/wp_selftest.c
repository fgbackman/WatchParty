/* wp_selftest.c — headless end-to-end check of the encrypted transport.
 *
 *   WatchParty.app/Contents/MacOS/WatchParty --selftest
 *
 * 1. Starts a host on an ephemeral port (no Bonjour, so no permission prompt).
 * 2. Connects a client with the right invite code: hello/welcome round trip,
 *    then a 40 KB frame (spans several TLS records).
 * 3. Connects a client with a WRONG invite code: must be rejected by TLS,
 *    and the host must never report it as joined. */
#include "wp_app.h"
#include "wp_net.h"
#include "wp_util.h"
#include "wp_json.h"

#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG_LEN 40000

static uint8_t good_psk[WP_PSK_LEN], bad_psk[WP_PSK_LEN];
static int stage;                   /* 1 = good client, 2 = bad client */
static int joined_count;
static int bad_client_rejected;

static void finish(int ok, const char *msg)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    fflush(stdout);
    exit(ok ? 0 : 1);
}

static void connect_with(const uint8_t *psk)
{
    char port[16];
    snprintf(port, sizeof port, "%u", (unsigned)wp_net_host_port());
    if (!wp_net_connect_host(psk, WP_PSK_LEN, "127.0.0.1", port)) finish(0, "wp_net_connect_host returned 0");
}

static void host_state(int listening, uint16_t port, const char *error)
{
    if (!listening) { printf("host failed: %s\n", error ? error : "?"); finish(0, "listener"); }
    printf("[selftest] host listening on 127.0.0.1:%u (TLS 1.2, PSK)\n", (unsigned)port);
    stage = 1;
    connect_with(good_psk);
}

static void peer_joined(int id)
{
    joined_count++;
    printf("[selftest] host: peer %d completed the TLS handshake\n", id);
    if (stage == 2) finish(0, "host accepted a client with the WRONG invite code");
}

static void peer_left(int id) { printf("[selftest] host: peer %d left\n", id); }

static void message(int peer_id, const char *json)
{
    wpj_obj o;
    if (!wpj_parse(json, &o)) finish(0, "received malformed JSON");
    const char *t = wpj_str(&o, "t");
    if (!t) finish(0, "message without type");

    if (peer_id > 0) {                                   /* host side */
        if (strcmp(t, "hello") == 0) {
            printf("[selftest] host: hello from '%s'\n", wpj_str(&o, "name") ? wpj_str(&o, "name") : "?");
            wp_net_send_to(peer_id, "{\"t\":\"welcome\",\"name\":\"host\"}");
        } else if (strcmp(t, "big") == 0) {
            const char *s = wpj_str(&o, "text");
            int ok = s && strlen(s) == BIG_LEN;
            for (size_t i = 0; ok && i < BIG_LEN; i++) if (s[i] != (char)('a' + (i % 26))) ok = 0;
            wp_net_send_to(peer_id, ok ? "{\"t\":\"bigok\"}" : "{\"t\":\"bigbad\"}");
        }
    } else {                                             /* client side */
        if (strcmp(t, "welcome") == 0) {
            printf("[selftest] client: welcome received — sending a %d-byte frame\n", BIG_LEN);
            char *big = (char *)malloc(BIG_LEN + 1);
            for (size_t i = 0; i < BIG_LEN; i++) big[i] = (char)('a' + (i % 26));
            big[BIG_LEN] = 0;
            wpj_w w;
            wpj_w_init(&w);
            wpj_w_str(&w, "t", "big");
            wpj_w_str(&w, "text", big);
            wp_net_send_host(wpj_w_end(&w));
            wpj_w_free(&w);
            free(big);
        } else if (strcmp(t, "bigok") == 0) {
            printf("[selftest] client: large frame round-trip OK\n");
            printf("[selftest] now connecting with a WRONG invite code...\n");
            stage = 2;
            wp_net_disconnect();
            connect_with(bad_psk);
        } else if (strcmp(t, "bigbad") == 0) {
            finish(0, "large frame corrupted");
        }
    }
}

static void client_state(wp_client_state s, const char *error)
{
    static const char *names[] = { "connecting", "waiting", "ready", "failed", "closed" };
    printf("[selftest] client: %s%s%s\n", names[s], error ? " — " : "", error ? error : "");
    if (stage == 1) {
        if (s == WP_CLIENT_READY) wp_net_send_host("{\"t\":\"hello\",\"name\":\"tester\",\"ver\":1}");
        else if (s == WP_CLIENT_FAILED || s == WP_CLIENT_CLOSED) finish(0, "client with the RIGHT code could not connect");
    } else if (stage == 2) {
        if (s == WP_CLIENT_READY) finish(0, "client with the WRONG invite code was accepted");
        if ((s == WP_CLIENT_FAILED || s == WP_CLIENT_CLOSED) && !bad_client_rejected) {
            bad_client_rejected = 1;
            /* give the host side a moment to make sure it never reports a join */
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 700 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                if (joined_count != 1) finish(0, "host joined-count is not 1");
                finish(1, "TLS-PSK transport OK (right code accepted, wrong code rejected, 40 KB frame intact)");
            });
        }
    }
}

/* ---------------------------------------------------------------------
 * Robot guest: joins a LIVE room headlessly and exercises the protocol.
 *   WatchParty --join <host[:port]> <invite-code> [name]
 * Says hello, prints the welcome, chats, loads a video, then leaves. */

static char robot_name[64] = "Robot";
static int robot_stage;

static void robot_client_state(wp_client_state s, const char *error)
{
    static const char *names[] = { "connecting", "waiting", "ready", "failed", "closed" };
    printf("[robot] %s%s%s\n", names[s], error ? " — " : "", error ? error : "");
    if (s == WP_CLIENT_READY) {
        wpj_w w;
        wpj_w_init(&w);
        wpj_w_str(&w, "t", "hello");
        wpj_w_str(&w, "name", robot_name);
        wpj_w_int(&w, "ver", 1);
        wpj_w_int(&w, "t0", (long long)wp_now_ms());
        wp_net_send_host(wpj_w_end(&w));
        wpj_w_free(&w);
    } else if (s == WP_CLIENT_FAILED) {
        finish(0, "could not join the room");
    } else if (s == WP_CLIENT_CLOSED) {
        finish(robot_stage >= 2, robot_stage >= 2 ? "robot guest finished" : "connection closed early");
    }
}

static void robot_message(int peer_id, const char *json)
{
    (void)peer_id;
    wpj_obj o;
    if (!wpj_parse(json, &o)) { printf("[robot] unparseable message\n"); return; }
    const char *t = wpj_str(&o, "t");
    if (!t) return;
    if (strcmp(t, "welcome") == 0) {
        printf("[robot] welcome: room '%s' hosted by '%s', video '%s', pos %.1f, playing %d\n",
               wpj_str(&o, "room") ? wpj_str(&o, "room") : "?", wpj_str(&o, "name") ? wpj_str(&o, "name") : "?",
               wpj_str(&o, "video") ? wpj_str(&o, "video") : "", wpj_num(&o, "pos", 0), wpj_bool(&o, "playing", 0));
        printf("[robot] watching:\n%s\n", wpj_str(&o, "text") ? wpj_str(&o, "text") : "");
        robot_stage = 1;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            wp_net_send_host("{\"t\":\"chat\",\"text\":\"Hello from the robot guest — the room works!\"}");
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            wp_net_send_host("{\"t\":\"load\",\"video\":\"wZhc5Kq7tdg\",\"pos\":0,\"playing\":true}");
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            wp_net_send_host("{\"t\":\"pause\",\"pos\":5,\"playing\":false}");
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 11 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            wp_net_send_host("{\"t\":\"sync\"}");
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 14 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            robot_stage = 2;
            wp_net_disconnect();
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                finish(1, "robot guest finished");
            });
        });
    } else if (strcmp(t, "tick") == 0) {
        printf("[robot] tick: pos %.1f playing %d ended %d\n", wpj_num(&o, "pos", 0), wpj_bool(&o, "playing", 0), wpj_bool(&o, "ended", 0));
    } else if (strcmp(t, "peers") == 0) {
        printf("[robot] peers now:\n%s\n", wpj_str(&o, "text") ? wpj_str(&o, "text") : "");
    } else if (strcmp(t, "chat") == 0) {
        printf("[robot] chat from '%s': %s\n", wpj_str(&o, "by") ? wpj_str(&o, "by") : "", wpj_str(&o, "text") ? wpj_str(&o, "text") : "");
    } else {
        printf("[robot] %s: %s\n", t, json);
    }
}

int wp_robot_main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s --join <host[:port]> <invite-code> [name]\n", argv[0]); return 2; }
    uint8_t secret[WP_SECRET_LEN], psk[WP_PSK_LEN];
    if (!wp_code_decode(argv[3], secret)) { fprintf(stderr, "bad invite code\n"); return 2; }
    wp_derive_psk(secret, psk);
    if (argc > 4) snprintf(robot_name, sizeof robot_name, "%s", argv[4]);

    char host[200], port[16] = "47321";
    snprintf(host, sizeof host, "%s", argv[2]);
    char *colon = strrchr(host, ':');
    if (colon && !strchr(host, '[')) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); }

    wp_net_callbacks cb = { .message = robot_message, .client_state = robot_client_state };
    wp_net_init(&cb);
    printf("[robot] joining %s:%s as '%s'\n", host, port, robot_name);
    if (!wp_net_connect_host(psk, WP_PSK_LEN, host, port)) finish(0, "connect failed");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        finish(0, "timed out");
    });
    dispatch_main();
    return 1;
}

int wp_selftest_main(void)
{
    uint8_t s1[WP_SECRET_LEN], s2[WP_SECRET_LEN];
    wp_random_bytes(s1, sizeof s1);
    wp_random_bytes(s2, sizeof s2);
    wp_derive_psk(s1, good_psk);
    wp_derive_psk(s2, bad_psk);

    wp_net_callbacks cb = {
        .host_state = host_state,
        .peer_joined = peer_joined,
        .peer_left = peer_left,
        .message = message,
        .client_state = client_state,
        .rooms_changed = NULL,
    };
    wp_net_init(&cb);
    if (!wp_net_host_start(good_psk, WP_PSK_LEN, "selftest", 0, 0)) finish(0, "wp_net_host_start returned 0");

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        finish(0, "timed out after 20 s");
    });
    dispatch_main();
    return 1;
}

/* ------------------------------------------------------------------------
 * --selftest-lobby: "ask to join", end to end, through the real room engine.
 *
 *   host (this process)                     guest (a second copy, --ask-test)
 *   hosts an unadvertised room      ─────▶  knocks on the lobby port
 *   sees the request, says NO               is told no, knocks again
 *   sees it again, says YES, sends the      opens the sealed room key, joins the
 *   room key sealed with ECDH               TLS room, says "check NNNN" in chat
 *   checks NNNN == its own check code  ◀──  … and the room works.
 * ---------------------------------------------------------------------- */
#include "wp_room.h"
#include <mach-o/dyld.h>
#include <spawn.h>
#include <signal.h>
extern char **environ;

static int  lt_knocks;
static char lt_check[8];
static int  lt_spawned;
static pid_t lt_child;

static void lt_finish(int ok, const char *msg)
{
    if (lt_child > 0) kill(lt_child, SIGTERM);
    finish(ok, msg);
}

static void lt_note(const char *line) { printf("[host] %s\n", line); fflush(stdout); }

static void lt_say(const char *line)
{
    lt_note(line);
    const char *p = strstr(line, ": check ");
    if (!p) return;
    if (strncmp(p + 8, lt_check, 4) == 0) lt_finish(1, "guest asked, was refused, asked again, was let in, joined over TLS, and both sides agree on the check code");
    else lt_finish(0, "check codes differ between host and guest");
}

static void lt_changed(void)
{
    const wp_room_state *r = wp_room();
    if (lt_spawned || r->role != WP_ROLE_HOST || !wp_net_lobby_port() || !wp_net_host_port()) return;
    lt_spawned = 1;
    char self[1024];
    uint32_t sz = sizeof self;
    if (_NSGetExecutablePath(self, &sz) != 0) lt_finish(0, "no executable path");
    char lp[16], rp[16];
    snprintf(lp, sizeof lp, "%u", (unsigned)wp_net_lobby_port());
    snprintf(rp, sizeof rp, "%u", (unsigned)wp_net_host_port());
    printf("[host] room on port %s, lobby on port %s — starting the guest\n", rp, lp);
    char *args[] = { self, "--ask-test", "127.0.0.1", lp, rp, NULL };
    if (posix_spawn(&lt_child, self, NULL, NULL, args, environ) != 0) lt_finish(0, "could not start the guest");
}

static void lt_knock(int id, const char *name, const char *check)
{
    lt_knocks++;
    printf("[host] knock #%d from %s, check code %s\n", lt_knocks, name, check);
    if (strlen(check) != 4) lt_finish(0, "check code is not 4 digits");
    if (lt_knocks == 1) { wp_room_answer(id, 0); return; }
    wp_strlcpy(lt_check, check, sizeof lt_check);
    wp_room_answer(id, 1);
}

int wp_lobbytest_main(void)
{
    wp_room_ui ui = { .say = lt_say, .note = lt_note, .changed = lt_changed, .knock = lt_knock };
    wp_room_init(&ui, "Host");
    wp_net_browse_stop();                         /* loopback only: no Bonjour, no permission prompt */
    if (!wp_room_host_ex("Host", "Lobby test", 0, 0)) finish(0, "could not host");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ lt_finish(0, "timed out"); });
    dispatch_main();
    return 1;
}

/* the guest half */
static const char *at_host, *at_lport, *at_rport;
static int at_asks, at_said;

static void at_ask(void) { at_asks++; wp_room_ask_addr("Guest", at_host, at_lport, at_rport); }

static void at_say(const char *line)
{
    printf("  [guest] %s\n", line);
    fflush(stdout);
    if (strstr(line, "didn't let you in") && at_asks == 1) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ at_ask(); });
    }
}

static char at_last_check[8];
static void at_changed_track(void)
{
    const wp_room_state *r = wp_room();
    if (r->ask == WP_ASK_WAITING && r->ask_check[0]) wp_strlcpy(at_last_check, r->ask_check, sizeof at_last_check);
    if (r->role == WP_ROLE_CLIENT && !r->connecting && !at_said) {
        at_said = 1;
        char msg[32];
        snprintf(msg, sizeof msg, "check %s", at_last_check[0] ? at_last_check : "????");
        wp_room_chat(msg);
    }
}

int wp_asktest_main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: --ask-test host lobby-port room-port\n"); return 2; }
    at_host = argv[2]; at_lport = argv[3]; at_rport = argv[4];
    wp_room_ui ui = { .say = at_say, .note = at_say, .changed = at_changed_track };
    wp_room_init(&ui, "Guest");
    wp_net_browse_stop();
    at_ask();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ exit(1); });
    dispatch_main();
    return 1;
}
