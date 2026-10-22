/* wp_room.c — see wp_room.h */
#include "wp_room.h"
#include "wp_fancy.h"
#include "wp_json.h"
#include "wp_lobby.h"
#include "wp_net.h"

#include <dispatch/dispatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROTO_VER      1
#define KNOCK_WAIT_S   90          /* host: an unanswered request expires */
#define ASK_WAIT_S     120         /* guest: give up waiting for an answer */
#define MAX_ASKING     3           /* host: requests on screen at once */

const wp_reaction WP_REACTIONS[] = {
    { "🗿", "sigma", NULL },      { "✨", "+1000 aura", NULL },  { "🔥", "W", NULL },          { "💀", "L", NULL },
    { "🧢", "cap", NULL },        { "",  "ate", "ate" },         { "📉", "−aura", NULL },      { "🤷", "6-7", NULL },
    { "🎯", "locked in", NULL },  { "🐐", "GOAT", NULL },        { "🧠", "brainrot", NULL },   { "😮‍💨", "rizz", NULL },
    { "🫡", "say less", NULL },   { "🍳", "cooked", NULL },      { "🚽", "skibidi", NULL },    { "🍝", "bussin", NULL },
    { "💅", "it's giving", NULL },{ "🌈", "delulu", NULL },      { "🤢", "the ick", NULL },    { "🧍", "NPC", NULL },
    { "🫨", "crash out", NULL },  { "🌾", "touch grass", NULL }, { "👀", "chat, is this real", NULL }, { "🥶", "no cap", NULL },
};
const int WP_N_REACTIONS = (int)(sizeof WP_REACTIONS / sizeof WP_REACTIONS[0]);

typedef struct {
    int  id;                 /* wp_net peer id, 0 = empty */
    int  hello;
    char name[WP_NAME_MAX + 1];
} apeer_t;

typedef struct {
    int      id;             /* lobby connection id, 0 = free */
    int      asked;          /* knock received, waiting for the host */
    int      answered;
    char     name[WP_NAME_MAX + 1];
    uint8_t  key[32];
    char     check[5];
} knock_t;

typedef struct {
    const char *t;
    const char *video;
    double pos;
    int playing;
    int ended;
    const char *by;
    const char *text;
    int react;
    const char *sticker;
} cmd_t;

static wp_room_ui UI;
static wp_room_state R;

static struct {
    uint8_t  secret[WP_SECRET_LEN];
    uint8_t  psk[WP_PSK_LEN];
    int      port_fallback_tried;
    int      advertise;
    double   pos;
    uint64_t pos_at;
    int      have_pos;
    double   offset_ms;
    int      have_offset;
    uint64_t ping_t0;
    apeer_t  peers[WP_MAX_PEERS];
    knock_t  knocks[WP_MAX_LOBBY];
    /* guest asking */
    wp_eckey ask_key;
    uint8_t  ask_shared_key[32];
    int      ask_gen;
    char     ask_host[200], ask_room_port[16];    /* set when asking by address */
    int      timer_ticks;
    dispatch_source_t timer;
} G;

/* ------------------------------------------------------------ helpers */

static void say(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (UI.say) UI.say(buf);
}

static void note(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (UI.note) UI.note(buf); else if (UI.say) UI.say(buf);
}

static void js(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (UI.player) UI.player(buf);
}

static void changed(void) { if (UI.changed) UI.changed(); }
static void beep(void) { if (UI.beep) UI.beep(); }

void wp_sanitize_text(char *dst, size_t cap, const char *src, int allow_newlines)
{
    size_t o = 0;
    if (!cap) return;
    if (!src) src = "";
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < cap; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7f) {
            if (c == '\n' && allow_newlines) dst[o++] = '\n';
            else if (c == '\t' || c == '\n' || c == '\r') dst[o++] = ' ';
            continue;
        }
        dst[o++] = (char)c;
    }
    dst[o] = 0;
    while (o > 0 && (dst[o - 1] & 0xC0) == 0x80) {        /* never cut a UTF-8 sequence in half */
        size_t k = o - 1;
        while (k > 0 && (dst[k] & 0xC0) == 0x80) k--;
        int need = ((unsigned char)dst[k] >= 0xF0) ? 4 : ((unsigned char)dst[k] >= 0xE0) ? 3 : 2;
        if ((int)(o - k) >= need) break;
        dst[k] = 0; o = k;
    }
    wp_trim(dst);
}

void wp_sanitize_name(char *dst, size_t cap, const char *src)
{
    char tmp[256];
    wp_sanitize_text(tmp, sizeof tmp, src, 0);
    wp_fancy(tmp, dst, cap);                          /* names are always kept clean */
    if (!dst[0]) wp_strlcpy(dst, "Guest", cap);
}

const char *wp_room_fancy(const char *text, char *buf, size_t cap)
{
    if (!R.fancy) return text;
    wp_fancy(text, buf, cap);
    return buf;
}

