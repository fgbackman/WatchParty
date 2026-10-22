/* wp_room.h — the WatchParty room engine, shared by the Mac and iPhone apps.
 *
 * Owns the room: hosting and joining (with an invite code or by asking the
 * host), the sync protocol (load / play / pause / seek / ticks / clock offset),
 * chat and reactions, and the "ask to join" lobby. The app supplies a small
 * set of UI callbacks and drives it from its main thread. */
#ifndef WP_ROOM_H
#define WP_ROOM_H

#include <stddef.h>
#include <stdint.h>
#include "wp_util.h"

enum { WP_ROLE_NONE, WP_ROLE_HOST, WP_ROLE_CLIENT };

enum {                       /* guest-side "ask to join" progress */
    WP_ASK_NONE,
    WP_ASK_CONNECTING,       /* opening the host's lobby */
    WP_ASK_WAITING,          /* host is deciding; check code is known */
};

#define WP_NAME_MAX  32
#define WP_ROOM_MAX  48
#define WP_TEXT_MAX  1000

typedef struct { const char *emoji; const char *word; const char *image; } wp_reaction;
extern const wp_reaction WP_REACTIONS[];
extern const int WP_N_REACTIONS;

/* Read-only view of the room for the UI. */
typedef struct {
    int  role;
    int  connecting;                 /* client: before the welcome arrives */
    char my_name[WP_NAME_MAX + 1];
    char room[WP_ROOM_MAX + 1];
    char host_name[WP_NAME_MAX + 1];
    char code[WP_CODE_STRLEN];       /* host only */
    char addr[160];
    char peers_text[1200];           /* "Fred (host)\nBob\n…" */
    char video[WP_VIDEO_ID_LEN + 1];
    int  player_ready;
    int  pstate;                     /* YouTube player state of our player */
    int  fancy;                      /* UI setting: show cuss words as big words */
    int  ask;                        /* WP_ASK_* */
    char ask_room[64];
    char ask_check[5];
} wp_room_state;

typedef struct {
    void (*say)(const char *line);                 /* a chat-worthy line (UI may also show it as a subtitle) */
    void (*note)(const char *line);                /* a status line for the chat log only */
    void (*player)(const char *js);                /* run JavaScript in the player page */
    void (*video_loaded)(const char *video_id);    /* show what is playing (e.g. in the search field) */
    void (*changed)(void);                         /* room info / people / ask state changed */
    void (*sticker)(const char *text, const char *by, const char *image);   /* animated reaction */
    void (*beep)(void);
    void (*rooms_changed)(void);                   /* nearby rooms list changed */
    void (*knock)(int knock_id, const char *name, const char *check);      /* host: someone asks to join */
    void (*knock_gone)(int knock_id);              /* host: that request is no longer pending */
} wp_room_ui;

void           wp_room_init(const wp_room_ui *ui, const char *my_name);
wp_room_state *wp_room(void);
void           wp_room_shutdown(void);               /* app is quitting */
int            wp_room_watching(void);

/* hosting / joining */
int  wp_room_host(const char *name, const char *room_name);
int  wp_room_host_ex(const char *name, const char *room_name, uint16_t port, int advertise);
int  wp_room_join(const char *name, int nearby_index, const char *address, const char *code);
void wp_room_leave(const char *why);
void wp_room_invite_text(char *buf, size_t cap);

/* ask to join (no code needed) */
int  wp_room_ask(const char *name, int nearby_index);
int  wp_room_ask_addr(const char *name, const char *host, const char *lobby_port, const char *room_port);
void wp_room_ask_cancel(void);
void wp_room_answer(int knock_id, int allow);     /* host */
int  wp_room_pending_knocks(void);

/* nearby rooms (Bonjour), excluding the one we host */
int         wp_room_nearby_count(void);
const char *wp_room_nearby_name(int i);
int         wp_room_nearby_can_ask(int i);

/* watching together */
int  wp_room_load(const char *link_or_id);        /* 0 if it isn't a YouTube link */
void wp_room_chat(const char *text);
void wp_room_react(int index);
void wp_room_sync_now(void);
void wp_room_player_event(const char *json);      /* from the player page's message handler */

/* helpers the UIs share */
void        wp_sanitize_text(char *dst, size_t cap, const char *src, int allow_newlines);
void        wp_sanitize_name(char *dst, size_t cap, const char *src);
const char *wp_room_fancy(const char *text, char *buf, size_t cap);
void        wp_format_time(double secs, char *out, size_t cap);

#endif