void wp_format_time(double secs, char *out, size_t cap)
{
    if (secs < 0) secs = 0;
    int s = (int)secs;
    if (s >= 3600) snprintf(out, cap, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

int wp_room_watching(void)
{
    if (R.role == WP_ROLE_HOST) {
        int n = 1;
        for (int i = 0; i < WP_MAX_PEERS; i++) if (G.peers[i].id && G.peers[i].hello) n++;
        return n;
    }
    if (R.role == WP_ROLE_CLIENT) {
        int n = 0;
        for (const char *p = R.peers_text; *p; p++) if (*p == '\n') n++;
        return R.peers_text[0] ? n + 1 : 0;
    }
    return 0;
}

static void rebuild_peers_text(void)
{
    size_t o = (size_t)snprintf(R.peers_text, sizeof R.peers_text, "%s (host)", R.my_name);
    for (int i = 0; i < WP_MAX_PEERS; i++) {
        if (!G.peers[i].id || !G.peers[i].hello) continue;
        if (o + 2 + strlen(G.peers[i].name) >= sizeof R.peers_text) break;
        o += (size_t)snprintf(R.peers_text + o, sizeof R.peers_text - o, "\n%s", G.peers[i].name);
    }
}

static apeer_t *apeer_by_id(int id)
{
    if (id <= 0) return NULL;
    for (int i = 0; i < WP_MAX_PEERS; i++) if (G.peers[i].id == id) return &G.peers[i];
    return NULL;
}

static double current_pos(void)
{
    if (!G.have_pos) return 0;
    if (R.pstate == 1) return G.pos + (double)(wp_now_ms() - G.pos_at) / 1000.0;
    return G.pos;
}

static int is_playing(void) { return R.pstate == 1 || R.pstate == 3; }

/* ------------------------------------------------------------- messages */

static const char *build_cmd(wpj_w *w, const cmd_t *c)
{
    wpj_w_init(w);
    wpj_w_str(w, "t", c->t);
    if (c->video) wpj_w_str(w, "video", c->video);
    if (strcmp(c->t, "chat") != 0) {
        wpj_w_num(w, "pos", c->pos);
        wpj_w_bool(w, "playing", c->playing);
    }
    if (c->text) wpj_w_str(w, "text", c->text);
    if (c->react) wpj_w_bool(w, "react", 1);
    if (c->sticker) wpj_w_str(w, "sticker", c->sticker);
    wpj_w_str(w, "by", c->by ? c->by : "");
    return wpj_w_end(w);
}

static void send_cmd(const cmd_t *c)
{
    if (R.role == WP_ROLE_NONE) return;
    cmd_t mine = *c;
    mine.by = R.my_name;
    wpj_w w;
    const char *json = build_cmd(&w, &mine);
    if (R.role == WP_ROLE_HOST) wp_net_broadcast(json, 0);
    else wp_net_send_host(json);
    wpj_w_free(&w);
}

static void apply_cmd(const cmd_t *c)
{
    char tbuf[32];
    const char *by = (c->by && c->by[0]) ? c->by : "Someone";
    if (strcmp(c->t, "load") == 0) {
        if (!c->video || !wp_is_video_id(c->video)) return;
        wp_strlcpy(R.video, c->video, sizeof R.video);
        G.pos = c->pos; R.pstate = c->playing ? 1 : 2; G.pos_at = wp_now_ms(); G.have_pos = 1;
        js("wp.load('%s', %.3f, %s)", c->video, c->pos, c->playing ? "true" : "false");
        if (UI.video_loaded) UI.video_loaded(c->video);
        say("▶ %s loaded youtu.be/%s", by, c->video);
    } else if (strcmp(c->t, "play") == 0) {
        js("wp.play(%.3f)", c->pos);
        wp_format_time(c->pos, tbuf, sizeof tbuf);
        say("▶ %s pressed play at %s", by, tbuf);
    } else if (strcmp(c->t, "pause") == 0) {
        js("wp.pause(%.3f)", c->pos);
        wp_format_time(c->pos, tbuf, sizeof tbuf);
        say("⏸ %s paused at %s", by, tbuf);
    } else if (strcmp(c->t, "seek") == 0) {
        js("wp.seek(%.3f, %s)", c->pos, c->playing ? "true" : "false");
        wp_format_time(c->pos, tbuf, sizeof tbuf);
        say("↷ %s jumped to %s", by, tbuf);
    } else if (strcmp(c->t, "chat") == 0) {
        if (c->text && c->text[0]) {
            char fb[WP_TEXT_MAX * 3 + 64];
            say("%s: %s", by, wp_room_fancy(c->text, fb, sizeof fb));
            if (c->react && UI.sticker) UI.sticker(c->text, by, c->sticker);
        }
    }
}

static int sticker_name_ok(const char *s)
{
    if (!s || !s[0] || strlen(s) > 24) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
    return 1;
}

static int parse_cmd(const wpj_obj *o, cmd_t *c, char *by_buf, size_t by_cap, char *text_buf, size_t text_cap,
                     char *video_buf, size_t video_cap)
{
    memset(c, 0, sizeof *c);
    const char *st = wpj_str(o, "sticker");
    c->sticker = sticker_name_ok(st) ? st : NULL;
    c->t = wpj_str(o, "t");
    if (!c->t) return 0;
    c->pos = wpj_num(o, "pos", 0);
    if (c->pos < 0 || c->pos > 1e7) c->pos = 0;
    c->playing = wpj_bool(o, "playing", 1);
    c->ended = wpj_bool(o, "ended", 0);
    c->react = wpj_bool(o, "react", 0);
    wp_sanitize_name(by_buf, by_cap, wpj_str(o, "by"));
    c->by = by_buf;
    wp_sanitize_text(text_buf, text_cap, wpj_str(o, "text"), 0);
    c->text = text_buf;
    const char *v = wpj_str(o, "video");
    if (v && wp_is_video_id(v)) { wp_strlcpy(video_buf, v, video_cap); c->video = video_buf; }
    return 1;
}

static void host_send_tick(int peer_id /* 0 = all */)
{
    if (R.role != WP_ROLE_HOST || !R.video[0]) return;
    wpj_w w;
    wpj_w_init(&w);
    wpj_w_str(&w, "t", "tick");
    wpj_w_num(&w, "pos", current_pos());
    wpj_w_bool(&w, "playing", is_playing());
    wpj_w_bool(&w, "ended", R.pstate == 0);
    wpj_w_int(&w, "ht", (long long)wp_now_ms());
    const char *json = wpj_w_end(&w);
    if (peer_id) wp_net_send_to(peer_id, json); else wp_net_broadcast(json, 0);
    wpj_w_free(&w);
}

static void host_send_peers(void)
{
    rebuild_peers_text();
    wpj_w w;
    wpj_w_init(&w);
    wpj_w_str(&w, "t", "peers");
    wpj_w_str(&w, "text", R.peers_text);
    wp_net_broadcast(wpj_w_end(&w), 0);
    wpj_w_free(&w);
}

static void host_send_system(const char *text)
{
    wpj_w w;
    wpj_w_init(&w);
    wpj_w_str(&w, "t", "chat");
    wpj_w_str(&w, "text", text);
    wpj_w_str(&w, "by", "");
    wpj_w_bool(&w, "sys", 1);
    wp_net_broadcast(wpj_w_end(&w), 0);
    wpj_w_free(&w);
}

static void host_handle(int peer_id, const wpj_obj *o)
{
    apeer_t *p = apeer_by_id(peer_id);
    if (!p) return;
    const char *t = wpj_str(o, "t");
    if (!t) return;

    if (!p->hello) {
        if (strcmp(t, "hello") != 0 || (int)wpj_num(o, "ver", 0) != PROTO_VER) {
            wp_net_send_to(peer_id, "{\"t\":\"bye\",\"text\":\"This room needs a newer WatchParty.\"}");
            wp_net_kick(peer_id);
            return;
        }
        wp_sanitize_name(p->name, sizeof p->name, wpj_str(o, "name"));
        p->hello = 1;
        rebuild_peers_text();

        wpj_w w;
        wpj_w_init(&w);
        wpj_w_str(&w, "t", "welcome");
        wpj_w_str(&w, "name", R.my_name);
        wpj_w_str(&w, "room", R.room);
        wpj_w_str(&w, "video", R.video);
        wpj_w_num(&w, "pos", current_pos());
        wpj_w_bool(&w, "playing", is_playing());
        wpj_w_bool(&w, "ended", R.pstate == 0);
        wpj_w_int(&w, "ht", (long long)wp_now_ms());
        wpj_w_int(&w, "t0", (long long)wpj_num(o, "t0", 0));
        wpj_w_str(&w, "text", R.peers_text);
        wp_net_send_to(peer_id, wpj_w_end(&w));
        wpj_w_free(&w);

        host_send_peers();
        char msg[96];
        snprintf(msg, sizeof msg, "%s joined", p->name);
        host_send_system(msg);
        say("— %s joined —", p->name);
        changed();
        return;
    }

    if (strcmp(t, "ping") == 0) {
        wpj_w w;
        wpj_w_init(&w);
        wpj_w_str(&w, "t", "pong");
        wpj_w_int(&w, "t0", (long long)wpj_num(o, "t0", 0));
        wpj_w_int(&w, "ht", (long long)wp_now_ms());
        wp_net_send_to(peer_id, wpj_w_end(&w));
        wpj_w_free(&w);
        return;
    }
    if (strcmp(t, "sync") == 0) { host_send_tick(peer_id); return; }

    if (strcmp(t, "load") == 0 || strcmp(t, "play") == 0 || strcmp(t, "pause") == 0 ||
        strcmp(t, "seek") == 0 || strcmp(t, "chat") == 0) {
        cmd_t c;
        char by[WP_NAME_MAX + 1], text[WP_TEXT_MAX + 1], video[WP_VIDEO_ID_LEN + 1];
        if (!parse_cmd(o, &c, by, sizeof by, text, sizeof text, video, sizeof video)) return;
        c.by = p->name;                                  /* never trust the claimed sender */
        if (strcmp(t, "load") == 0 && !c.video) return;
        if (strcmp(t, "chat") == 0 && !text[0]) return;
        apply_cmd(&c);
        wpj_w w;
        const char *json = build_cmd(&w, &c);
        wp_net_broadcast(json, peer_id);
        wpj_w_free(&w);
    }
}

static void client_update_clock(double t0, double ht)
{
    uint64_t now = wp_now_ms();
    if (t0 <= 0 || (double)now < t0) return;
    double rtt = (double)now - t0;
    double offset = ht - (t0 + rtt / 2.0);
    G.offset_ms = G.have_offset ? (G.offset_ms * 0.5 + offset * 0.5) : offset;
    G.have_offset = 1;
}

static double host_pos_now(double pos, int playing, double ht)
{
    if (!playing || !G.have_offset) return pos;
    double elapsed = ((double)wp_now_ms() + G.offset_ms - ht) / 1000.0;
    if (elapsed < 0 || elapsed > 60) elapsed = 0;
    return pos + elapsed;
}

static void client_handle(const wpj_obj *o)
{
    const char *t = wpj_str(o, "t");
    if (!t) return;

    if (strcmp(t, "welcome") == 0) {
        wp_sanitize_name(R.host_name, sizeof R.host_name, wpj_str(o, "name"));
        char room[WP_ROOM_MAX + 1];
        wp_sanitize_text(room, sizeof room, wpj_str(o, "room"), 0);
        wp_fancy(room, R.room, sizeof R.room);
        if (!R.room[0]) wp_strlcpy(R.room, "(unnamed room)", sizeof R.room);
        wp_sanitize_text(R.peers_text, sizeof R.peers_text, wpj_str(o, "text"), 1);
        R.connecting = 0;
        client_update_clock(wpj_num(o, "t0", 0), wpj_num(o, "ht", 0));
        say("— Joined “%s” (hosted by %s) —", R.room, R.host_name);
        const char *v = wpj_str(o, "video");
        if (v && wp_is_video_id(v)) {
            int playing = wpj_bool(o, "playing", 0);
            cmd_t c = { "load", v, host_pos_now(wpj_num(o, "pos", 0), playing, wpj_num(o, "ht", 0)), playing, 0, R.host_name, NULL, 0, NULL };
            apply_cmd(&c);
        }
        changed();
        return;
    }
    if (strcmp(t, "peers") == 0) {
        wp_sanitize_text(R.peers_text, sizeof R.peers_text, wpj_str(o, "text"), 1);
        changed();
        return;
    }
    if (strcmp(t, "pong") == 0) {
        client_update_clock(wpj_num(o, "t0", 0), wpj_num(o, "ht", 0));
        return;
    }
    if (strcmp(t, "tick") == 0) {
        if (!R.video[0] || !R.player_ready) return;
        int playing = wpj_bool(o, "playing", 0);
        int ended = wpj_bool(o, "ended", 0);
        double pos = host_pos_now(wpj_num(o, "pos", 0), playing, wpj_num(o, "ht", 0));
        js("wp.applyTick(%.3f, %s, %s)", pos, playing ? "true" : "false", ended ? "true" : "false");
        return;
    }
    if (strcmp(t, "bye") == 0) {
        char text[WP_TEXT_MAX + 1];
        wp_sanitize_text(text, sizeof text, wpj_str(o, "text"), 0);
        say("Host says: %s", text[0] ? text : "goodbye");
        return;
    }
    if (strcmp(t, "chat") == 0 && wpj_bool(o, "sys", 0)) {
        char text[WP_TEXT_MAX + 1];
        wp_sanitize_text(text, sizeof text, wpj_str(o, "text"), 0);
        if (text[0]) say("— %s —", text);
        return;
    }
    if (strcmp(t, "load") == 0 || strcmp(t, "play") == 0 || strcmp(t, "pause") == 0 ||
        strcmp(t, "seek") == 0 || strcmp(t, "chat") == 0) {
        cmd_t c;
        char by[WP_NAME_MAX + 1], text[WP_TEXT_MAX + 1], video[WP_VIDEO_ID_LEN + 1];
        if (!parse_cmd(o, &c, by, sizeof by, text, sizeof text, video, sizeof video)) return;
        apply_cmd(&c);
    }
}

/* ------------------------------------------------- lobby: host side */

static knock_t *knock_by_id(int id)
{
    if (id <= 0) return NULL;
    for (int i = 0; i < WP_MAX_LOBBY; i++) if (G.knocks[i].id == id) return &G.knocks[i];
    return NULL;
}

int wp_room_pending_knocks(void)
{
    int n = 0;
    for (int i = 0; i < WP_MAX_LOBBY; i++) if (G.knocks[i].id && G.knocks[i].asked && !G.knocks[i].answered) n++;
    return n;
}

static void knock_forget(knock_t *k, int tell_ui)
{
    int id = k->id;
    int was_pending = k->asked && !k->answered;
    memset(k, 0, sizeof *k);
    if (tell_ui && was_pending && UI.knock_gone) UI.knock_gone(id);
}

static void lobby_close_later(int lobby_id)
{
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        wp_net_lobby_close(lobby_id);
    });
}

static void on_lobby_joined(int lobby_id)
{
    if (R.role != WP_ROLE_HOST) { wp_net_lobby_close(lobby_id); return; }
    knock_t *k = NULL;
    for (int i = 0; i < WP_MAX_LOBBY; i++) if (!G.knocks[i].id) { k = &G.knocks[i]; break; }
    if (!k) { wp_net_lobby_close(lobby_id); return; }
    memset(k, 0, sizeof *k);
    k->id = lobby_id;
    /* say something within 15 s or the door closes */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        knock_t *kk = knock_by_id(lobby_id);
        if (kk && !kk->asked) wp_net_lobby_close(lobby_id);
    });
}

static void on_lobby_left(int lobby_id)
{
    knock_t *k = knock_by_id(lobby_id);
    if (!k) return;
    if (k->asked && !k->answered) note("— %s stopped asking to join —", k->name);
    knock_forget(k, 1);
}

static void on_lobby_message(int lobby_id, const char *json)
{
    knock_t *k = knock_by_id(lobby_id);
    wpj_obj o;
    if (!k || R.role != WP_ROLE_HOST || !wpj_parse(json, &o)) { wp_net_lobby_close(lobby_id); return; }
    const char *t = wpj_str(&o, "t");
    if (!t || strcmp(t, "knock") != 0 || k->asked) { wp_net_lobby_close(lobby_id); return; }

    if (wp_room_pending_knocks() >= MAX_ASKING) {
        wp_net_lobby_send(lobby_id, "{\"t\":\"busy\"}");
        lobby_close_later(lobby_id);
        return;
    }
    uint8_t guest_pub[WP_EC_PUB_LEN + 4];
    if (wp_b64_decode(wpj_str(&o, "pub"), guest_pub, sizeof guest_pub) != WP_EC_PUB_LEN) { wp_net_lobby_close(lobby_id); return; }

    wp_eckey hk;
    uint8_t shared[32];
    if (!wp_ec_generate(&hk)) { wp_net_lobby_close(lobby_id); return; }
    int ok = wp_ec_shared(&hk, guest_pub, shared);
    if (ok) wp_lobby_key(shared, guest_pub, hk.pub, k->key);
    uint8_t host_pub[WP_EC_PUB_LEN];
    memcpy(host_pub, hk.pub, sizeof host_pub);
    wp_ec_free(&hk);
    memset(shared, 0, sizeof shared);
    if (!ok) { wp_net_lobby_close(lobby_id); return; }

    wp_sanitize_name(k->name, sizeof k->name, wpj_str(&o, "name"));
    wp_lobby_check_code(k->key, k->check);
    k->asked = 1;

    char pub_b64[128];
    wp_b64_encode(host_pub, sizeof host_pub, pub_b64, sizeof pub_b64);
    wpj_w w;
    wpj_w_init(&w);
    wpj_w_str(&w, "t", "knocked");
    wpj_w_str(&w, "room", R.room);
    wpj_w_str(&w, "pub", pub_b64);
    wpj_w_int(&w, "v", 1);
    wp_net_lobby_send(lobby_id, wpj_w_end(&w));
    wpj_w_free(&w);

    note("— %s is asking to join (check code %s) —", k->name, k->check);
    if (UI.knock) UI.knock(lobby_id, k->name, k->check);
    beep();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)KNOCK_WAIT_S * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        knock_t *kk = knock_by_id(lobby_id);
        if (kk && kk->asked && !kk->answered) wp_room_answer(lobby_id, 0);
    });
}

void wp_room_answer(int knock_id, int allow)
{
    knock_t *k = knock_by_id(knock_id);
    if (!k || !k->asked || k->answered) return;
    k->answered = 1;
    if (allow && R.role == WP_ROLE_HOST) {
        uint8_t box[WP_LOBBY_BOX_LEN];
        char b64[96];
        wp_lobby_seal(k->key, G.secret, box);
        wp_b64_encode(box, sizeof box, b64, sizeof b64);
        wpj_w w;
        wpj_w_init(&w);
        wpj_w_str(&w, "t", "allow");
        wpj_w_str(&w, "box", b64);
        wpj_w_int(&w, "port", wp_net_host_port());
        wp_net_lobby_send(knock_id, wpj_w_end(&w));
        wpj_w_free(&w);
        note("— You let %s in —", k->name);
    } else {
        wp_net_lobby_send(knock_id, "{\"t\":\"deny\"}");
        note("— You didn't let %s in —", k->name);
    }
    if (UI.knock_gone) UI.knock_gone(knock_id);
    memset(k->key, 0, sizeof k->key);
    lobby_close_later(knock_id);
}

static void knocks_clear(void)
{
    for (int i = 0; i < WP_MAX_LOBBY; i++) if (G.knocks[i].id) knock_forget(&G.knocks[i], 1);
}

/* ------------------------------------------------ lobby: guest side */

static int join_secret(const char *name, int nearby_index, const char *host, const char *port);

static void ask_end(void)
{
    R.ask = WP_ASK_NONE;
    G.ask_gen++;
    wp_ec_free(&G.ask_key);
    memset(G.ask_shared_key, 0, sizeof G.ask_shared_key);
    wp_net_knock_close();
    changed();
}

static int ask_begin(const char *name)
{
    if (R.role != WP_ROLE_NONE) wp_room_leave("— Left the room —");
    if (R.ask != WP_ASK_NONE) ask_end();
    wp_sanitize_name(R.my_name, sizeof R.my_name, name);
    if (!wp_ec_generate(&G.ask_key)) { say("Couldn't create a key for the request."); return 0; }
    R.ask = WP_ASK_CONNECTING;
    R.ask_check[0] = 0;
    int gen = ++G.ask_gen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ASK_WAIT_S * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (gen == G.ask_gen && R.ask != WP_ASK_NONE) {
            say("No answer from “%s” — try again, or ask for the invite code.", R.ask_room);
            ask_end();
        }
    });
    return 1;
}

int wp_room_ask(const char *name, int nearby_index)
{
    const char *rn = wp_room_nearby_name(nearby_index);
    if (!rn) return 0;
    if (!wp_room_nearby_can_ask(nearby_index)) {
        say("“%s” is running an older WatchParty that can't take requests — ask the host for the invite code.", rn);
        return 0;
    }
    char room[64];
    wp_strlcpy(room, rn, sizeof room);
    if (!ask_begin(name)) return 0;                   /* (may leave our own room first) */
    int net_index = wp_net_room_find(room);
    wp_strlcpy(R.ask_room, room, sizeof R.ask_room);
    G.ask_host[0] = 0;
    if (net_index < 0 || !wp_net_knock_room(net_index)) { say("Couldn't reach “%s”.", room); ask_end(); return 0; }
    say("Asking “%s” to let you in…", room);
    changed();
    return 1;
}

int wp_room_ask_addr(const char *name, const char *host, const char *lobby_port, const char *room_port)
{
    if (!ask_begin(name)) return 0;
    snprintf(R.ask_room, sizeof R.ask_room, "%s", host);
    wp_strlcpy(G.ask_host, host, sizeof G.ask_host);
    wp_strlcpy(G.ask_room_port, room_port, sizeof G.ask_room_port);
    if (!wp_net_knock_host(host, lobby_port)) { say("Couldn't reach %s.", host); ask_end(); return 0; }
    say("Asking %s to let you in…", host);
    changed();
    return 1;
}

void wp_room_ask_cancel(void)
{
    if (R.ask == WP_ASK_NONE) return;
    say("— Stopped asking to join “%s” —", R.ask_room);
    ask_end();
}

static void on_knock_state(wp_client_state s, const char *err)
{
    if (R.ask == WP_ASK_NONE) return;
    if (s == WP_CLIENT_READY) {
        char pub[128];
        wp_b64_encode(G.ask_key.pub, WP_EC_PUB_LEN, pub, sizeof pub);
        wpj_w w;
        wpj_w_init(&w);
        wpj_w_str(&w, "t", "knock");
        wpj_w_str(&w, "name", R.my_name);
        wpj_w_str(&w, "pub", pub);
        wpj_w_int(&w, "v", 1);
        wp_net_knock_send(wpj_w_end(&w));
        wpj_w_free(&w);
    } else if (s == WP_CLIENT_FAILED || s == WP_CLIENT_CLOSED) {
        say("Couldn't ask “%s”: %s", R.ask_room, err ? err : "the host closed the door");
        ask_end();
    }
}

static void on_knock_message(const char *json)
{
    wpj_obj o;
    if (R.ask == WP_ASK_NONE || !wpj_parse(json, &o)) return;
    const char *t = wpj_str(&o, "t");
    if (!t) return;
    if (strcmp(t, "knocked") == 0 && R.ask == WP_ASK_CONNECTING) {
        uint8_t host_pub[WP_EC_PUB_LEN + 4], shared[32];
        if (wp_b64_decode(wpj_str(&o, "pub"), host_pub, sizeof host_pub) != WP_EC_PUB_LEN ||
            !wp_ec_shared(&G.ask_key, host_pub, shared)) {
            say("The host's answer didn't make sense — try again.");
            ask_end();
            return;
        }
        wp_lobby_key(shared, G.ask_key.pub, host_pub, G.ask_shared_key);
        memset(shared, 0, sizeof shared);
        wp_lobby_check_code(G.ask_shared_key, R.ask_check);
        R.ask = WP_ASK_WAITING;
        say("Waiting for the host to let you in — your check code is %s.", R.ask_check);
        changed();
    } else if (strcmp(t, "allow") == 0 && R.ask == WP_ASK_WAITING) {
        uint8_t box[WP_LOBBY_BOX_LEN + 4], secret[WP_SECRET_LEN];
        if (wp_b64_decode(wpj_str(&o, "box"), box, sizeof box) != WP_LOBBY_BOX_LEN ||
            !wp_lobby_open(G.ask_shared_key, box, secret)) {
            say("The room key didn't check out — not joining.");
            ask_end();
            return;
        }
        char name[WP_NAME_MAX + 1], room[64], host[200], port[16];
        wp_strlcpy(name, R.my_name, sizeof name);
        wp_strlcpy(room, R.ask_room, sizeof room);
        wp_strlcpy(host, G.ask_host, sizeof host);
        wp_strlcpy(port, G.ask_room_port, sizeof port);
        int hp = (int)wpj_num(&o, "port", 0);          /* the host says where its room really is */
        if (hp > 0 && hp < 65536) snprintf(port, sizeof port, "%d", hp);
        ask_end();
        say("You're in — joining “%s”…", room);
        memcpy(G.secret, secret, sizeof secret);
        memset(secret, 0, sizeof secret);
        if (host[0]) join_secret(name, -1, host, port);
        else join_secret(name, wp_net_room_find(room), NULL, NULL);
    } else if (strcmp(t, "deny") == 0) {
        say("“%s” didn't let you in this time.", R.ask_room);
        ask_end();
    } else if (strcmp(t, "busy") == 0) {
        say("“%s” has too many requests right now — try again in a minute.", R.ask_room);
        ask_end();
    }
}

/* ------------------------------------------------------- room control */

static void reset_room_state(void)
{
    R.role = WP_ROLE_NONE;
    R.connecting = 0;
    G.have_offset = 0;
    R.room[0] = 0; R.host_name[0] = 0; R.addr[0] = 0; R.code[0] = 0;
    memset(G.peers, 0, sizeof G.peers);
    R.peers_text[0] = 0;
    memset(G.secret, 0, sizeof G.secret);
    memset(G.psk, 0, sizeof G.psk);
}

void wp_room_leave(const char *why)
{
    if (R.role == WP_ROLE_HOST) {
        host_send_system("The host closed the room");
        knocks_clear();
        wp_net_host_stop();
    } else if (R.role == WP_ROLE_CLIENT) {
        wp_net_disconnect();
    }
    int was = R.role;
    reset_room_state();
    if (was != WP_ROLE_NONE && why) say("%s", why);
    changed();
}

int wp_room_host_ex(const char *name, const char *room, uint16_t port, int advertise)
{
    if (R.ask != WP_ASK_NONE) ask_end();
    wp_room_leave("— Left the room —");
    wp_sanitize_name(R.my_name, sizeof R.my_name, name);
    char tmp[WP_ROOM_MAX + 1];
    wp_sanitize_text(tmp, sizeof tmp, room, 0);
    wp_fancy(tmp, R.room, sizeof R.room);
    if (!R.room[0]) snprintf(R.room, sizeof R.room, "%s's room", R.my_name);
    wp_random_bytes(G.secret, sizeof G.secret);
    wp_code_encode(G.secret, R.code);
    wp_derive_psk(G.secret, G.psk);
    R.role = WP_ROLE_HOST;
    G.port_fallback_tried = 0;
    G.advertise = advertise;
    rebuild_peers_text();
    if (!wp_net_host_start(G.psk, sizeof G.psk, R.room, port, advertise)) {
        note("Could not start hosting.");
        reset_room_state();
        changed();
        return 0;
    }
    say("— Hosting “%s” — opening the room… —", R.room);
    changed();
    return 1;
}

int wp_room_host(const char *name, const char *room) { return wp_room_host_ex(name, room, WP_DEFAULT_PORT, 1); }

static void split_address(const char *in, char *host, size_t hcap, char *port, size_t pcap)
{
    char buf[256];
    wp_strlcpy(buf, in, sizeof buf);
    wp_trim(buf);
    host[0] = 0;
    snprintf(port, pcap, "%d", WP_DEFAULT_PORT);
    if (buf[0] == '[') {                                 /* [ipv6]:port */
        char *end = strchr(buf, ']');
        if (!end) { wp_strlcpy(host, buf + 1, hcap); return; }
        *end = 0;
        wp_strlcpy(host, buf + 1, hcap);
        if (end[1] == ':' && end[2]) wp_strlcpy(port, end + 2, pcap);
        return;
    }
    char *colon = strchr(buf, ':');
    if (colon && !strchr(colon + 1, ':')) {              /* exactly one colon: host:port */
        *colon = 0;
        wp_strlcpy(host, buf, hcap);
        if (colon[1]) wp_strlcpy(port, colon + 1, pcap);
        return;
    }
    wp_strlcpy(host, buf, hcap);                         /* bare host, or bare ipv6 */
}

/* Join with G.secret already set: by nearby index (Bonjour) or host/port. */
static int join_secret(const char *name, int net_index, const char *host, const char *port)
{
    uint8_t secret[WP_SECRET_LEN];
    memcpy(secret, G.secret, sizeof secret);
    if (R.role != WP_ROLE_NONE) wp_room_leave("— Left the room —");
    wp_sanitize_name(R.my_name, sizeof R.my_name, name);
    memcpy(G.secret, secret, sizeof secret);
    memset(secret, 0, sizeof secret);
    wp_derive_psk(G.secret, G.psk);
    R.role = WP_ROLE_CLIENT;
    R.connecting = 1;
    int ok;
    if (host && host[0]) {
        snprintf(R.addr, sizeof R.addr, "%s:%s", host, (port && port[0]) ? port : "47321");
        ok = wp_net_connect_host(G.psk, sizeof G.psk, host, port);
    } else {
        const char *rn = wp_net_room_name(net_index);
        snprintf(R.addr, sizeof R.addr, "“%s” (nearby)", rn ? rn : "room");
        ok = net_index >= 0 && wp_net_connect_room(G.psk, sizeof G.psk, net_index);
    }
    if (!ok) { say("Could not start the connection."); reset_room_state(); changed(); return 0; }
    say("Connecting to %s…", R.addr);
    changed();
    return 1;
}

int wp_room_join(const char *name, int nearby_index, const char *address, const char *code)
{
    uint8_t secret[WP_SECRET_LEN];
    if (!wp_code_decode(code, secret)) {
        beep();
        say("That invite code doesn't look right — it has 24 letters/digits, like abcd-efgh-ijkl-mnop-qrst-uvwx.");
        return 0;
    }
    if (R.ask != WP_ASK_NONE) ask_end();
    memcpy(G.secret, secret, sizeof secret);
    memset(secret, 0, sizeof secret);
    if (nearby_index >= 0 && !(address && address[0])) {
        const char *rn = wp_room_nearby_name(nearby_index);
        return join_secret(name, rn ? wp_net_room_find(rn) : -1, NULL, NULL);
    }
    char host[200], port[16];
    split_address(address ? address : "", host, sizeof host, port, sizeof port);
    if (!host[0]) {
        say("Enter the host's address (for example 192.168.1.20 or 192.168.1.20:47321).");
        memset(G.secret, 0, sizeof G.secret);
        return 0;
    }
    return join_secret(name, -1, host, port);
}

void wp_room_invite_text(char *text, size_t cap)
{
    if (R.role != WP_ROLE_HOST) { wp_strlcpy(text, "", cap); return; }
    snprintf(text, cap,
             "Join my WatchParty room “%s”\n"
             "On the same Wi-Fi? Open WatchParty, find “%s” under Rooms nearby and tap Ask to join — or use this invite code:\n"
             "%s\n"
             "Elsewhere? Join with the address %s and the code.",
             R.room, R.room, R.code, R.addr[0] ? R.addr : "(not open yet)");
}

/* ------------------------------------------------------- nearby rooms */

static int nearby_skip(int net_index)              /* hide the room we are hosting */
{
    const char *n = wp_net_room_name(net_index);
    return R.role == WP_ROLE_HOST && n && strcmp(n, R.room) == 0;
}

static int nearby_to_net(int i)
{
    int n = wp_net_room_count(), k = 0;
    for (int j = 0; j < n; j++) {
        if (nearby_skip(j)) continue;
        if (k == i) return j;
        k++;
    }
    return -1;
}

int wp_room_nearby_count(void)
{
    int n = wp_net_room_count(), k = 0;
    for (int j = 0; j < n; j++) if (!nearby_skip(j)) k++;
    return k;
}

const char *wp_room_nearby_name(int i)
{
    int j = nearby_to_net(i);
    return j >= 0 ? wp_net_room_name(j) : NULL;
}

int wp_room_nearby_can_ask(int i)
{
    int j = nearby_to_net(i);
    return j >= 0 && wp_net_room_has_lobby(j);
}

/* -------------------------------------------------- watching together */

int wp_room_load(const char *input)
{
    char vid[WP_VIDEO_ID_LEN + 1];
    char trimmed[2048];
    wp_strlcpy(trimmed, input, sizeof trimmed);
    wp_trim(trimmed);
    if (!trimmed[0]) return 0;
    if (!wp_parse_youtube_id(trimmed, vid)) {
        beep();
        say("That doesn't look like a YouTube link. Paste something like https://www.youtube.com/watch?v=…");
        return 0;
    }
    static uint64_t last_load_at;                      /* Return + button can both fire */
    static char last_load_id[WP_VIDEO_ID_LEN + 1];
    uint64_t now = wp_now_ms();
    if (strcmp(vid, last_load_id) == 0 && now - last_load_at < 1500) return 1;
    wp_strlcpy(last_load_id, vid, sizeof last_load_id);
    last_load_at = now;

    cmd_t c = { "load", vid, 0, 1, 0, R.my_name, NULL, 0, NULL };
    apply_cmd(&c);
    send_cmd(&c);
    return 1;
}

void wp_room_chat(const char *text)
{
    char clean[WP_TEXT_MAX + 1];
    wp_sanitize_text(clean, sizeof clean, text, 0);
    if (!clean[0]) return;
    if (R.role == WP_ROLE_NONE) { say("You're not in a room yet — host or join one to chat."); return; }
    char fb[WP_TEXT_MAX * 3 + 64];
    say("%s: %s", R.my_name, wp_room_fancy(clean, fb, sizeof fb));     /* the room gets the original; */
    cmd_t c = { "chat", NULL, 0, 0, 0, R.my_name, clean, 0, NULL };    /* each screen applies its own setting */
    send_cmd(&c);
}

void wp_room_react(int index)
{
    if (index < 0 || index >= WP_N_REACTIONS) return;
    const wp_reaction *r = &WP_REACTIONS[index];
    char text[96];
    if (r->emoji[0]) snprintf(text, sizeof text, "%s %s", r->emoji, r->word);
    else wp_strlcpy(text, r->word, sizeof text);
    say("%s: %s", R.my_name, text);
    if (UI.sticker) UI.sticker(text, R.my_name, r->image);
    if (R.role == WP_ROLE_NONE) return;
    cmd_t c = { "chat", NULL, 0, 0, 0, R.my_name, text, 1, r->image };
    send_cmd(&c);
}

void wp_room_sync_now(void)
{
    if (R.role == WP_ROLE_HOST) { host_send_tick(0); say("Sent your position to everyone."); }
    else if (R.role == WP_ROLE_CLIENT) { wp_net_send_host("{\"t\":\"sync\"}"); say("Asked the host for its position."); }
}

void wp_room_player_event(const char *json)
{
    wpj_obj o;
    if (!wpj_parse(json, &o)) return;
    const char *ev = wpj_str(&o, "ev");
    if (!ev) return;

    if (strcmp(ev, "ready") == 0) { R.player_ready = 1; return; }
    if (strcmp(ev, "tick") == 0) {
        G.pos = wpj_num(&o, "pos", 0);
        R.pstate = (int)wpj_num(&o, "s", -1);
        G.pos_at = wp_now_ms();
        G.have_pos = 1;
        return;
    }
    if (strcmp(ev, "state") == 0) {
        int s = (int)wpj_num(&o, "s", -1);
        double pos = wpj_num(&o, "pos", 0);
        G.pos = pos; R.pstate = s; G.pos_at = wp_now_ms(); G.have_pos = 1;
        if (s == 1) { cmd_t c = { "play", NULL, pos, 1, 0, R.my_name, NULL, 0, NULL }; send_cmd(&c); }
        else if (s == 2) { cmd_t c = { "pause", NULL, pos, 0, 0, R.my_name, NULL, 0, NULL }; send_cmd(&c); }
        return;
    }
    if (strcmp(ev, "seek") == 0) {
        double pos = wpj_num(&o, "pos", 0);
        int playing = wpj_bool(&o, "playing", 0);
        G.pos = pos; R.pstate = playing ? 1 : 2; G.pos_at = wp_now_ms(); G.have_pos = 1;
        cmd_t c = { "seek", NULL, pos, playing, 0, R.my_name, NULL, 0, NULL };
        send_cmd(&c);
        return;
    }
    if (strcmp(ev, "error") == 0) {
        const char *code = wpj_str(&o, "code");
        int n = (int)wpj_num(&o, "code", 0);
        if (code && strcmp(code, "api") == 0) say("Couldn't load the YouTube player — is the internet connection up?");
        else if (n == 2) say("YouTube rejected that video id.");
        else if (n == 100) say("That video wasn't found (removed or private).");
        else if (n == 101 || n == 150) say("The video's owner doesn't allow it to be embedded. Try a different video.");
        else if (n == 5) say("The YouTube player hit a playback error.");
        else say("YouTube player error %d.", n);
    }
}

/* ------------------------------------------------ network callbacks */

static void cb_host_state(int listening, uint16_t port, const char *error)
{
    if (R.role != WP_ROLE_HOST) return;
    if (listening) {
        char ip[64];
        if (wp_local_ipv4(ip, sizeof ip)) snprintf(R.addr, sizeof R.addr, "%s:%u", ip, (unsigned)port);
        else snprintf(R.addr, sizeof R.addr, "port %u (no network?)", (unsigned)port);
        wp_net_lobby_start(G.advertise ? R.room : NULL);  /* the "ask to join" door */
        say("Room is open at %s — people nearby can ask to join.", R.addr);
        changed();
    } else if (!G.port_fallback_tried) {
        G.port_fallback_tried = 1;
        note("Port %d is busy — picking another one.", WP_DEFAULT_PORT);
        if (!wp_net_host_start(G.psk, sizeof G.psk, R.room, 0, G.advertise)) {
            say("Could not start hosting.");
            knocks_clear(); wp_net_host_stop(); reset_room_state(); changed();
        }
    } else {
        say("Could not open the room: %s", error ? error : "unknown error");
        knocks_clear();
        wp_net_host_stop();                            /* also closes the ask-to-join door */
        reset_room_state();
        changed();
    }
}

static void cb_peer_joined(int peer_id)
{
    if (R.role != WP_ROLE_HOST) { wp_net_kick(peer_id); return; }
    for (int i = 0; i < WP_MAX_PEERS; i++) {
        if (G.peers[i].id == 0) { G.peers[i].id = peer_id; G.peers[i].hello = 0; G.peers[i].name[0] = 0; return; }
    }
    wp_net_kick(peer_id);
}

static void cb_peer_left(int peer_id)
{
    apeer_t *p = apeer_by_id(peer_id);
    if (p && R.role == WP_ROLE_HOST && p->hello) {
        char msg[96];
        snprintf(msg, sizeof msg, "%s left", p->name);
        say("— %s left —", p->name);
        memset(p, 0, sizeof *p);
        host_send_peers();
        host_send_system(msg);
        changed();
    } else if (p) memset(p, 0, sizeof *p);
}

static void cb_message(int peer_id, const char *json)
{
    wpj_obj o;
    if (wpj_parse(json, &o)) {
        if (R.role == WP_ROLE_HOST && peer_id > 0) host_handle(peer_id, &o);
        else if (R.role == WP_ROLE_CLIENT && peer_id == 0) client_handle(&o);
    } else if (R.role == WP_ROLE_HOST && peer_id > 0) {
        wp_net_kick(peer_id);                            /* garbage in, peer out */
    }
}

static void cb_client_state(wp_client_state state, const char *error)
{
    if (R.role != WP_ROLE_CLIENT) return;
    switch (state) {
    case WP_CLIENT_CONNECTING:
        break;
    case WP_CLIENT_WAITING:
        note("Waiting for a network path%s%s", error ? ": " : "…", error ? error : "");
        break;
    case WP_CLIENT_READY: {
        G.ping_t0 = wp_now_ms();
        wpj_w w;
        wpj_w_init(&w);
        wpj_w_str(&w, "t", "hello");
        wpj_w_str(&w, "name", R.my_name);
        wpj_w_int(&w, "ver", PROTO_VER);
        wpj_w_int(&w, "t0", (long long)G.ping_t0);
        wp_net_send_host(wpj_w_end(&w));
        wpj_w_free(&w);
        note("Secure connection established — joining…");
        break;
    }
    case WP_CLIENT_FAILED:
        say("Could not join: %s", error ? error : "connection failed");
        reset_room_state();
        changed();
        break;
    case WP_CLIENT_CLOSED:
        say("— Disconnected from the room —");
        reset_room_state();
        changed();
        break;
    }
}

static void cb_rooms_changed(void) { if (UI.rooms_changed) UI.rooms_changed(); }

static void on_timer(void)
{
    G.timer_ticks++;
    if (R.role == WP_ROLE_HOST && (G.timer_ticks % 2) == 0 && wp_net_peer_count() > 0) host_send_tick(0);
    if (R.role == WP_ROLE_CLIENT && !R.connecting && (G.timer_ticks % 5) == 0 && wp_net_connected()) {
        wpj_w w;
        wpj_w_init(&w);
        wpj_w_str(&w, "t", "ping");
        wpj_w_int(&w, "t0", (long long)wp_now_ms());
        wp_net_send_host(wpj_w_end(&w));
        wpj_w_free(&w);
    }
}

/* Network callbacks arrive on the main queue but outside any run-loop pass that
 * drains autorelease pools, and the UI callbacks they trigger make ObjC objects. */
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);
#define POOLED(call) do { void *pool_ = objc_autoreleasePoolPush(); call; objc_autoreleasePoolPop(pool_); } while (0)

static void p_host_state(int l, uint16_t port, const char *e) { POOLED(cb_host_state(l, port, e)); }
static void p_peer_joined(int id) { POOLED(cb_peer_joined(id)); }
static void p_peer_left(int id) { POOLED(cb_peer_left(id)); }
static void p_message(int id, const char *json) { POOLED(cb_message(id, json)); }
static void p_client_state(wp_client_state s, const char *e) { POOLED(cb_client_state(s, e)); }
static void p_rooms_changed(void) { POOLED(cb_rooms_changed()); }
static void p_lobby_joined(int id) { POOLED(on_lobby_joined(id)); }
static void p_lobby_message(int id, const char *json) { POOLED(on_lobby_message(id, json)); }
static void p_lobby_left(int id) { POOLED(on_lobby_left(id)); }
static void p_lobby_ready(uint16_t port) { (void)port; POOLED(changed()); }
static void p_knock_state(wp_client_state s, const char *e) { POOLED(on_knock_state(s, e)); }
static void p_knock_message(const char *json) { POOLED(on_knock_message(json)); }
static void p_timer(void) { POOLED(on_timer()); }

/* ---------------------------------------------------------------- init */

wp_room_state *wp_room(void) { return &R; }

void wp_room_shutdown(void)
{
    if (R.ask != WP_ASK_NONE) ask_end();
    if (R.role == WP_ROLE_HOST) host_send_system("The host quit WatchParty");
    wp_net_shutdown();
}

void wp_room_init(const wp_room_ui *ui, const char *my_name)
{
    memset(&UI, 0, sizeof UI);
    if (ui) UI = *ui;
    memset(&R, 0, sizeof R);
    memset(&G, 0, sizeof G);
    R.pstate = -1;
    R.fancy = 1;
    wp_sanitize_name(R.my_name, sizeof R.my_name, my_name);

    wp_net_callbacks cb = {
        .host_state = p_host_state,
        .peer_joined = p_peer_joined,
        .peer_left = p_peer_left,
        .message = p_message,
        .client_state = p_client_state,
        .rooms_changed = p_rooms_changed,
        .lobby_joined = p_lobby_joined,
        .lobby_message = p_lobby_message,
        .lobby_left = p_lobby_left,
        .lobby_ready = p_lobby_ready,
        .knock_state = p_knock_state,
        .knock_message = p_knock_message,
    };
    wp_net_init(&cb);
    wp_net_browse_start();

    G.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(G.timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, 200 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(G.timer, ^{ p_timer(); });
    dispatch_resume(G.timer);
}
