/* wp_app.c — the WatchParty Cocoa application, written in C.
 *
 * Cinema layout: a borderless, floating window that is nothing but the video.
 * Everything else lives in translucent overlays that fade away when the mouse
 * and keyboard go quiet:
 *
 *   ┌──────────────────────────────────────────────────────────────────┐
 *   │ ╭ (×) [ Search YouTube / link ...] [Load]  [Host][Rooms][Invite][Leave] ╮ │
 *   │ ╰ ● Hosting “Movie night” · 3 watching · code …  · 192.168.1.5:47321 ╯ │
 *   │ ╭ Bob wants to join ╮                            ╭──────────────╮ │
 *   │ │ check code 4821   │                            │ Fred, Bob    │ │
 *   │ ╰ [Deny]   [Allow]  ╯   WKWebView (YouTube)      │ chat, live   │ │
 *   │                                                   │ [ message  ] │ │
 *   │                 ╭ Bob: that part! ╮ (subtitle)    ╰──────────────╯ │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * The room itself (hosting, joining, asking to join, the sync protocol, chat)
 * is the shared engine in wp_room.c; this file is the Mac face of it. */
#include "objc_glue.h"
#include "wp_app.h"
#include "wp_json.h"
#include "wp_net.h"
#include "wp_room.h"
#include "wp_search.h"
#include "wp_fancy.h"
#include "wp_util.h"

#include <dispatch/dispatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern id   NSFullUserName(void);
extern void NSBeep(void);
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);

/* ------------------------------------------------------------ constants */

enum {                                 /* NSAutoresizingMaskOptions */
    MASK_MINX = 1, MASK_WIDTH = 2, MASK_MAXX = 4,
    MASK_MINY = 8, MASK_HEIGHT = 16, MASK_MAXY = 32
};

enum {                                 /* NSEventModifierFlags */
    MOD_SHIFT = 1UL << 17, MOD_CONTROL = 1UL << 18, MOD_OPTION = 1UL << 19, MOD_COMMAND = 1UL << 20
};

/* overlay geometry (points) */
#define PAD          14.0
#define TOP_H        76.0
#define CHAT_W       300.0
#define CHAT_BOTTOM  70.0            /* keeps YouTube's own control bar uncovered */
#define CHAT_TOP     96.0            /* keeps YouTube's top-corner buttons reachable */
#define BAR_MIN_W    560.0
#define BAR_MAX_W    900.0
#define CARD_W       290.0           /* "wants to join" / "room nearby" cards, left side */
#define MAX_CARDS    8
#define TOAST_MS     12000           /* a "room nearby" card goes away by itself */

#define MSG_BAR_H     44.0           /* bottom message box */
#define MSG_BAR_Y     62.0           /* sits just above YouTube's control bar */
#define MSG_BAR_MAX_W 560.0
#define MSG_LINGER_MS 2500           /* stays this long after sending, then fades */
#define SUB_DELAY_MS  600            /* subtitles follow the chat by this much */
#define CURSOR_HIDE_MS 4000          /* pointer disappears over the video after this much quiet */
#define SUB_MIN_MS    2500           /* a subtitle stays at least this long */
#define SUB_SHOW_MS   5000           /* ...and at most this long */
#define SUB_QUEUE     8

#define LOG_CAP       (96 * 1024)

/* ---------------------------------------------------------------- state */

enum { CARD_NONE, CARD_KNOCK, CARD_ASK, CARD_ROOM };

typedef struct {
    int  kind;                       /* CARD_* */
    int  key;                        /* knock id (CARD_KNOCK) */
    char room[64];                   /* room name (CARD_ROOM) */
    int  gen;
    id   view;
} card_t;

static struct {
    char    *log;
    size_t   log_len, log_cap;

    dispatch_source_t hud_timer;

    /* overlays */
    int      bar_visible, bar_suppressed, floating;
    int      msgbar_visible, msgbar_zone;
    uint64_t msgbar_activity;
    int      chat_mode;              /* CHAT_PANEL, CHAT_SUBTITLES or CHAT_OFF */
    uint64_t last_mouse_move;
    char     sub_queue[SUB_QUEUE][320];
    int      sub_n, sub_gen, sub_visible;
    uint64_t sub_shown_at;

    /* Cocoa objects */
    id ctl, window, web;
    id hud_top, close_btn, url_field, load_btn, status_label, chat_btn;
    id chat_panel, chat_header, chat_scroll, chat_view, chat_input;
    id msg_bar, msg_field;
    id sub_box, sub_label;
    id status_item, status_menu, float_item, chat_items[3];
    id font_items[12], color_items[12];
    id join_popup;                   /* alive only while the Join dialog is up */
    id nearby_menu, status_nearby_menu;
    int font_choice, color_choice, sticker_slot;
    id fancy_item;

    /* YouTube search results (shown under the bar) */
    id   results_panel, results_header, results_thumbs[WP_MAX_RESULTS];
    int  results_visible, results_gen, nresults;
    char last_query[256];
    wp_result results[WP_MAX_RESULTS];

    /* rooms on the network (shown under the bar, like results) */
    id   rooms_panel;
    int  rooms_visible;

    /* cards on the left: requests to join, our own request, rooms that appeared */
    card_t cards[MAX_CARDS];
    int    card_gen;
    char   seen_rooms[WP_MAX_ROOMS][64];
    int    n_seen;
} G;

enum { CHAT_PANEL, CHAT_SUBTITLES, CHAT_OFF };

/* text style for chat and subtitles (Chat ▸ Font / Colour) */
typedef struct { const char *label; const char *name; } font_opt;
static const font_opt FONTS[] = {
    { "System", "" }, { "System Rounded", "@rounded" }, { "System Serif", "@serif" }, { "Monospaced", "@mono" },
    { "Avenir Next", "AvenirNext-Medium" }, { "Futura", "Futura-Medium" }, { "Georgia", "Georgia" },
    { "Chalkboard", "ChalkboardSE-Regular" }, { "Marker Felt", "MarkerFelt-Wide" }, { "American Typewriter", "AmericanTypewriter" },
};
typedef struct { const char *label; double r, g, b; } color_opt;
static const color_opt COLORS[] = {
    { "White", 1, 1, 1 }, { "Subtitle Yellow", 1, 0.93, 0.25 }, { "Cyan", 0.45, 0.92, 1 }, { "Lime", 0.62, 1, 0.42 },
    { "Pink", 1, 0.58, 0.82 }, { "Orange", 1, 0.66, 0.3 }, { "Lavender", 0.8, 0.74, 1 }, { "Mint", 0.55, 1, 0.85 },
};
#define N_FONTS  ((int)(sizeof FONTS / sizeof FONTS[0]))
#define N_COLORS ((int)(sizeof COLORS / sizeof COLORS[0]))

/* animated reaction stickers live in the shared engine (wp_room.c) */
#define REACTIONS       WP_REACTIONS
#define N_REACTIONS     WP_N_REACTIONS
#define QUICK_REACTIONS 6

static void subtitle_push(const char *text);
static void layout_overlays(void);
static void results_hide(void);
static void rooms_hide(void);
static void rooms_show(void);
static void yt_search(const char *query);
static void show_sticker(const char *text, const char *by, const char *image);
static id sticker_image(const char *name, double size);
static id chat_font(double size, int bold);
static id chat_color(void);
static void populate_join_popup(id popup);
static void cards_layout(void);
static void ask_card_refresh(void);
static void attention_refresh(void);
static void bar_set(int on);
static const char *default_name(void);

static wp_room_state *RS(void) { return wp_room(); }

/* ------------------------------------------------------------ utilities */

static void js_raw(const char *script)
{
    if (!G.web) return;
    MSG(void, id, id)(G.web, S("evaluateJavaScript:completionHandler:"), nsstr(script), NULL);
}

static void js(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    js_raw(buf);
}

static int sticker_name_ok(const char *s)
{
    if (!s || !s[0] || strlen(s) > 24) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
    return 1;
}

static id white(double alpha)
{
    return MSG(id, double, double)(C("NSColor"), S("colorWithWhite:alpha:"), 1.0, alpha);
}

static id sysfont(double size, int bold)
{
    return bold ? MSG(id, double)(C("NSFont"), S("boldSystemFontOfSize:"), size)
                : MSG(id, double)(C("NSFont"), S("systemFontOfSize:"), size);
}

/* Append a line to the chat panel (no subtitle). */
static void log_line(const char *text)
{
    size_t need = strlen(text) + 2;
    if (!G.log) { G.log_cap = 8192; G.log = (char *)malloc(G.log_cap); G.log[0] = 0; G.log_len = 0; }
    if (G.log_len + need >= LOG_CAP) {              /* drop the oldest third */
        size_t cut = G.log_len / 3;
        while (cut < G.log_len && G.log[cut] != '\n') cut++;
        if (cut < G.log_len) cut++;
        memmove(G.log, G.log + cut, G.log_len - cut + 1);
        G.log_len -= cut;
    }
    if (G.log_len + need >= G.log_cap) {
        while (G.log_len + need >= G.log_cap) G.log_cap *= 2;
        G.log = (char *)realloc(G.log, G.log_cap);
    }
    G.log_len += (size_t)snprintf(G.log + G.log_len, G.log_cap - G.log_len, "%s\n", text);

    if (G.chat_view) {
        MSG(void, id)(G.chat_view, S("setString:"), nsstr(G.log));
        MSG(void, id)(G.chat_view, S("setTextColor:"), chat_color());
        MSG(void, id)(G.chat_view, S("setFont:"), chat_font(13, 0));
        unsigned long len = MSG(unsigned long)(MSG(id)(G.chat_view, S("string")), S("length"));
        WPRange r = { len, 0 };
        MSG(void, WPRange)(G.chat_view, S("scrollRangeToVisible:"), r);
    }
}

static void logf_line(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line(buf);
}

/* Chat line; shown as a subtitle when the chat column is turned off. */
static void say(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line(buf);
    if (G.chat_mode == CHAT_SUBTITLES) {
        char *later = strdup(buf);                      /* subtitles trail the chat slightly */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)SUB_DELAY_MS * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (G.chat_mode == CHAT_SUBTITLES) subtitle_push(later);
            free(later);
        });
    }
}

static void update_info(void)
{
    const wp_room_state *r = RS();
    char status[512], header[1300], title[160];
    int n = wp_room_watching();
    int nearby = wp_room_nearby_count();
    if (r->role == WP_ROLE_HOST) {
        int asking = wp_room_pending_knocks();
        snprintf(status, sizeof status, "●  Hosting “%s” · %d watching%s%s · code %s · %s   (⌘I copies the invite)",
                 r->room, n, asking ? " · " : "", asking ? (asking == 1 ? "1 asking to join" : "people asking to join") : "",
                 r->code, r->addr[0] ? r->addr : "opening…");
        snprintf(title, sizeof title, "%s — hosting “%s”", WP_APP_NAME, r->room);
    } else if (r->role == WP_ROLE_CLIENT) {
        if (r->connecting) snprintf(status, sizeof status, "◌  Connecting to %s…", r->addr);
        else snprintf(status, sizeof status, "●  In “%s” · hosted by %s · %d watching · %s", r->room, r->host_name, n, r->addr);
        snprintf(title, sizeof title, "%s — “%s”", WP_APP_NAME, r->room);
    } else if (r->ask == WP_ASK_WAITING) {
        snprintf(status, sizeof status, "◌  Waiting for “%s” to let you in · your check code is %s", r->ask_room, r->ask_check);
        snprintf(title, sizeof title, "%s", WP_APP_NAME);
    } else if (r->ask == WP_ASK_CONNECTING) {
        snprintf(status, sizeof status, "◌  Asking “%s” to let you in…", r->ask_room);
        snprintf(title, sizeof title, "%s", WP_APP_NAME);
    } else {
        if (nearby == 1) snprintf(status, sizeof status, "○  “%s” is on your Wi-Fi — Rooms (⌘J) to ask to join. Or Host (⌘N), or search YouTube above.", wp_room_nearby_name(0));
        else if (nearby > 1) snprintf(status, sizeof status, "○  %d rooms on your Wi-Fi — Rooms (⌘J) to ask to join one. Or Host (⌘N), or search YouTube above.", nearby);
        else snprintf(status, sizeof status, "○  Not in a room — Host (⌘N) or Rooms (⌘J). Search YouTube or paste a link above. Esc hides the controls.");
        snprintf(title, sizeof title, "%s", WP_APP_NAME);
    }
    /* chat header: who is here, on one line */
    if (r->role != WP_ROLE_NONE && r->peers_text[0]) {
        size_t o = 0;
        for (const char *p = r->peers_text; *p && o + 3 < sizeof header; p++) {
            if (*p == '\n') { header[o++] = ','; header[o++] = ' '; }
            else header[o++] = *p;
        }
        header[o] = 0;
    } else wp_strlcpy(header, "Chat", sizeof header);

    if (G.status_label) MSG(void, id)(G.status_label, S("setStringValue:"), nsstr(status));
    if (G.chat_header) MSG(void, id)(G.chat_header, S("setStringValue:"), nsstr(header));
    if (G.window) MSG(void, id)(G.window, S("setTitle:"), nsstr(title));
    static const char *modes[] = { "Chat: on", "Chat: subtitles", "Chat: off" };
    if (G.chat_btn) MSG(void, id)(G.chat_btn, S("setTitle:"), nsstr(modes[G.chat_mode]));
    for (int i = 0; i < 3; i++) if (G.chat_items[i]) MSG(void, long)(G.chat_items[i], S("setState:"), G.chat_mode == i ? 1 : 0);
}

static void copy_invite(void)
{
    if (RS()->role != WP_ROLE_HOST) { NSBeep(); say("Host a room first — then Copy Invite gives you the code to share."); return; }
    char text[900];
    wp_room_invite_text(text, sizeof text);
    id pb = MSG(id)(C("NSPasteboard"), S("generalPasteboard"));
    MSG(long)(pb, S("clearContents"));
    MSG(BOOL, id, id)(pb, S("setString:forType:"), nsstr(text), nsstr("public.utf8-plain-text"));
    say("Invite copied to the clipboard.");
}

/* ------------------------------------------------- room engine → Mac UI */

static void ui_say(const char *line) { say("%s", line); }
static void ui_note(const char *line) { log_line(line); }
static void ui_player(const char *script) { js_raw(script); }

static void ui_video_loaded(const char *vid)
{
    if (!G.url_field) return;
    char url[80];
    snprintf(url, sizeof url, "https://www.youtube.com/watch?v=%s", vid);
    MSG(void, id)(G.url_field, S("setStringValue:"), nsstr(url));
}

static void ui_changed(void)
{
    update_info();
    ask_card_refresh();
    attention_refresh();
    if (G.rooms_visible) rooms_show();
}

static void ui_sticker(const char *text, const char *by, const char *image)
{
    int mine = by && strcmp(by, RS()->my_name) == 0;
    if (mine || G.chat_mode != CHAT_OFF) show_sticker(text, by, image);
}

static void ui_beep(void) { NSBeep(); }

/* --------------------------------------------------------- UI helpers */

static id make_label(const char *text, CGRect r, double size, int bold)
{
    id lbl = MSG(id, id)(C("NSTextField"), S("labelWithString:"), nsstr(text));
    MSG(void, CGRect)(lbl, S("setFrame:"), r);
    MSG(void, id)(lbl, S("setFont:"), sysfont(size, bold));
    return lbl;
}

static id make_field(CGRect r, const char *placeholder, const char *value)
{
    id f = MSG(id, CGRect)(MSG(id)(C("NSTextField"), S("alloc")), S("initWithFrame:"), r);
    MSG(void, id)(f, S("setPlaceholderString:"), nsstr(placeholder));
    MSG(void, BOOL)(f, S("setBezeled:"), YES);
    MSG(void, unsigned long)(f, S("setBezelStyle:"), 1);          /* NSTextFieldRoundedBezel */
    MSG(void, BOOL)(f, S("setEditable:"), YES);
    MSG(void, BOOL)(f, S("setSelectable:"), YES);
    MSG(void, id)(f, S("setFont:"), sysfont(13, 0));
    if (value) MSG(void, id)(f, S("setStringValue:"), nsstr(value));
    return f;
}

static id make_button(const char *title, const char *sel, CGRect r, unsigned long mask)
{
    id b = MSG(id, id, id, SEL)(C("NSButton"), S("buttonWithTitle:target:action:"), nsstr(title), G.ctl, S(sel));
    MSG(void, CGRect)(b, S("setFrame:"), r);
    MSG(void, unsigned long)(b, S("setControlSize:"), 1);         /* NSControlSizeSmall */
    MSG(void, id)(b, S("setFont:"), sysfont(12, 0));
    MSG(void, unsigned long)(b, S("setAutoresizingMask:"), mask);
    return b;
}

static void set_layer_color(id layer, const char *sel, double r, double g, double b, double a)
{
    CGColorRef c = CGColorCreateGenericRGB(r, g, b, a);
    MSG(void, void *)(layer, S(sel), c);
    CGColorRelease(c);
}

/* A translucent rounded panel (no border); dragging its background moves the window. */
static id make_panel(CGRect r, unsigned long mask)
{
    id v = MSG(id, CGRect)(MSG(id)((id)objc_getClass("WPHUDView"), S("alloc")), S("initWithFrame:"), r);
    MSG(void, BOOL)(v, S("setWantsLayer:"), YES);
    id layer = MSG(id)(v, S("layer"));
    MSG(void, double)(layer, S("setCornerRadius:"), 14.0);
    MSG(void, BOOL)(layer, S("setMasksToBounds:"), YES);
    set_layer_color(layer, "setBackgroundColor:", 0.07, 0.07, 0.09, 0.70);
    MSG(void, unsigned long)(v, S("setAutoresizingMask:"), mask);
    MSG(void, id)(v, S("setAppearance:"), MSG(id, id)(C("NSAppearance"), S("appearanceNamed:"), nsstr("NSAppearanceNameDarkAqua")));
    return v;
}

static int debug_on(void) { static int v = -1; if (v < 0) v = getenv("WP_DEBUG") != NULL; return v; }
#define DBG(...) do { if (debug_on()) { fprintf(stderr, "[wp] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

/* Animate a view's alpha; hide it completely once fully transparent so an
 * invisible overlay never swallows clicks meant for the video. */
static void fade_view(id view, double alpha, double duration)
{
    if (!view) return;
    if (alpha > 0) MSG(void, BOOL)(view, S("setHidden:"), NO);
    void (^changes)(id) = ^(id ctx) {
        MSG(void, double)(ctx, S("setDuration:"), duration);
        MSG(void, double)(MSG(id)(view, S("animator")), S("setAlphaValue:"), alpha);
    };
    retain(view);                                      /* plain C blocks don't retain: keep it alive */
    void (^done)(void) = ^{
        if (MSG(double)(view, S("alphaValue")) <= 0.001) MSG(void, BOOL)(view, S("setHidden:"), YES);
        release(view);
    };
    MSG(void, void *, void *)(C("NSAnimationContext"), S("runAnimationGroup:completionHandler:"), (void *)changes, (void *)done);
}

static id add_menu(id bar, const char *title)
{
    id item = MSG(id, id, SEL, id)(MSG(id)(C("NSMenuItem"), S("alloc")),
                                   S("initWithTitle:action:keyEquivalent:"), nsstr(title), (SEL)NULL, nsstr(""));
    id menu = MSG(id, id)(MSG(id)(C("NSMenu"), S("alloc")), S("initWithTitle:"), nsstr(title));
    MSG(void, id)(item, S("setSubmenu:"), menu);
    MSG(void, id)(bar, S("addItem:"), item);
    release(item);
    return menu;                                                   /* retained by its item */
}

static id add_item(id menu, const char *title, const char *sel, const char *key, unsigned long mods, int target_ctl)
{
    id item = MSG(id, id, SEL, id)(MSG(id)(C("NSMenuItem"), S("alloc")),
                                   S("initWithTitle:action:keyEquivalent:"), nsstr(title), S(sel), nsstr(key ? key : ""));
    if (mods) MSG(void, unsigned long)(item, S("setKeyEquivalentModifierMask:"), mods);
    if (target_ctl) MSG(void, id)(item, S("setTarget:"), G.ctl);
    MSG(void, id)(menu, S("addItem:"), item);
    release(item);
    return item;
}

static void add_separator(id menu)
{
    MSG(void, id)(menu, S("addItem:"), MSG(id)(C("NSMenuItem"), S("separatorItem")));
}

static void show_window(void)
{
    if (!G.window) return;
    MSG(void, id)(G.window, S("makeKeyAndOrderFront:"), NULL);
    MSG(void, BOOL)(MSG(id)(C("NSApplication"), S("sharedApplication")), S("activateIgnoringOtherApps:"), YES);
}

static const char *default_name(void)
{
    static char name[WP_NAME_MAX + 1];
    id defaults = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    id saved = MSG(id, id)(defaults, S("stringForKey:"), nsstr("displayName"));
    const char *s = saved ? cstr(saved) : "";
    if (!s[0]) s = cstr(NSFullUserName());
    wp_sanitize_name(name, sizeof name, s);
    return name;
}

static void remember_name(const char *name)
{
    id defaults = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    MSG(void, id, id)(defaults, S("setObject:forKey:"), nsstr(name), nsstr("displayName"));
}

/* ---------------------------------------------------- overlays */

static CGRect content_bounds(void)
{
    id content = MSG(id)(G.window, S("contentView"));
    return MSG_STRET(CGRect)(content, S("bounds"));
}

static CGRect bar_frame(void)
{
    CGRect b = content_bounds();
    double w = b.size.width * 0.64;
    if (w < BAR_MIN_W) w = BAR_MIN_W;
    if (w > BAR_MAX_W) w = BAR_MAX_W;
    if (w > b.size.width - 2 * PAD) w = b.size.width - 2 * PAD;
    return rect((b.size.width - w) / 2, b.size.height - PAD - TOP_H, w, TOP_H);
}

/* The bar shows while the pointer is around it: its own rect, up to the top edge. */
static int in_bar_zone(CGPoint p)
{
    CGRect f = bar_frame();
    CGRect b = content_bounds();
    double bottom = f.origin.y - 24;
    if (G.results_visible && G.results_panel) {
        CGRect r = MSG_STRET(CGRect)(G.results_panel, S("frame"));
        bottom = r.origin.y - 24;
    }
    if (G.rooms_visible && G.rooms_panel) {
        CGRect r = MSG_STRET(CGRect)(G.rooms_panel, S("frame"));
        bottom = r.origin.y - 24;
    }
    return p.x >= f.origin.x - 24 && p.x <= f.origin.x + f.size.width + 24 &&
           p.y >= bottom && p.y <= b.size.height;
}

static int url_field_focused(void)
{
    id fr = MSG(id)(G.window, S("firstResponder"));
    return fr && MSG(BOOL, id)(fr, S("isKindOfClass:"), C("NSTextView")) && MSG(id)(fr, S("delegate")) == G.url_field;
}

static void bar_set(int on)
{
    if (G.bar_visible == on) return;
    DBG("bar %s", on ? "show" : "hide");
    G.bar_visible = on;
    fade_view(G.hud_top, on ? 1.0 : 0.0, on ? 0.15 : 0.35);
    if (!on && url_field_focused()) MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.web);
    if (!on) { results_hide(); rooms_hide(); }
    layout_overlays();
}

static void bar_update(int pointer_in_zone)
{
    if (pointer_in_zone && !G.bar_suppressed) bar_set(1);
    else if (!pointer_in_zone) { G.bar_suppressed = 0; if (!url_field_focused()) bar_set(0); }
}

/* ---- bottom message box: for chatting while the chat column is hidden ---- */

static CGRect msgbar_frame(void)
{
    CGRect b = content_bounds();
    double w = b.size.width * 0.5;
    if (w > MSG_BAR_MAX_W) w = MSG_BAR_MAX_W;
    if (w < 300) w = b.size.width - 2 * PAD < 300 ? b.size.width - 2 * PAD : 300;
    return rect((b.size.width - w) / 2, MSG_BAR_Y, w, MSG_BAR_H);
}

/* near the box, but not down on YouTube's own control bar */
static int in_msgbar_zone(CGPoint p)
{
    CGRect f = msgbar_frame();
    return p.x >= f.origin.x - 40 && p.x <= f.origin.x + f.size.width + 40 &&
           p.y >= f.origin.y - 6 && p.y <= f.origin.y + f.size.height + 40;
}

static int msg_field_focused(void)
{
    id fr = MSG(id)(G.window, S("firstResponder"));
    return fr && MSG(BOOL, id)(fr, S("isKindOfClass:"), C("NSTextView")) && MSG(id)(fr, S("delegate")) == G.msg_field;
}

static int msg_draft(void) { return G.msg_field && cstr(MSG(id)(G.msg_field, S("stringValue")))[0] != 0; }

static void msgbar_set(int on)
{
    if (!G.msg_bar || G.msgbar_visible == on) return;
    G.msgbar_visible = on;
    fade_view(G.msg_bar, on ? 1.0 : 0.0, on ? 0.15 : 0.4);
    if (!on && msg_field_focused()) MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.web);
}

static void msgbar_open(void)                          /* Return, or the pointer arriving */
{
    if (G.chat_mode == CHAT_PANEL) return;            /* the column has its own box */
    G.msgbar_activity = wp_now_ms();
    msgbar_set(1);
    MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.msg_field);
}

static void msgbar_update(int pointer_in_zone)
{
    G.msgbar_zone = pointer_in_zone;
    if (pointer_in_zone && G.chat_mode != CHAT_PANEL) { G.msgbar_activity = wp_now_ms(); msgbar_set(1); }
}

/* called from the HUD timer: fade once the pointer has gone and nothing is being typed */
static void msgbar_tick(void)
{
    if (!G.msgbar_visible) return;
    if (G.chat_mode == CHAT_PANEL) { msgbar_set(0); return; }
    if (G.msgbar_zone || msg_draft()) return;
    if (wp_now_ms() - G.msgbar_activity > MSG_LINGER_MS) msgbar_set(0);
}

/* ---- subtitles: one message at a time, top centre ---- */

static void subtitle_layout(void)
{
    if (!G.sub_box) return;
    CGRect b = content_bounds();
    double max_w = b.size.width * 0.72 - 32;
    if (max_w < 120) max_w = 120;
    CGSize fit = MSG(CGSize, CGSize)(G.sub_label, S("sizeThatFits:"), (CGSize){ max_w, 10000 });
    double bw = fit.width + 32, bh = fit.height + 18;
    double y = b.size.height - PAD - bh - (G.bar_visible ? TOP_H + 8 : 0);
    MSG(void, CGRect)(G.sub_box, S("setFrame:"), rect((b.size.width - bw) / 2, y, bw, bh));
    MSG(void, CGRect)(G.sub_label, S("setFrame:"), rect(16, 9, fit.width, fit.height));
}

static void subtitle_expire(int gen);

static void subtitle_display(const char *text)
{
    MSG(void, id)(G.sub_label, S("setStringValue:"), nsstr(text));
    subtitle_layout();
    G.sub_visible = 1;
    G.sub_shown_at = wp_now_ms();
    int gen = ++G.sub_gen;
    fade_view(G.sub_box, 1.0, 0.15);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)SUB_MIN_MS * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ subtitle_expire(gen); });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)SUB_SHOW_MS * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ subtitle_expire(gen); });
}

static void subtitle_hide(void)
{
    G.sub_visible = 0;
    G.sub_n = 0;
    G.sub_gen++;
    fade_view(G.sub_box, 0.0, 0.4);
}

static void subtitle_expire(int gen)
{
    if (gen != G.sub_gen) return;                       /* superseded */
    if (G.sub_n > 0) {                                  /* next one is waiting */
        char next[320];
        wp_strlcpy(next, G.sub_queue[0], sizeof next);
        memmove(G.sub_queue[0], G.sub_queue[1], sizeof G.sub_queue[0] * (SUB_QUEUE - 1));
        G.sub_n--;
        subtitle_display(next);
        return;
    }
    if (wp_now_ms() - G.sub_shown_at >= SUB_SHOW_MS - 50) {
        G.sub_visible = 0;
        fade_view(G.sub_box, 0.0, 0.5);
    }
}

static void subtitle_push(const char *text)
{
    if (!G.sub_box || !text || !text[0]) return;
    if (!G.sub_visible || (G.sub_n == 0 && wp_now_ms() - G.sub_shown_at >= SUB_MIN_MS)) {
        subtitle_display(text);
        return;
    }
    if (G.sub_n == SUB_QUEUE) {                         /* drop the oldest waiting line */
        memmove(G.sub_queue[0], G.sub_queue[1], sizeof G.sub_queue[0] * (SUB_QUEUE - 1));
        G.sub_n--;
    }
    wp_strlcpy(G.sub_queue[G.sub_n++], text, sizeof G.sub_queue[0]);
}

/* ---- chat display mode ---- */

static void set_chat_mode(int mode)
{
    if (mode < CHAT_PANEL || mode > CHAT_OFF) mode = CHAT_PANEL;
    G.chat_mode = mode;
    fade_view(G.chat_panel, mode == CHAT_PANEL ? 1.0 : 0.0, 0.25);
    if (mode != CHAT_SUBTITLES) subtitle_hide();
    if (mode == CHAT_PANEL) msgbar_set(0);
    if (mode != CHAT_PANEL) {
        id fr = MSG(id)(G.window, S("firstResponder"));
        if (fr && MSG(BOOL, id)(fr, S("isKindOfClass:"), C("NSTextView")) && MSG(id)(fr, S("delegate")) == G.chat_input)
            MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.web);
    }
    id defaults = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    MSG(void, long, id)(defaults, S("setInteger:forKey:"), (long)mode, nsstr("chatMode"));
    update_info();
}

static void layout_overlays(void)
{
    if (!G.hud_top) return;
    MSG(void, CGRect)(G.hud_top, S("setFrame:"), bar_frame());
    if (G.msg_bar) MSG(void, CGRect)(G.msg_bar, S("setFrame:"), msgbar_frame());
    if (G.results_panel) {
        CGRect bf = bar_frame();
        CGRect rf = MSG_STRET(CGRect)(G.results_panel, S("frame"));
        MSG(void, CGRect)(G.results_panel, S("setFrame:"), rect(bf.origin.x, bf.origin.y - 8 - rf.size.height, bf.size.width, rf.size.height));
    }
    if (G.rooms_panel) {
        CGRect bf = bar_frame();
        CGRect rf = MSG_STRET(CGRect)(G.rooms_panel, S("frame"));
        MSG(void, CGRect)(G.rooms_panel, S("setFrame:"), rect(bf.origin.x, bf.origin.y - 8 - rf.size.height, bf.size.width, rf.size.height));
    }
    subtitle_layout();
    cards_layout();
}

static void on_hud_timer(void)
{
    void *pool = objc_autoreleasePoolPush();
    uint64_t now = wp_now_ms();
    msgbar_tick();
    if (!G.bar_visible && now - G.last_mouse_move > CURSOR_HIDE_MS && MSG(BOOL)(G.window, S("isKeyWindow"))) {
        id fr = MSG(id)(G.window, S("firstResponder"));
        int typing = fr && MSG(BOOL, id)(fr, S("isKindOfClass:"), C("NSTextView"));
        if (!typing) MSG(void, BOOL)(C("NSCursor"), S("setHiddenUntilMouseMoves:"), YES);
    }
    objc_autoreleasePoolPop(pool);
}

/* ---------------------------------------------- text style & stickers */

static id chat_font(double size, int bold)
{
    const char *name = FONTS[G.font_choice].name;
    id base = sysfont(size, bold);
    if (!name[0]) return base;
    if (strcmp(name, "@mono") == 0) {
        id f = MSG(id, double, double)(C("NSFont"), S("monospacedSystemFontOfSize:weight:"), size, bold ? 0.4 : 0.0);
        return f ? f : base;
    }
    if (name[0] == '@') {                                /* system design variants (macOS 11+) */
        id desc = MSG(id)(base, S("fontDescriptor"));
        if (!responds(desc, "fontDescriptorWithDesign:")) return base;
        id d2 = MSG(id, id)(desc, S("fontDescriptorWithDesign:"), nsstr(name + 1));
        id f = d2 ? MSG(id, id, double)(C("NSFont"), S("fontWithDescriptor:size:"), d2, size) : NULL;
        return f ? f : base;
    }
    id f = MSG(id, id, double)(C("NSFont"), S("fontWithName:size:"), nsstr(name), size);
    if (!f) return base;
    if (bold) {
        id fm = MSG(id)(C("NSFontManager"), S("sharedFontManager"));
        id fb = MSG(id, id, unsigned long)(fm, S("convertFont:toHaveTrait:"), f, 2 /* NSBoldFontMask */);
        if (fb) f = fb;
    }
    return f;
}

static id chat_color(void)
{
    const color_opt *c = &COLORS[G.color_choice];
    return MSG(id, double, double, double, double)(C("NSColor"), S("colorWithSRGBRed:green:blue:alpha:"), c->r, c->g, c->b, 1.0);
}

static void apply_text_style(void)
{
    if (G.chat_view) {
        MSG(void, id)(G.chat_view, S("setTextColor:"), chat_color());
        MSG(void, id)(G.chat_view, S("setFont:"), chat_font(13, 0));
    }
    if (G.sub_label) {
        MSG(void, id)(G.sub_label, S("setFont:"), chat_font(17, 1));
        MSG(void, id)(G.sub_label, S("setTextColor:"), chat_color());
    }
    for (int i = 0; i < N_FONTS; i++) if (G.font_items[i]) MSG(void, long)(G.font_items[i], S("setState:"), G.font_choice == i ? 1 : 0);
    for (int i = 0; i < N_COLORS; i++) if (G.color_items[i]) MSG(void, long)(G.color_items[i], S("setState:"), G.color_choice == i ? 1 : 0);
    id defaults = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    MSG(void, long, id)(defaults, S("setInteger:forKey:"), (long)G.font_choice, nsstr("chatFont"));
    MSG(void, long, id)(defaults, S("setInteger:forKey:"), (long)G.color_choice, nsstr("chatColor"));
}

static void animate(id view, double duration, CGRect frame, double alpha, void (^done)(void))
{
    void (^changes)(id) = ^(id ctx) {
        MSG(void, double)(ctx, S("setDuration:"), duration);
        id a = MSG(id)(view, S("animator"));
        MSG(void, CGRect)(a, S("setFrame:"), frame);
        MSG(void, double)(a, S("setAlphaValue:"), alpha);
    };
    MSG(void, void *, void *)(C("NSAnimationContext"), S("runAnimationGroup:completionHandler:"), (void *)changes, (void *)done);
}

/* A copy of a bundled sticker image (Resources/<name>.png, @2x aware) at a given point size. */
static id sticker_image(const char *name, double size)
{
    if (!sticker_name_ok(name)) return NULL;
    id bundle = MSG(id)(C("NSBundle"), S("mainBundle"));
    id img = MSG(id, id)(bundle, S("imageForResource:"), nsstr(name));
    if (!img) return NULL;
    id copy = MSG(id)(img, S("copy"));                      /* each use gets its own size */
    MSG(void, CGSize)(copy, S("setSize:"), (CGSize){ size, size });
    return copy;
}

/* A reaction pops up over the video, bounces, floats away. */
static void show_sticker(const char *text, const char *by, const char *image)
{
    if (!G.window) return;
    id content = MSG(id)(G.window, S("contentView"));
    CGRect b = content_bounds();
    id pic = image ? sticker_image(image, 60) : NULL;
    const double pic_w = pic ? 68 : 0;                        /* image plus a gap */

    id box = MSG(id, CGRect)(MSG(id)((id)objc_getClass("WPPassView"), S("alloc")), S("initWithFrame:"), rect(0, 0, 10, 10));
    MSG(void, BOOL)(box, S("setWantsLayer:"), YES);
    id layer = MSG(id)(box, S("layer"));
    MSG(void, double)(layer, S("setCornerRadius:"), 16.0);
    MSG(void, BOOL)(layer, S("setMasksToBounds:"), YES);
    set_layer_color(layer, "setBackgroundColor:", 0, 0, 0, 0.55);

    id big = MSG(id, id)(C("NSTextField"), S("labelWithString:"), nsstr(text));
    MSG(void, id)(big, S("setFont:"), chat_font(26, 1));
    MSG(void, id)(big, S("setTextColor:"), chat_color());
    CGSize bs = MSG(CGSize, CGSize)(big, S("sizeThatFits:"), (CGSize){ 600, 100 });
    id who = MSG(id, id)(C("NSTextField"), S("labelWithString:"), nsstr(by ? by : ""));
    MSG(void, id)(who, S("setFont:"), sysfont(11, 0));
    MSG(void, id)(who, S("setTextColor:"), white(0.75));
    CGSize ws = MSG(CGSize, CGSize)(who, S("sizeThatFits:"), (CGSize){ 600, 40 });
    double row_w = bs.width + pic_w;
    double w = (row_w > ws.width ? row_w : ws.width) + 36;
    double row_h = (pic && bs.height < 60) ? 60 : bs.height;
    double h = row_h + ws.height + 22;
    double x0 = (w - row_w) / 2;
    if (pic) {
        id iv = MSG(id, CGRect)(MSG(id)(C("NSImageView"), S("alloc")), S("initWithFrame:"), rect(x0, ws.height + 12, 60, 60));
        MSG(void, id)(iv, S("setImage:"), pic);
        MSG(void, unsigned long)(iv, S("setImageScaling:"), 3);   /* proportionally up or down */
        MSG(void, id)(box, S("addSubview:"), iv);
        release(iv);
        release(pic);
    }
    MSG(void, CGRect)(big, S("setFrame:"), rect(x0 + pic_w, ws.height + 12 + (row_h - bs.height) / 2, bs.width, bs.height));
    MSG(void, CGRect)(who, S("setFrame:"), rect((w - ws.width) / 2, 8, ws.width, ws.height));
    MSG(void, id)(box, S("addSubview:"), big);
    MSG(void, id)(box, S("addSubview:"), who);

    /* five launch slots across the middle of the picture so bursts don't pile up */
    int slot = G.sticker_slot++ % 5;
    double cx = b.size.width * (0.30 + 0.10 * slot);
    double y0 = b.size.height * 0.22;
    CGRect start = rect(cx - w / 2, y0 - 14, w, h);
    MSG(void, CGRect)(box, S("setFrame:"), start);
    MSG(void, double)(box, S("setAlphaValue:"), 0.0);
    MSG(void, id)(content, S("addSubview:"), box);

    CGRect up = rect(cx - w / 2, y0 + 16, w, h);
    CGRect settle = rect(cx - w / 2, y0 + 8, w, h);
    CGRect away = rect(cx - w / 2, y0 + 90, w, h);
    animate(box, 0.20, up, 1.0, ^{
        animate(box, 0.12, settle, 1.0, ^{
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                animate(box, 0.75, away, 0.0, ^{
                    MSG(void)(box, S("removeFromSuperview"));
                    release(box);
                });
            });
        });
    });
}


/* ------------------------------------------------------ YouTube search */

static int looks_like_link(const char *text)
{
    if (wp_is_video_id(text)) return 1;
    return strstr(text, "youtube.com/") || strstr(text, "youtu.be/") || strstr(text, "://") || strncmp(text, "www.", 4) == 0;
}

static void results_hide(void)
{
    if (!G.results_visible) return;
    G.results_visible = 0;
    G.results_gen++;
    fade_view(G.results_panel, 0.0, 0.2);
}

static void on_thumbnail(int row, int gen, const void *bytes, size_t len)
{
    if (!bytes || gen != G.results_gen || row < 0 || row >= WP_MAX_RESULTS || !G.results_thumbs[row]) return;
    id data = MSG(id, const void *, unsigned long)(C("NSData"), S("dataWithBytes:length:"), bytes, (unsigned long)len);
    id img = MSG(id, id)(MSG(id)(C("NSImage"), S("alloc")), S("initWithData:"), data);
    if (img) { MSG(void, id)(G.results_thumbs[row], S("setImage:"), img); release(img); }
}

/* Rebuild the results panel under the bar. */
static void results_show(const char *header_text)
{
    id content = MSG(id)(G.window, S("contentView"));
    rooms_hide();
    if (G.results_panel) { MSG(void)(G.results_panel, S("removeFromSuperview")); release(G.results_panel); G.results_panel = NULL; }
    memset(G.results_thumbs, 0, sizeof G.results_thumbs);

    CGRect bf = bar_frame();
    const double row_h = 62, head_h = 30;
    int rows = G.nresults;
    while (rows > 0 && head_h + 16 + rows * row_h > bf.origin.y - 40) rows--;    /* fit the window */
    double ph = head_h + 8 + rows * row_h + 8;
    G.results_panel = make_panel(rect(bf.origin.x, bf.origin.y - 8 - ph, bf.size.width, ph), 0);
    id panel = G.results_panel;

    G.results_header = make_label(header_text, rect(14, ph - head_h + 6, bf.size.width - 28, 18), 12, 1);
    MSG(void, id)(G.results_header, S("setTextColor:"), white(0.85));
    MSG(void, unsigned long)(G.results_header, S("setLineBreakMode:"), 4);
    MSG(void, id)(panel, S("addSubview:"), G.results_header);

    for (int i = 0; i < rows; i++) {
        double top = ph - head_h - 8 - i * row_h;              /* top edge of this row */
        id thumb = MSG(id, CGRect)(MSG(id)(C("NSImageView"), S("alloc")), S("initWithFrame:"), rect(12, top - row_h + 6, 90, 50));
        MSG(void, unsigned long)(thumb, S("setImageScaling:"), 3);
        MSG(void, BOOL)(thumb, S("setWantsLayer:"), YES);
        MSG(void, double)(MSG(id)(thumb, S("layer")), S("setCornerRadius:"), 6.0);
        MSG(void, BOOL)(MSG(id)(thumb, S("layer")), S("setMasksToBounds:"), YES);
        set_layer_color(MSG(id)(thumb, S("layer")), "setBackgroundColor:", 1, 1, 1, 0.08);
        MSG(void, id)(panel, S("addSubview:"), thumb);
        G.results_thumbs[i] = thumb;
        release(thumb);

        id title = make_label(G.results[i].title, rect(112, top - 26, bf.size.width - 124, 18), 13, 1);
        MSG(void, id)(title, S("setTextColor:"), white(0.95));
        MSG(void, unsigned long)(title, S("setLineBreakMode:"), 4);
        MSG(void, id)(panel, S("addSubview:"), title);
        id meta = make_label(G.results[i].meta, rect(112, top - 46, bf.size.width - 124, 16), 11, 0);
        MSG(void, id)(meta, S("setTextColor:"), white(0.65));
        MSG(void, unsigned long)(meta, S("setLineBreakMode:"), 4);
        MSG(void, id)(panel, S("addSubview:"), meta);

        /* an invisible button over the whole row takes the click */
        id hit = MSG(id, id, id, SEL)(C("NSButton"), S("buttonWithTitle:target:action:"), nsstr(""), G.ctl, S("pickResult:"));
        MSG(void, CGRect)(hit, S("setFrame:"), rect(4, top - row_h + 1, bf.size.width - 8, row_h - 2));
        MSG(void, BOOL)(hit, S("setBordered:"), NO);
        MSG(void, BOOL)(hit, S("setTransparent:"), YES);
        MSG(void, long)(hit, S("setTag:"), (long)i);
        MSG(void, id)(hit, S("setToolTip:"), nsstr("Play this for everyone"));
        MSG(void, id)(panel, S("addSubview:"), hit);

        char url[128];
        snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", G.results[i].id);
        wp_fetch(url, i, G.results_gen, on_thumbnail);
    }
    MSG(void, double)(panel, S("setAlphaValue:"), 0.0);
    MSG(void, id)(content, S("addSubview:"), panel);
    G.results_visible = 1;
    fade_view(panel, 1.0, 0.2);
    bar_set(1);
}

static void on_search_results(int gen, const wp_result *r, int n, int ok)
{
    if (gen != G.results_gen) return;                    /* a newer search, or the panel closed */
    G.nresults = n;
    if (n > 0) memcpy(G.results, r, (size_t)n * sizeof *r);
    char head[300];
    if (!ok) snprintf(head, sizeof head, "Search failed — check the connection, or paste a link");
    else if (n == 0) snprintf(head, sizeof head, "No results for “%s” — try other words, or paste a link", G.last_query);
    else snprintf(head, sizeof head, "YouTube results for “%s” — click one to play it for everyone (Return plays the first)", G.last_query);
    results_show(head);
}

static void yt_search(const char *query)
{
    char q[256];
    wp_strlcpy(q, query, sizeof q);
    wp_trim(q);
    if (!q[0]) return;
    wp_strlcpy(G.last_query, q, sizeof G.last_query);
    int gen = ++G.results_gen;
    G.nresults = 0;
    char header[300];
    snprintf(header, sizeof header, "Searching YouTube for “%s”…", q);
    results_show(header);
    wp_yt_search(q, gen, on_search_results);
}

/* --------------------------------------------- rooms on the network */

static void rooms_hide(void)
{
    if (!G.rooms_visible) return;
    G.rooms_visible = 0;
    fade_view(G.rooms_panel, 0.0, 0.2);
}

static void rooms_show(void)
{
    if (!G.window) return;
    id content = MSG(id)(G.window, S("contentView"));
    int was_visible = G.rooms_visible;
    if (G.results_visible) results_hide();
    if (G.rooms_panel) { MSG(void)(G.rooms_panel, S("removeFromSuperview")); release(G.rooms_panel); G.rooms_panel = NULL; }

    const wp_room_state *rs = RS();
    CGRect bf = bar_frame();
    const double W = bf.size.width, row_h = 40, head_h = 32, foot_h = 44;
    int n = wp_room_nearby_count();
    int rows = n;
    while (rows > 0 && head_h + foot_h + 16 + rows * row_h > bf.origin.y - 40) rows--;
    double body_h = n ? rows * row_h : 46;
    double ph = head_h + body_h + foot_h;
    G.rooms_panel = make_panel(rect(bf.origin.x, bf.origin.y - 8 - ph, W, ph), 0);
    id panel = G.rooms_panel;

    id head = make_label(n ? "Rooms on your Wi-Fi — ask to join and the host lets you in, no code needed"
                           : "Rooms on your Wi-Fi",
                         rect(14, ph - head_h + 8, W - 28, 18), 12, 1);
    MSG(void, id)(head, S("setTextColor:"), white(0.85));
    MSG(void, unsigned long)(head, S("setLineBreakMode:"), 4);
    MSG(void, id)(panel, S("addSubview:"), head);

    if (!n) {
        id none = make_label("No rooms yet. When someone on this Wi-Fi hosts one, it shows up here (and you'll get a heads-up).",
                             rect(14, foot_h + 14, W - 28, 18), 12, 0);
        MSG(void, id)(none, S("setTextColor:"), white(0.65));
        MSG(void, unsigned long)(none, S("setLineBreakMode:"), 4);
        MSG(void, id)(panel, S("addSubview:"), none);
    }
    for (int i = 0; i < rows; i++) {
        const char *name = wp_room_nearby_name(i);
        if (!name) continue;
        double top = ph - head_h - i * row_h;
        char label[120];
        snprintf(label, sizeof label, "📺  %s", name);
        id l = make_label(label, rect(14, top - row_h + 11, W - 28 - 210, 20), 14, 1);
        MSG(void, id)(l, S("setTextColor:"), white(0.95));
        MSG(void, unsigned long)(l, S("setLineBreakMode:"), 4);
        MSG(void, id)(panel, S("addSubview:"), l);

        int here = rs->role == WP_ROLE_CLIENT && strcmp(rs->room, name) == 0;
        int asking = rs->ask != WP_ASK_NONE && strcmp(rs->ask_room, name) == 0;
        if (here || asking) {
            id s = make_label(here ? "you're in" : "asking…", rect(W - 12 - 200, top - row_h + 11, 200, 18), 12, 0);
            MSG(void, id)(s, S("setTextColor:"), white(0.6));
            MSG(void, long)(s, S("setAlignment:"), 1);                     /* right */
            MSG(void, id)(panel, S("addSubview:"), s);
            continue;
        }
        id code = make_button("Code…", "joinCode:", rect(W - 12 - 80, top - row_h + 7, 80, 28), 0);
        MSG(void, long)(code, S("setTag:"), (long)i);
        MSG(void, id)(code, S("setToolTip:"), nsstr("Join with the invite code the host shared"));
        MSG(void, id)(panel, S("addSubview:"), code);
        id ask = make_button("Ask to join", "askJoin:", rect(W - 12 - 80 - 6 - 116, top - row_h + 7, 116, 28), 0);
        MSG(void, long)(ask, S("setTag:"), (long)i);
        if (wp_room_nearby_can_ask(i)) {
            MSG(void, id)(ask, S("setToolTip:"), nsstr("The host gets a Let in / Not now card"));
        } else {
            MSG(void, BOOL)(ask, S("setEnabled:"), NO);
            MSG(void, id)(ask, S("setToolTip:"), nsstr("This host has an older WatchParty — use the invite code"));
        }
        MSG(void, id)(panel, S("addSubview:"), ask);
    }

    MSG(void, id)(panel, S("addSubview:"), make_button("Host a Room…", "hostRoom:", rect(12, 10, 124, 28), 0));
    id manual = make_button("Join with Address & Code…", "joinCode:", rect(142, 10, 206, 28), 0);
    MSG(void, long)(manual, S("setTag:"), -1L);        /* no particular room */
    MSG(void, id)(panel, S("addSubview:"), manual);
    MSG(void, id)(panel, S("addSubview:"), make_button("Close", "hideRooms:", rect(W - 12 - 80, 10, 80, 28), 0));

    MSG(void, double)(panel, S("setAlphaValue:"), was_visible ? 1.0 : 0.0);
    MSG(void, id)(content, S("addSubview:"), panel);
    G.rooms_visible = 1;
    if (!was_visible) fade_view(panel, 1.0, 0.2);
    bar_set(1);
}

/* ----------------------------------------------- cards on the left */

static void card_remove(int i, int animated)
{
    if (i < 0 || i >= MAX_CARDS || G.cards[i].kind == CARD_NONE) return;
    id v = G.cards[i].view;
    memset(&G.cards[i], 0, sizeof G.cards[i]);
    if (v) {
        id subs = MSG(id)(v, S("subviews"));            /* a fading card takes no more clicks */
        unsigned long ns = MSG(unsigned long)(subs, S("count"));
        for (unsigned long k = 0; k < ns; k++) {
            id sv = MSG(id, unsigned long)(subs, S("objectAtIndex:"), k);
            if (MSG(BOOL, id)(sv, S("isKindOfClass:"), C("NSButton"))) MSG(void, BOOL)(sv, S("setEnabled:"), NO);
        }
        if (animated) {
            fade_view(v, 0.0, 0.25);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                MSG(void)(v, S("removeFromSuperview"));
                release(v);
            });
        } else {
            MSG(void)(v, S("removeFromSuperview"));
            release(v);
        }
    }
    cards_layout();
}

static int card_find(int kind, int key, const char *room)
{
    for (int i = 0; i < MAX_CARDS; i++) {
        if (G.cards[i].kind != kind) continue;
        if (kind == CARD_KNOCK && G.cards[i].key != key) continue;
        if (kind == CARD_ROOM && room && strcmp(G.cards[i].room, room) != 0) continue;
        return i;
    }
    return -1;
}

static int card_slot(void)
{
    for (int i = 0; i < MAX_CARDS; i++) if (G.cards[i].kind == CARD_NONE) return i;
    int oldest = -1;                                  /* full: drop the oldest heads-up */
    for (int i = 0; i < MAX_CARDS; i++)
        if (G.cards[i].kind == CARD_ROOM && (oldest < 0 || G.cards[i].gen < G.cards[oldest].gen)) oldest = i;
    if (oldest >= 0) { card_remove(oldest, 0); return oldest; }
    return -1;
}

static void cards_layout(void)
{
    if (!G.window) return;
    CGRect b = content_bounds();
    double y = b.size.height - CHAT_TOP;
    int done[MAX_CARDS] = { 0 };
    for (;;) {                                         /* oldest first, top down */
        int pick = -1;
        for (int i = 0; i < MAX_CARDS; i++)
            if (G.cards[i].kind != CARD_NONE && !done[i] && (pick < 0 || G.cards[i].gen < G.cards[pick].gen)) pick = i;
        if (pick < 0) break;
        done[pick] = 1;
        CGRect f = MSG_STRET(CGRect)(G.cards[pick].view, S("frame"));
        y -= f.size.height;
        MSG(void, CGRect)(G.cards[pick].view, S("setFrame:"), rect(PAD, y, f.size.width, f.size.height));
        y -= 8;
    }
}

/* A card: bold title, a line or two of text, up to two buttons (tag = the card's slot). */
static void card_build(int i, int kind, const char *title, const char *body,
                       const char *b1, const char *sel1, const char *b2, const char *sel2)
{
    if (!G.window) return;
    id content = MSG(id)(G.window, S("contentView"));
    const double inner = CARD_W - 28;
    id t = MSG(id, id)(C("NSTextField"), S("wrappingLabelWithString:"), nsstr(title));
    MSG(void, id)(t, S("setFont:"), sysfont(13, 1));
    MSG(void, id)(t, S("setTextColor:"), white(0.97));
    MSG(void, BOOL)(t, S("setSelectable:"), NO);
    CGSize ts = MSG(CGSize, CGSize)(t, S("sizeThatFits:"), (CGSize){ inner, 400 });
    id d = MSG(id, id)(C("NSTextField"), S("wrappingLabelWithString:"), nsstr(body ? body : ""));
    MSG(void, id)(d, S("setFont:"), sysfont(12, 0));
    MSG(void, id)(d, S("setTextColor:"), white(0.78));
    MSG(void, BOOL)(d, S("setSelectable:"), NO);
    CGSize ds = (body && body[0]) ? MSG(CGSize, CGSize)(d, S("sizeThatFits:"), (CGSize){ inner, 400 }) : (CGSize){ 0, 0 };
    const double btn_h = (b1 || b2) ? 38 : 0;
    double h = 12 + ts.height + (ds.height ? 4 + ds.height : 0) + 10 + btn_h;

    id v = make_panel(rect(PAD, 0, CARD_W, h), MASK_MINY);
    double y = h - 12 - ts.height;
    MSG(void, CGRect)(t, S("setFrame:"), rect(14, y, inner, ts.height));
    MSG(void, id)(v, S("addSubview:"), t);
    if (ds.height) {
        y -= 4 + ds.height;
        MSG(void, CGRect)(d, S("setFrame:"), rect(14, y, inner, ds.height));
        MSG(void, id)(v, S("addSubview:"), d);
    }
    if (b2) {                                          /* the main action, on the right */
        id b = make_button(b2, sel2, rect(CARD_W - 12 - 112, 10, 112, 28), 0);
        MSG(void, long)(b, S("setTag:"), (long)i);
        MSG(void, id)(v, S("addSubview:"), b);
    }
    if (b1) {
        id b = make_button(b1, sel1, rect(b2 ? CARD_W - 12 - 112 - 6 - 96 : CARD_W - 12 - 96, 10, 96, 28), 0);
        MSG(void, long)(b, S("setTag:"), (long)i);
        MSG(void, id)(v, S("addSubview:"), b);
    }

    card_t *c = &G.cards[i];
    if (c->view) { MSG(void)(c->view, S("removeFromSuperview")); release(c->view); c->view = NULL; }
    c->kind = kind;
    if (!c->gen) c->gen = ++G.card_gen;
    c->view = v;
    MSG(void, double)(v, S("setAlphaValue:"), 0.0);
    MSG(void, id)(content, S("addSubview:"), v);
    fade_view(v, 1.0, 0.2);
    cards_layout();
}

/* Dock badge + menu-bar count while people are waiting at the door. */
static void attention_refresh(void)
{
    int n = wp_room_pending_knocks();
    char num[16];
    snprintf(num, sizeof num, "%d", n);
    id app = MSG(id)(C("NSApplication"), S("sharedApplication"));
    MSG(void, id)(MSG(id)(app, S("dockTile")), S("setBadgeLabel:"), n ? nsstr(num) : NULL);
    if (G.status_item) {
        id button = MSG(id)(G.status_item, S("button"));
        char t[24];
        snprintf(t, sizeof t, " %d", n);
        MSG(void, id)(button, S("setTitle:"), nsstr(n ? t : ""));
        MSG(void, unsigned long)(button, S("setImagePosition:"), n ? 2 : 1);    /* image left / image only */
    }
}

static void ui_knock(int knock_id, const char *name, const char *check)
{
    int i = card_slot();
    if (i < 0) return;
    char title[96], body[300];
    snprintf(title, sizeof title, "%s wants to join", name);
    snprintf(body, sizeof body, "Check code %s — it's on %s's screen too. If it matches, it's really them.", check, name);
    G.cards[i].gen = 0;
    card_build(i, CARD_KNOCK, title, body, "Not now", "knockDeny:", "Let in", "knockAllow:");
    G.cards[i].key = knock_id;
    if (G.window && !MSG(BOOL)(G.window, S("isVisible"))) MSG(void)(G.window, S("orderFrontRegardless"));
    MSG(long, unsigned long)(MSG(id)(C("NSApplication"), S("sharedApplication")), S("requestUserAttention:"), 10);  /* bounce once */
    attention_refresh();
    update_info();
}

static void ui_knock_gone(int knock_id)
{
    int i = card_find(CARD_KNOCK, knock_id, NULL);
    if (i >= 0) card_remove(i, 1);
    attention_refresh();
    update_info();
}

/* our own "ask to join" in progress */
static void ask_card_refresh(void)
{
    const wp_room_state *rs = RS();
    if (rs->role != WP_ROLE_NONE || rs->ask != WP_ASK_NONE)       /* in (or getting into) a room: */
        for (int k = 0; k < MAX_CARDS; k++) if (G.cards[k].kind == CARD_ROOM) card_remove(k, 1);   /* no heads-ups */
    int i = card_find(CARD_ASK, 0, NULL);
    if (rs->ask == WP_ASK_NONE) { if (i >= 0) card_remove(i, 1); return; }
    int state = rs->ask * 10000 + atoi(rs->ask_check);
    if (i >= 0 && G.cards[i].key == state) return;
    if (i < 0) { i = card_slot(); if (i < 0) return; G.cards[i].gen = 0; }
    char title[120], body[300];
    if (rs->ask == WP_ASK_WAITING) {
        snprintf(title, sizeof title, "Waiting for “%s” to let you in", rs->ask_room);
        snprintf(body, sizeof body, "Your check code is %s — the host sees the same code, so they know it's you.", rs->ask_check);
    } else {
        snprintf(title, sizeof title, "Asking “%s”…", rs->ask_room);
        snprintf(body, sizeof body, "Knocking on the host's door.");
    }
    card_build(i, CARD_ASK, title, body, NULL, NULL, "Cancel", "askCancel:");
    G.cards[i].key = state;
}

static void ui_rooms_changed(void)
{
    if (!G.window) return;                            /* not up yet: launch catches up */
    if (G.join_popup) populate_join_popup(G.join_popup);
    if (G.rooms_visible) rooms_show();
    const wp_room_state *rs = RS();
    int n = wp_room_nearby_count();
    /* heads-up cards go when their room does */
    for (int i = 0; i < MAX_CARDS; i++) {
        if (G.cards[i].kind != CARD_ROOM) continue;
        int still = 0;
        for (int j = 0; j < n; j++) if (strcmp(wp_room_nearby_name(j), G.cards[i].room) == 0) still = 1;
        if (!still) card_remove(i, 1);
    }
    for (int j = 0; j < n; j++) {
        const char *name = wp_room_nearby_name(j);
        int seen = 0;
        for (int k = 0; k < G.n_seen; k++) if (strcmp(G.seen_rooms[k], name) == 0) seen = 1;
        if (seen) continue;
        if (G.n_seen < WP_MAX_ROOMS) wp_strlcpy(G.seen_rooms[G.n_seen++], name, sizeof G.seen_rooms[0]);
        if (rs->role != WP_ROLE_NONE || rs->ask != WP_ASK_NONE) continue;      /* busy watching: no interruptions */
        int i = card_slot();
        if (i < 0) continue;
        char title[120];
        snprintf(title, sizeof title, "“%s” is on your Wi-Fi", name);
        G.cards[i].gen = 0;
        if (wp_room_nearby_can_ask(j)) card_build(i, CARD_ROOM, title, "Someone nearby is hosting a watch party.", "Not now", "cardDismiss:", "Ask to join", "cardAsk:");
        else card_build(i, CARD_ROOM, title, "Someone nearby is hosting a watch party.", "Not now", "cardDismiss:", "Join with code…", "cardCode:");
        wp_strlcpy(G.cards[i].room, name, sizeof G.cards[i].room);
        int gen = G.cards[i].gen;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)TOAST_MS * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (G.cards[i].kind == CARD_ROOM && G.cards[i].gen == gen) card_remove(i, 1);
        });
    }
    update_info();
}

/* ------------------------------------------------------------ dialogs */

static int nearby_index_of(const char *name)
{
    int n = wp_room_nearby_count();
    for (int i = 0; i < n; i++) if (strcmp(wp_room_nearby_name(i), name) == 0) return i;
    return -1;
}

static id begin_alert(const char *title, const char *info, const char *ok, double acc_h)
{
    id alert = alloc_init("NSAlert");
    MSG(void, id)(alert, S("setMessageText:"), nsstr(title));
    MSG(void, id)(alert, S("setInformativeText:"), nsstr(info));
    MSG(id, id)(alert, S("addButtonWithTitle:"), nsstr(ok));
    MSG(id, id)(alert, S("addButtonWithTitle:"), nsstr("Cancel"));
    if (acc_h > 0) {
        id acc = MSG(id, CGRect)(MSG(id)(C("NSView"), S("alloc")), S("initWithFrame:"), rect(0, 0, 360, acc_h));
        MSG(void, id)(alert, S("setAccessoryView:"), acc);
        release(acc);
    }
    return alert;
}

/* Hosting with people in the room? Make sure before walking out on them. */
static int ok_to_leave(const char *doing)
{
    const wp_room_state *rs = RS();
    if (rs->role != WP_ROLE_HOST || wp_room_watching() < 2) return 1;
    char info[200];
    snprintf(info, sizeof info, "To %s you'll close “%s” for everyone watching.", doing, rs->room);
    id alert = begin_alert("Close your room?", info, "Close Room", 0);
    long r = MSG(long)(alert, S("runModal"));
    release(alert);
    return r == 1000;
}

static void run_host_dialog(void)
{
    id alert = begin_alert("Host a Room",
                           "People on your Wi-Fi see your room and can ask to join — you get a Let in / Not now card. "
                           "Anyone else can join with the invite code (⌘I copies it).",
                           "Host", 64);
    id acc = MSG(id)(alert, S("accessoryView"));
    char room_default[WP_ROOM_MAX + 1];
    snprintf(room_default, sizeof room_default, "%s's room", default_name());
    id f_name = make_field(rect(100, 36, 260, 24), "Your name", default_name());
    id f_room = make_field(rect(100, 4, 260, 24), "Movie night", room_default);
    MSG(void, id)(acc, S("addSubview:"), make_label("Your name", rect(0, 38, 96, 20), 13, 0));
    MSG(void, id)(acc, S("addSubview:"), f_name);
    MSG(void, id)(acc, S("addSubview:"), make_label("Room name", rect(0, 6, 96, 20), 13, 0));
    MSG(void, id)(acc, S("addSubview:"), f_room);
    MSG(void, id)(f_name, S("setNextKeyView:"), f_room);
    MSG(void, id)(MSG(id)(alert, S("window")), S("setInitialFirstResponder:"), f_name);

    long r = MSG(long)(alert, S("runModal"));
    if (r == 1000 && ok_to_leave("host a new room")) {
        char name[WP_NAME_MAX + 1], room[WP_ROOM_MAX + 1];
        wp_strlcpy(name, cstr(MSG(id)(f_name, S("stringValue"))), sizeof name);
        wp_strlcpy(room, cstr(MSG(id)(f_room, S("stringValue"))), sizeof room);
        remember_name(name);
        rooms_hide();
        wp_room_host(name, room);
    }
    release(f_name);
    release(f_room);
    release(alert);
}

static void populate_join_popup(id popup)
{
    id selected = MSG(id)(popup, S("titleOfSelectedItem"));
    char keep[128];
    wp_strlcpy(keep, selected ? cstr(selected) : "", sizeof keep);
    MSG(void)(popup, S("removeAllItems"));
    int n = wp_room_nearby_count();
    for (int i = 0; i < n; i++) MSG(void, id)(popup, S("addItemWithTitle:"), nsstr(wp_room_nearby_name(i)));
    static const char *manual_some = "Somewhere else — use the address below";
    static const char *manual_none = "No rooms found nearby — use the address below";
    MSG(void, id)(popup, S("addItemWithTitle:"), nsstr(n ? manual_some : manual_none));
    long sel = n;                                                   /* default: manual */
    int chose_room = 0;
    for (int i = 0; i < n; i++) if (strcmp(keep, wp_room_nearby_name(i)) == 0) { sel = i; chose_room = 1; }
    if (!chose_room && n > 0 && (!keep[0] || strcmp(keep, manual_none) == 0)) sel = 0;
    MSG(void, long)(popup, S("selectItemAtIndex:"), sel);
}

static void run_join_dialog(int preselect)
{
    wp_net_browse_start();
    id alert = begin_alert("Join with an Invite Code",
                           "Pick the room (or type the host's address) and enter the code the host shared. "
                           "No code? Use Rooms (⌘J) ▸ Ask to join — the host lets you in.",
                           "Join", 128);
    id acc = MSG(id)(alert, S("accessoryView"));
    id f_name = make_field(rect(100, 100, 260, 24), "Your name", default_name());
    id popup = MSG(id, CGRect, BOOL)(MSG(id)(C("NSPopUpButton"), S("alloc")), S("initWithFrame:pullsDown:"), rect(97, 66, 266, 26), NO);
    populate_join_popup(popup);
    if (preselect >= 0 && preselect < wp_room_nearby_count()) MSG(void, long)(popup, S("selectItemAtIndex:"), (long)preselect);
    id f_addr = make_field(rect(100, 36, 260, 24), "192.168.1.20 or host:port", NULL);
    id f_code = make_field(rect(100, 4, 260, 24), "abcd-efgh-ijkl-mnop-qrst-uvwx", NULL);
    MSG(void, id)(acc, S("addSubview:"), make_label("Your name", rect(0, 102, 96, 20), 13, 0));
    MSG(void, id)(acc, S("addSubview:"), f_name);
    MSG(void, id)(acc, S("addSubview:"), make_label("Nearby room", rect(0, 70, 96, 20), 13, 0));
    MSG(void, id)(acc, S("addSubview:"), popup);
    MSG(void, id)(acc, S("addSubview:"), make_label("Address", rect(0, 38, 96, 20), 13, 0));
    MSG(void, id)(acc, S("addSubview:"), f_addr);
    MSG(void, id)(acc, S("addSubview:"), make_label("Invite code", rect(0, 6, 96, 20), 13, 0));
    MSG(void, id)(acc, S("addSubview:"), f_code);
    MSG(void, id)(f_name, S("setNextKeyView:"), popup);
    MSG(void, id)(popup, S("setNextKeyView:"), f_addr);
    MSG(void, id)(f_addr, S("setNextKeyView:"), f_code);
    MSG(void, id)(MSG(id)(alert, S("window")), S("setInitialFirstResponder:"), f_code);

    G.join_popup = popup;
    long r = MSG(long)(alert, S("runModal"));
    G.join_popup = NULL;
    char picked[64] = "";
    long idx0 = MSG(long)(popup, S("indexOfSelectedItem"));
    if (r == 1000 && idx0 >= 0 && idx0 < wp_room_nearby_count()) wp_strlcpy(picked, wp_room_nearby_name((int)idx0), sizeof picked);
    if (r == 1000 && ok_to_leave("join another room")) {
        char name[WP_NAME_MAX + 1], addr[256], code[128];
        wp_strlcpy(name, cstr(MSG(id)(f_name, S("stringValue"))), sizeof name);
        wp_strlcpy(addr, cstr(MSG(id)(f_addr, S("stringValue"))), sizeof addr);
        wp_strlcpy(code, cstr(MSG(id)(f_code, S("stringValue"))), sizeof code);
        int room_index = picked[0] ? nearby_index_of(picked) : -1;
        wp_trim(addr);
        if (room_index >= 0 && addr[0]) room_index = -1;           /* typed address wins */
        remember_name(name);
        rooms_hide();
        wp_room_join(name, room_index, addr, code);
    }
    release(f_name); release(popup); release(f_addr); release(f_code);
    release(alert);
}

static void ask_nearby(int index)
{
    if (index < 0 || index >= wp_room_nearby_count()) return;
    char room[64];
    wp_strlcpy(room, wp_room_nearby_name(index), sizeof room);
    if (!ok_to_leave("join another room")) return;     /* (the list can change during the dialog) */
    index = nearby_index_of(room);
    if (index < 0) { say("“%s” isn't on the network any more.", room); return; }
    rooms_hide();
    wp_room_ask(default_name(), index);
}


/* --------------------------------------------- controller (ObjC class) */

static void ctl_load_url(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    char text[2048];
    wp_strlcpy(text, cstr(MSG(id)(G.url_field, S("stringValue"))), sizeof text);
    wp_trim(text);
    if (!text[0]) {                                   /* ⌘L with nothing typed: open the bar for typing */
        bar_set(1);
        MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.url_field);
        return;
    }
    if (!looks_like_link(text)) {
        /* Return again on the same words plays the first result */
        if (G.results_visible && G.nresults > 0 && strcmp(text, G.last_query) == 0) {
            wp_room_load(G.results[0].id);
            results_hide();
            MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.web);
            return;
        }
        yt_search(text);
        return;
    }
    results_hide();
    wp_room_load(text);
    MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.web);     /* keyboard back to the player */
}

static void ctl_pick_result(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd;
    long i = MSG(long)(sender, S("tag"));
    if (i < 0 || i >= G.nresults) return;
    char vid[WP_VIDEO_ID_LEN + 1];
    wp_strlcpy(vid, G.results[i].id, sizeof vid);
    results_hide();
    wp_room_load(vid);
    MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.web);
}

static void ctl_host_room(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; show_window(); run_host_dialog(); }
static void ctl_join_room(id self, SEL cmd, id sender)          /* ⌘J, "Rooms": the rooms on this Wi-Fi */
{
    (void)self; (void)cmd; (void)sender;
    show_window();
    if (G.rooms_visible) rooms_hide(); else rooms_show();
}
static void ctl_join_code(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd;
    long tag = sender && responds(sender, "tag") ? MSG(long)(sender, S("tag")) : -1;
    show_window();
    run_join_dialog((int)tag);
}
static void ctl_ask_join(id self, SEL cmd, id sender) { (void)self; (void)cmd; show_window(); ask_nearby((int)MSG(long)(sender, S("tag"))); }
static void ctl_hide_rooms(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; rooms_hide(); }
static void ctl_ask_cancel(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; wp_room_ask_cancel(); }

static void ctl_knock_answer(id sender, int allow)
{
    long i = MSG(long)(sender, S("tag"));
    if (i < 0 || i >= MAX_CARDS || G.cards[i].kind != CARD_KNOCK) return;
    wp_room_answer(G.cards[i].key, allow);
}
static void ctl_knock_allow(id self, SEL cmd, id sender) { (void)self; (void)cmd; ctl_knock_answer(sender, 1); }
static void ctl_knock_deny(id self, SEL cmd, id sender) { (void)self; (void)cmd; ctl_knock_answer(sender, 0); }

static void ctl_card_dismiss(id self, SEL cmd, id sender) { (void)self; (void)cmd; card_remove((int)MSG(long)(sender, S("tag")), 1); }

static void ctl_card_room(id sender, int ask)
{
    long i = MSG(long)(sender, S("tag"));
    if (i < 0 || i >= MAX_CARDS || G.cards[i].kind != CARD_ROOM) return;
    int idx = nearby_index_of(G.cards[i].room);
    card_remove((int)i, 1);
    if (idx < 0) { say("That room isn't on the network any more."); return; }
    if (ask) ask_nearby(idx); else run_join_dialog(idx);
}
static void ctl_card_ask(id self, SEL cmd, id sender) { (void)self; (void)cmd; ctl_card_room(sender, 1); }
static void ctl_card_code(id self, SEL cmd, id sender) { (void)self; (void)cmd; ctl_card_room(sender, 0); }

/* Room ▸ Ask to Join ▸ … is rebuilt each time it opens */
static void ctl_menu_needs_update(id self, SEL cmd, id menu)
{
    (void)self; (void)cmd;
    if (menu != G.nearby_menu && menu != G.status_nearby_menu) return;
    MSG(void)(menu, S("removeAllItems"));
    int n = wp_room_nearby_count();
    if (!n) {
        id it = MSG(id, id, SEL, id)(MSG(id)(C("NSMenuItem"), S("alloc")), S("initWithTitle:action:keyEquivalent:"),
                                     nsstr("No rooms on your Wi-Fi yet"), (SEL)NULL, nsstr(""));
        MSG(void, BOOL)(it, S("setEnabled:"), NO);
        MSG(void, id)(menu, S("addItem:"), it);
        release(it);
    }
    for (int i = 0; i < n; i++) {
        char title[120];
        snprintf(title, sizeof title, "“%s”%s", wp_room_nearby_name(i), wp_room_nearby_can_ask(i) ? "" : "  (needs the code)");
        id it = add_item(menu, title, wp_room_nearby_can_ask(i) ? "askJoin:" : "joinCode:", "", 0, 1);
        MSG(void, long)(it, S("setTag:"), (long)i);
    }
}
static void ctl_copy_invite(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; copy_invite(); }
static void ctl_leave_room(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    if (RS()->ask != WP_ASK_NONE) { wp_room_ask_cancel(); return; }
    if (ok_to_leave("leave")) wp_room_leave("— Left the room —");
}
static void ctl_play_pause(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; js("wp.toggle()"); }
static void ctl_show_window(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; show_window(); }
static void ctl_close_window(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; MSG(void, id)(G.window, S("orderOut:"), NULL); }

static void ctl_toggle_hud(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    if (G.bar_visible) { bar_set(0); G.bar_suppressed = 1; }
    else bar_set(1);
}

static void ctl_chat_panel(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; set_chat_mode(CHAT_PANEL); }
static void ctl_chat_subtitles(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; set_chat_mode(CHAT_SUBTITLES); }
static void ctl_chat_off(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; set_chat_mode(CHAT_OFF); }
static void ctl_chat_cycle(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; set_chat_mode((G.chat_mode + 1) % 3); }

static void ctl_send_emoji(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd;
    const char *e = cstr(MSG(id)(sender, S("title")));
    if (e[0]) wp_room_chat(e);
}

static void ctl_send_reaction(id self, SEL cmd, id sender) { (void)self; (void)cmd; wp_room_react((int)MSG(long)(sender, S("tag"))); }

static void ctl_chat_font(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd;
    long i = MSG(long)(sender, S("tag"));
    if (i >= 0 && i < N_FONTS) { G.font_choice = (int)i; apply_text_style(); }
}

static void ctl_toggle_fancy(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    RS()->fancy = !RS()->fancy;
    if (G.fancy_item) MSG(void, long)(G.fancy_item, S("setState:"), RS()->fancy ? 1 : 0);
    id defaults = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    MSG(void, BOOL, id)(defaults, S("setBool:forKey:"), RS()->fancy ? YES : NO, nsstr("fancyWords"));
    log_line(RS()->fancy ? "Fancy words on: cuss words now appear as their most magniloquent equivalents."
                     : "Fancy words off: messages show exactly as typed.");
}

static void ctl_chat_color(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd;
    long i = MSG(long)(sender, S("tag"));
    if (i >= 0 && i < N_COLORS) { G.color_choice = (int)i; apply_text_style(); }
}

static void ctl_toggle_float(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    G.floating = !G.floating;
    MSG(void, long)(G.window, S("setLevel:"), G.floating ? 3 /* NSFloatingWindowLevel */ : 0);
    if (G.float_item) MSG(void, long)(G.float_item, S("setState:"), G.floating ? 1 : 0);
    id defaults = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    MSG(void, BOOL, id)(defaults, S("setBool:forKey:"), G.floating ? YES : NO, nsstr("floatOnTop"));
}

static void ctl_sync_now(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    wp_room_sync_now();
}

static void ctl_chat_enter(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    char text[WP_TEXT_MAX + 1];
    wp_strlcpy(text, cstr(MSG(id)(G.chat_input, S("stringValue"))), sizeof text);
    MSG(void, id)(G.chat_input, S("setStringValue:"), nsstr(""));
    wp_room_chat(text);
}

static void ctl_msg_enter(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    char text[WP_TEXT_MAX + 1];
    wp_strlcpy(text, cstr(MSG(id)(G.msg_field, S("stringValue"))), sizeof text);
    wp_trim(text);
    if (!text[0]) { msgbar_set(0); return; }          /* Return on an empty box closes it */
    MSG(void, id)(G.msg_field, S("setStringValue:"), nsstr(""));
    G.msgbar_activity = wp_now_ms();                  /* linger a moment for a follow-up */
    wp_room_chat(text);
    if (G.chat_mode == CHAT_OFF) {                    /* nothing on screen shows it: confirm in the box */
        MSG(void, id)(G.msg_field, S("setPlaceholderString:"), nsstr("Sent ✓"));
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            MSG(void, id)(G.msg_field, S("setPlaceholderString:"), nsstr("Message the room… (Return sends, Esc hides)"));
        });
    }
}

static void ctl_script_message(id self, SEL cmd, id controller, id message)
{
    (void)self; (void)cmd; (void)controller;
    id body = MSG(id)(message, S("body"));
    if (body && MSG(BOOL, id)(body, S("isKindOfClass:"), C("NSString"))) wp_room_player_event(cstr(body));
}

static BOOL ctl_validate_menu_item(id self, SEL cmd, id item)
{
    (void)self; (void)cmd;
    SEL action = MSG(SEL)(item, S("action"));
    const wp_room_state *rs = RS();
    if (action == S("copyInvite:")) return rs->role == WP_ROLE_HOST;
    if (action == S("leaveRoom:")) return rs->role != WP_ROLE_NONE || rs->ask != WP_ASK_NONE;
    if (action == S("syncNow:")) return rs->role != WP_ROLE_NONE;
    if (action == S("playPause:")) return rs->player_ready && rs->video[0] != 0;
    return YES;
}

static BOOL ctl_no(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; return NO; }
static BOOL ctl_yes(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; return YES; }
static BOOL ctl_yes0(id self, SEL cmd) { (void)self; (void)cmd; return YES; }
static id   ctl_nil1(id self, SEL cmd, CGPoint p) { (void)self; (void)cmd; (void)p; return NULL; }

static BOOL ctl_reopen(id self, SEL cmd, id app, BOOL visible)
{
    (void)self; (void)cmd; (void)app; (void)visible;
    show_window();
    return YES;
}

static void ctl_will_terminate(id self, SEL cmd, id note)
{
    (void)self; (void)cmd; (void)note;
    wp_room_shutdown();
}

static void ctl_mouse_moved(id self, SEL cmd, id event)
{
    (void)self; (void)cmd;
    G.last_mouse_move = wp_now_ms();
    CGPoint p = MSG(CGPoint)(event, S("locationInWindow"));
    bar_update(in_bar_zone(p));
    msgbar_update(in_msgbar_zone(p));
}

static void ctl_mouse_exited(id self, SEL cmd, id event)
{
    (void)self; (void)cmd; (void)event;
    bar_update(0);
    msgbar_update(0);
}

static void ctl_window_resized(id self, SEL cmd, id note) { (void)self; (void)cmd; (void)note; layout_overlays(); }

static void build_menus(void)
{
    id app = MSG(id)(C("NSApplication"), S("sharedApplication"));
    id bar = MSG(id, id)(MSG(id)(C("NSMenu"), S("alloc")), S("initWithTitle:"), nsstr("MainMenu"));

    id app_menu = add_menu(bar, WP_APP_NAME);
    add_item(app_menu, "About " WP_APP_NAME, "orderFrontStandardAboutPanel:", "", 0, 0);
    add_separator(app_menu);
    add_item(app_menu, "Hide " WP_APP_NAME, "hide:", "h", 0, 0);
    add_item(app_menu, "Hide Others", "hideOtherApplications:", "h", MOD_COMMAND | MOD_OPTION, 0);
    add_item(app_menu, "Show All", "unhideAllApplications:", "", 0, 0);
    add_separator(app_menu);
    add_item(app_menu, "Quit " WP_APP_NAME, "terminate:", "q", 0, 0);

    id edit = add_menu(bar, "Edit");
    add_item(edit, "Undo", "undo:", "z", 0, 0);
    add_item(edit, "Redo", "redo:", "Z", MOD_COMMAND | MOD_SHIFT, 0);
    add_separator(edit);
    add_item(edit, "Cut", "cut:", "x", 0, 0);
    add_item(edit, "Copy", "copy:", "c", 0, 0);
    add_item(edit, "Paste", "paste:", "v", 0, 0);
    add_item(edit, "Select All", "selectAll:", "a", 0, 0);
    add_separator(edit);
    add_item(edit, "Emoji & Symbols", "orderFrontCharacterPalette:", " ", MOD_COMMAND | MOD_CONTROL, 0);

    id room = add_menu(bar, "Room");
    add_item(room, "Host a Room…", "hostRoom:", "n", 0, 1);
    add_item(room, "Rooms on This Wi-Fi…", "joinRoom:", "j", 0, 1);
    G.nearby_menu = add_menu(room, "Ask to Join");
    MSG(void, id)(G.nearby_menu, S("setDelegate:"), G.ctl);
    id jc = add_item(room, "Join with Invite Code…", "joinCode:", "j", MOD_COMMAND | MOD_SHIFT, 1);
    MSG(void, long)(jc, S("setTag:"), -1L);
    add_separator(room);
    add_item(room, "Copy Invite", "copyInvite:", "i", 0, 1);
    add_item(room, "Leave Room", "leaveRoom:", "l", MOD_COMMAND | MOD_SHIFT, 1);

    id video = add_menu(bar, "Video");
    add_item(video, "Load Link…", "loadURL:", "l", 0, 1);
    add_item(video, "Play / Pause", "playPause:", "p", 0, 1);
    add_item(video, "Sync Now", "syncNow:", "r", 0, 1);
    add_separator(video);
    id hud_item = add_item(video, "Show / Hide Controls", "toggleHUD:", "\x1b", 0, 1);   /* shows as ⎋ */
    MSG(void, unsigned long)(hud_item, S("setKeyEquivalentModifierMask:"), 0);
    G.float_item = add_item(video, "Float on Top", "toggleFloat:", "t", MOD_COMMAND | MOD_SHIFT, 1);
    MSG(void, long)(G.float_item, S("setState:"), G.floating ? 1 : 0);
    add_item(video, "Enter Full Screen", "toggleFullScreen:", "f", MOD_COMMAND | MOD_CONTROL, 0);

    id chat = add_menu(bar, "Chat");
    G.chat_items[CHAT_PANEL]     = add_item(chat, "Chat Column", "chatPanel:", "", 0, 1);
    G.chat_items[CHAT_SUBTITLES] = add_item(chat, "Chat as Subtitles", "chatSubtitles:", "", 0, 1);
    G.chat_items[CHAT_OFF]       = add_item(chat, "Hide Chat", "chatOff:", "", 0, 1);
    add_separator(chat);
    G.fancy_item = add_item(chat, "Fancy Words for Cuss Words", "toggleFancy:", "", 0, 1);
    MSG(void, long)(G.fancy_item, S("setState:"), RS()->fancy ? 1 : 0);
    add_separator(chat);
    add_item(chat, "Next Chat Mode", "chatCycle:", "c", MOD_COMMAND | MOD_SHIFT, 1);
    add_separator(chat);
    id fonts = add_menu(chat, "Font");
    for (int i = 0; i < N_FONTS; i++) {
        G.font_items[i] = add_item(fonts, FONTS[i].label, "chatFont:", "", 0, 1);
        MSG(void, long)(G.font_items[i], S("setTag:"), (long)i);
    }
    id colors = add_menu(chat, "Colour");
    for (int i = 0; i < N_COLORS; i++) {
        G.color_items[i] = add_item(colors, COLORS[i].label, "chatColor:", "", 0, 1);
        MSG(void, long)(G.color_items[i], S("setTag:"), (long)i);
    }
    id reactions = add_menu(chat, "Send Reaction");
    for (int i = 0; i < N_REACTIONS; i++) {
        char title[96];
        if (REACTIONS[i].emoji[0]) snprintf(title, sizeof title, "%s  %s", REACTIONS[i].emoji, REACTIONS[i].word);
        else snprintf(title, sizeof title, "%s", REACTIONS[i].word);
        id it = add_item(reactions, title, "sendReaction:", i < 9 ? (char[]){ (char)('1' + i), 0 } : "", i < 9 ? MOD_COMMAND | MOD_OPTION : 0, 1);
        MSG(void, long)(it, S("setTag:"), (long)i);
        if (REACTIONS[i].image) {
            id im = sticker_image(REACTIONS[i].image, 18);
            if (im) { MSG(void, id)(it, S("setImage:"), im); release(im); }
        }
    }

    id window = add_menu(bar, "Window");
    add_item(window, "Minimize", "performMiniaturize:", "m", 0, 0);
    add_item(window, "Close", "closeWindow:", "w", 0, 1);
    add_separator(window);
    add_item(window, "Show " WP_APP_NAME, "showWindow:", "0", 0, 1);
    MSG(void, id)(app, S("setWindowsMenu:"), window);

    MSG(void, id)(app, S("setMainMenu:"), bar);
    release(bar);
}

static void build_status_item(void)
{
    id bar = MSG(id)(C("NSStatusBar"), S("systemStatusBar"));
    G.status_item = retain(MSG(id, double)(bar, S("statusItemWithLength:"), -1.0));   /* NSVariableStatusItemLength */
    id button = MSG(id)(G.status_item, S("button"));
    id image = NULL;
    if (responds(C("NSImage"), "imageWithSystemSymbolName:accessibilityDescription:")) {
        image = MSG(id, id, id)(C("NSImage"), S("imageWithSystemSymbolName:accessibilityDescription:"),
                                nsstr("play.tv"), nsstr(WP_APP_NAME));
        if (!image) image = MSG(id, id, id)(C("NSImage"), S("imageWithSystemSymbolName:accessibilityDescription:"),
                                            nsstr("play.rectangle"), nsstr(WP_APP_NAME));
    }
    if (image) MSG(void, id)(button, S("setImage:"), image);
    else MSG(void, id)(button, S("setTitle:"), nsstr("▶︎"));
    MSG(void, id)(button, S("setToolTip:"), nsstr(WP_APP_NAME));

    G.status_menu = MSG(id, id)(MSG(id)(C("NSMenu"), S("alloc")), S("initWithTitle:"), nsstr(WP_APP_NAME));
    add_item(G.status_menu, "Show " WP_APP_NAME, "showWindow:", "", 0, 1);
    add_separator(G.status_menu);
    add_item(G.status_menu, "Host a Room…", "hostRoom:", "", 0, 1);
    add_item(G.status_menu, "Rooms on This Wi-Fi…", "joinRoom:", "", 0, 1);
    G.status_nearby_menu = add_menu(G.status_menu, "Ask to Join");
    MSG(void, id)(G.status_nearby_menu, S("setDelegate:"), G.ctl);
    id jc = add_item(G.status_menu, "Join with Invite Code…", "joinCode:", "", 0, 1);
    MSG(void, long)(jc, S("setTag:"), -1L);
    add_item(G.status_menu, "Copy Invite", "copyInvite:", "", 0, 1);
    add_item(G.status_menu, "Leave Room", "leaveRoom:", "", 0, 1);
    add_separator(G.status_menu);
    add_item(G.status_menu, "Play / Pause", "playPause:", "", 0, 1);
    add_item(G.status_menu, "Next Chat Mode", "chatCycle:", "", 0, 1);
    add_item(G.status_menu, "Float on Top", "toggleFloat:", "", 0, 1);
    add_separator(G.status_menu);
    add_item(G.status_menu, "Quit " WP_APP_NAME, "terminate:", "", 0, 0);
    MSG(void, id)(G.status_item, S("setMenu:"), G.status_menu);
}

/* Favourite emoji: `defaults write com.fredbackman.watchparty favoriteEmoji "🍿 🤣 ❤️"` */
static int favorite_emoji(char out[12][16])
{
    id defaults = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    id saved = MSG(id, id)(defaults, S("stringForKey:"), nsstr("favoriteEmoji"));
    const char *list = (saved && cstr(saved)[0]) ? cstr(saved) : "😂 ❤️ 🔥 👏 😮 😢 👍 🎉";
    int n = 0;
    char buf[256];
    wp_strlcpy(buf, list, sizeof buf);
    for (char *tok = strtok(buf, " ,"); tok && n < 12; tok = strtok(NULL, " ,")) {
        if (strlen(tok) < 16) wp_strlcpy(out[n++], tok, 16);
    }
    return n;
}

static void build_window(void)
{
    /* size: 16:9, up to 1280 wide, fitting the screen */
    id screen = MSG(id)(C("NSScreen"), S("mainScreen"));
    CGRect vf = screen ? MSG_STRET(CGRect)(screen, S("visibleFrame")) : rect(0, 0, 1440, 900);
    double W = vf.size.width * 0.82;
    if (W > 1280) W = 1280;
    double H = W * 9.0 / 16.0;
    if (H > vf.size.height * 0.9) { H = vf.size.height * 0.9; W = H * 16.0 / 9.0; }
    CGRect frame = rect(vf.origin.x + (vf.size.width - W) / 2, vf.origin.y + (vf.size.height - H) / 2, W, H);

    unsigned long style = 8;                          /* borderless + resizable */
    G.window = MSG(id, CGRect, unsigned long, unsigned long, BOOL)(
        MSG(id)((id)objc_getClass("WPWindow"), S("alloc")), S("initWithContentRect:styleMask:backing:defer:"),
        frame, style, 2 /* NSBackingStoreBuffered */, NO);
    MSG(void, id)(G.window, S("setTitle:"), nsstr(WP_APP_NAME));
    MSG(void, BOOL)(G.window, S("setReleasedWhenClosed:"), NO);
    MSG(void, BOOL)(G.window, S("setOpaque:"), NO);
    MSG(void, id)(G.window, S("setBackgroundColor:"), MSG(id)(C("NSColor"), S("clearColor")));
    MSG(void, BOOL)(G.window, S("setHasShadow:"), YES);
    MSG(void, BOOL)(G.window, S("setMovableByWindowBackground:"), YES);
    MSG(void, BOOL)(G.window, S("setAcceptsMouseMovedEvents:"), YES);
    MSG(void, CGSize)(G.window, S("setContentAspectRatio:"), (CGSize){ 16, 9 });
    MSG(void, CGSize)(G.window, S("setMinSize:"), (CGSize){ 480, 270 });
    MSG(void, unsigned long)(G.window, S("setCollectionBehavior:"), 1UL << 7);   /* full-screen primary */
    MSG(void, long)(G.window, S("setLevel:"), G.floating ? 3 : 0);
    MSG(void, id)(G.window, S("setDelegate:"), G.ctl);

    id content = MSG(id)(G.window, S("contentView"));
    MSG(void, BOOL)(content, S("setWantsLayer:"), YES);
    id clayer = MSG(id)(content, S("layer"));
    MSG(void, double)(clayer, S("setCornerRadius:"), 12.0);
    MSG(void, BOOL)(clayer, S("setMasksToBounds:"), YES);
    set_layer_color(clayer, "setBackgroundColor:", 0, 0, 0, 1);

    /* mouse tracking over the whole window, even when it is not key */
    id ta = MSG(id, CGRect, unsigned long, id, id)(MSG(id)(C("NSTrackingArea"), S("alloc")),
                                                    S("initWithRect:options:owner:userInfo:"),
                                                    rect(0, 0, 0, 0), 0x01 | 0x02 | 0x80 | 0x200, G.ctl, NULL);
    MSG(void, id)(content, S("addTrackingArea:"), ta);
    release(ta);

    /* ---- the player fills the window ---- */
    id cfg = alloc_init("WKWebViewConfiguration");
    MSG(void, unsigned long)(cfg, S("setMediaTypesRequiringUserActionForPlayback:"), 0);
    /* Present as Safari: YouTube serves its full desktop player to a UA it recognizes. */
    MSG(void, id)(cfg, S("setApplicationNameForUserAgent:"), nsstr("Version/18.6 Safari/605.1.15"));
    id prefs = MSG(id)(cfg, S("preferences"));
    MSG(void, id, id)(prefs, S("setValue:forKey:"), MSG(id, BOOL)(C("NSNumber"), S("numberWithBool:"), YES), nsstr("developerExtrasEnabled"));
    id ucc = MSG(id)(cfg, S("userContentController"));
    MSG(void, id, id)(ucc, S("addScriptMessageHandler:name:"), G.ctl, nsstr("wp"));
    G.web = MSG(id, CGRect, id)(MSG(id)(C("WKWebView"), S("alloc")), S("initWithFrame:configuration:"), rect(0, 0, W, H), cfg);
    MSG(void, unsigned long)(G.web, S("setAutoresizingMask:"), MASK_WIDTH | MASK_HEIGHT);
    MSG(void, id)(content, S("addSubview:"), G.web);
    release(cfg);

    id bundle = MSG(id)(C("NSBundle"), S("mainBundle"));
    id path = MSG(id, id, id)(bundle, S("pathForResource:ofType:"), nsstr("player"), nsstr("html"));
    id html = NULL;
    if (path) html = MSG(id, id, unsigned long, void *)(C("NSString"), S("stringWithContentsOfFile:encoding:error:"), path, 4 /* UTF-8 */, NULL);
    if (!html) html = nsstr("<html><body style='background:#000;color:#aaa;font-family:sans-serif;padding:24px'>"
                            "player.html is missing from the app bundle — rebuild with make.</body></html>");
    id base = MSG(id, id)(C("NSURL"), S("URLWithString:"), nsstr("https://localhost/"));
    MSG(id, id, id)(G.web, S("loadHTMLString:baseURL:"), html, base);

    /* ---- top bar: link, room buttons, status (shown while the pointer is near it) ---- */
    CGRect bf = bar_frame();
    const double PW = bf.size.width;
    G.hud_top = retain(make_panel(bf, 0));
    const double row_y = TOP_H - 12 - 26, bw = 62, gap = 6, chat_w = 104;
    const double group_w = 4 * bw + 3 * gap + gap + chat_w;

    id close_img = responds(C("NSImage"), "imageWithSystemSymbolName:accessibilityDescription:")
        ? MSG(id, id, id)(C("NSImage"), S("imageWithSystemSymbolName:accessibilityDescription:"), nsstr("xmark.circle.fill"), nsstr("Close"))
        : NULL;
    if (close_img) {
        G.close_btn = retain(MSG(id, id, id, SEL)(C("NSButton"), S("buttonWithImage:target:action:"), close_img, G.ctl, S("closeWindow:")));
    } else {
        G.close_btn = retain(MSG(id, id, id, SEL)(C("NSButton"), S("buttonWithTitle:target:action:"), nsstr("✕"), G.ctl, S("closeWindow:")));
    }
    MSG(void, BOOL)(G.close_btn, S("setBordered:"), NO);
    MSG(void, CGRect)(G.close_btn, S("setFrame:"), rect(10, row_y + 1, 24, 24));
    MSG(void, id)(G.close_btn, S("setToolTip:"), nsstr("Close the window (⌘W) — WatchParty stays in the menu bar"));
    MSG(void, id)(G.hud_top, S("addSubview:"), G.close_btn);

    const double load_w = 56;
    const double load_x = PW - 12 - group_w - 8 - load_w;
    G.url_field = retain(make_field(rect(42, row_y, load_x - 8 - 42, 26), "Search YouTube or paste a link, then Return", NULL));
    MSG(void, unsigned long)(G.url_field, S("setAutoresizingMask:"), MASK_WIDTH);
    MSG(void, id)(G.url_field, S("setTarget:"), G.ctl);
    MSG(void, SEL)(G.url_field, S("setAction:"), S("loadURL:"));
    MSG(void, BOOL)(MSG(id)(G.url_field, S("cell")), S("setSendsActionOnEndEditing:"), NO);
    MSG(void, id)(G.hud_top, S("addSubview:"), G.url_field);

    G.load_btn = retain(make_button("Load", "loadURL:", rect(load_x, row_y - 1, load_w, 28), MASK_MINX));
    MSG(void, id)(G.hud_top, S("addSubview:"), G.load_btn);

    const char *titles[4] = { "Host…", "Rooms", "Invite", "Leave" };
    const char *sels[4]   = { "hostRoom:", "joinRoom:", "copyInvite:", "leaveRoom:" };
    for (int i = 0; i < 4; i++) {
        double x = PW - 12 - group_w + i * (bw + gap);
        MSG(void, id)(G.hud_top, S("addSubview:"), make_button(titles[i], sels[i], rect(x, row_y - 1, bw, 28), MASK_MINX));
    }
    G.chat_btn = retain(make_button("Chat: on", "chatCycle:", rect(PW - 12 - chat_w, row_y - 1, chat_w, 28), MASK_MINX));
    MSG(void, id)(G.chat_btn, S("setToolTip:"), nsstr("Chat column → subtitles → off (⇧⌘C)"));
    MSG(void, id)(G.hud_top, S("addSubview:"), G.chat_btn);

    G.status_label = retain(make_label("", rect(14, 10, PW - 28, 18), 12, 0));
    MSG(void, unsigned long)(G.status_label, S("setAutoresizingMask:"), MASK_WIDTH);
    MSG(void, id)(G.status_label, S("setTextColor:"), white(0.78));
    MSG(void, unsigned long)(G.status_label, S("setLineBreakMode:"), 4);   /* truncate tail */
    MSG(void, id)(G.hud_top, S("addSubview:"), G.status_label);
    MSG(void, double)(G.hud_top, S("setAlphaValue:"), 0.0);
    MSG(void, BOOL)(G.hud_top, S("setHidden:"), YES);
    MSG(void, id)(content, S("addSubview:"), G.hud_top);

    /* ---- chat column on the right ---- */
    const double ch = H - CHAT_BOTTOM - CHAT_TOP;
    G.chat_panel = retain(make_panel(rect(W - PAD - CHAT_W, CHAT_BOTTOM, CHAT_W, ch), MASK_MINX | MASK_HEIGHT));

    G.chat_header = retain(make_label("Chat", rect(12, ch - 12 - 18, CHAT_W - 24 - 44, 18), 12, 1));
    MSG(void, unsigned long)(G.chat_header, S("setAutoresizingMask:"), MASK_WIDTH | MASK_MINY);
    MSG(void, id)(G.chat_header, S("setTextColor:"), white(0.9));
    MSG(void, unsigned long)(G.chat_header, S("setLineBreakMode:"), 4);
    MSG(void, id)(G.chat_panel, S("addSubview:"), G.chat_header);

    id hide_btn = make_button("Hide", "chatSubtitles:", rect(CHAT_W - 12 - 46, ch - 12 - 22, 46, 24), MASK_MINX | MASK_MINY);
    MSG(void, BOOL)(hide_btn, S("setBordered:"), NO);
    MSG(void, id)(hide_btn, S("setToolTip:"), nsstr("Hide the chat column; messages then appear as subtitles"));
    MSG(void, id)(G.chat_panel, S("addSubview:"), hide_btn);

    const double input_h = 28, emoji_h = 26, emoji_y = 10 + input_h + 4;
    const double react_h = 22, react_y = emoji_y + emoji_h + 2, scroll_y = react_y + 2 * react_h + 6;
    G.chat_scroll = retain(MSG(id)(C("NSTextView"), S("scrollableTextView")));
    MSG(void, CGRect)(G.chat_scroll, S("setFrame:"), rect(6, scroll_y, CHAT_W - 12, ch - 12 - 22 - 4 - scroll_y));
    MSG(void, unsigned long)(G.chat_scroll, S("setAutoresizingMask:"), MASK_WIDTH | MASK_HEIGHT);
    MSG(void, unsigned long)(G.chat_scroll, S("setBorderType:"), 0);
    MSG(void, BOOL)(G.chat_scroll, S("setDrawsBackground:"), NO);
    G.chat_view = retain(MSG(id)(G.chat_scroll, S("documentView")));
    MSG(void, BOOL)(G.chat_view, S("setEditable:"), NO);
    MSG(void, BOOL)(G.chat_view, S("setDrawsBackground:"), NO);
    MSG(void, id)(G.chat_view, S("setTextColor:"), white(0.92));
    MSG(void, id)(G.chat_view, S("setFont:"), sysfont(13, 0));
    MSG(void, CGSize)(G.chat_view, S("setTextContainerInset:"), (CGSize){ 6, 4 });
    MSG(void, id)(G.chat_panel, S("addSubview:"), G.chat_scroll);

    char emoji[12][16];
    int ne = favorite_emoji(emoji);
    double ex = 8, ew = (CHAT_W - 16) / (ne > 0 ? ne : 1);
    if (ew > 36) ew = 36;
    for (int i = 0; i < ne; i++) {
        id b = make_button(emoji[i], "sendEmoji:", rect(ex + i * ew, emoji_y, ew, emoji_h), MASK_MAXY);
        MSG(void, BOOL)(b, S("setBordered:"), NO);
        MSG(void, id)(b, S("setFont:"), sysfont(17, 0));
        MSG(void, id)(b, S("setToolTip:"), nsstr("Send to the room"));
        MSG(void, id)(G.chat_panel, S("addSubview:"), b);
    }

    for (int i = 0; i < QUICK_REACTIONS && i < N_REACTIONS; i++) {           /* two rows of three */
        char title[64];
        if (REACTIONS[i].emoji[0]) snprintf(title, sizeof title, "%s %s", REACTIONS[i].emoji, REACTIONS[i].word);
        else snprintf(title, sizeof title, "%s", REACTIONS[i].word);
        double rw = (CHAT_W - 16) / 3.0;
        double rx = 8 + (i % 3) * rw, ry = react_y + (i < 3 ? react_h : 0);
        id b = make_button(title, "sendReaction:", rect(rx, ry, rw, react_h), MASK_MAXY);
        MSG(void, BOOL)(b, S("setBordered:"), NO);
        MSG(void, id)(b, S("setFont:"), sysfont(11, 1));
        if (REACTIONS[i].image) {
            id im = sticker_image(REACTIONS[i].image, 18);
            if (im) {
                MSG(void, id)(b, S("setImage:"), im);
                MSG(void, unsigned long)(b, S("setImagePosition:"), 2);      /* NSImageLeft */
                if (responds(b, "setImageHugsTitle:")) MSG(void, BOOL)(b, S("setImageHugsTitle:"), YES);
                release(im);
            }
        }
        MSG(void, long)(b, S("setTag:"), (long)i);
        MSG(void, id)(b, S("setToolTip:"), nsstr("Send an animated reaction (more under Chat ▸ Send Reaction)"));
        MSG(void, id)(G.chat_panel, S("addSubview:"), b);
    }

    G.chat_input = retain(make_field(rect(10, 10, CHAT_W - 20, input_h), "Say something… (Return)", NULL));
    MSG(void, unsigned long)(G.chat_input, S("setAutoresizingMask:"), MASK_WIDTH | MASK_MAXY);
    MSG(void, id)(G.chat_input, S("setTarget:"), G.ctl);
    MSG(void, SEL)(G.chat_input, S("setAction:"), S("chatEnter:"));
    MSG(void, BOOL)(MSG(id)(G.chat_input, S("cell")), S("setSendsActionOnEndEditing:"), NO);
    MSG(void, id)(G.chat_panel, S("addSubview:"), G.chat_input);
    MSG(void, id)(content, S("addSubview:"), G.chat_panel);

    /* ---- bottom message box (chat column hidden) ---- */
    CGRect mf = msgbar_frame();
    G.msg_bar = retain(make_panel(mf, 0));
    MSG(void, double)(MSG(id)(G.msg_bar, S("layer")), S("setCornerRadius:"), MSG_BAR_H / 2);
    G.msg_field = retain(make_field(rect(14, 8, mf.size.width - 28, 28), "Message the room… (Return sends, Esc hides)", NULL));
    MSG(void, unsigned long)(G.msg_field, S("setAutoresizingMask:"), MASK_WIDTH);
    MSG(void, BOOL)(G.msg_field, S("setBezeled:"), NO);
    MSG(void, BOOL)(G.msg_field, S("setDrawsBackground:"), NO);
    MSG(void, unsigned long)(G.msg_field, S("setFocusRingType:"), 1);     /* none */
    MSG(void, id)(G.msg_field, S("setFont:"), sysfont(15, 0));
    MSG(void, id)(G.msg_field, S("setTextColor:"), white(0.95));
    MSG(void, id)(G.msg_field, S("setTarget:"), G.ctl);
    MSG(void, SEL)(G.msg_field, S("setAction:"), S("msgEnter:"));
    MSG(void, BOOL)(MSG(id)(G.msg_field, S("cell")), S("setSendsActionOnEndEditing:"), NO);
    MSG(void, id)(G.msg_bar, S("addSubview:"), G.msg_field);
    MSG(void, double)(G.msg_bar, S("setAlphaValue:"), 0.0);
    MSG(void, BOOL)(G.msg_bar, S("setHidden:"), YES);
    MSG(void, id)(content, S("addSubview:"), G.msg_bar);

    /* ---- subtitle: one message at a time, top centre, never blocks clicks ---- */
    G.sub_box = retain(MSG(id, CGRect)(MSG(id)((id)objc_getClass("WPPassView"), S("alloc")), S("initWithFrame:"), rect(0, 0, 10, 10)));
    MSG(void, BOOL)(G.sub_box, S("setWantsLayer:"), YES);
    id slayer = MSG(id)(G.sub_box, S("layer"));
    MSG(void, double)(slayer, S("setCornerRadius:"), 10.0);
    MSG(void, BOOL)(slayer, S("setMasksToBounds:"), YES);
    set_layer_color(slayer, "setBackgroundColor:", 0, 0, 0, 0.62);
    G.sub_label = retain(MSG(id, id)(C("NSTextField"), S("wrappingLabelWithString:"), nsstr("")));
    MSG(void, id)(G.sub_label, S("setFont:"), chat_font(17, 1));
    MSG(void, id)(G.sub_label, S("setTextColor:"), chat_color());
    MSG(void, long)(G.sub_label, S("setAlignment:"), 2);            /* NSTextAlignmentCenter */
    MSG(void, BOOL)(G.sub_label, S("setSelectable:"), NO);
    MSG(void, id)(G.sub_box, S("addSubview:"), G.sub_label);
    MSG(void, double)(G.sub_box, S("setAlphaValue:"), 0.0);
    MSG(void, BOOL)(G.sub_box, S("setHidden:"), YES);
    MSG(void, id)(content, S("addSubview:"), G.sub_box);

    MSG(void, id)(G.url_field, S("setNextKeyView:"), G.chat_input);
    MSG(void, id)(G.chat_input, S("setNextKeyView:"), G.url_field);
    G.bar_visible = 0;
    G.last_mouse_move = wp_now_ms();
    if (G.chat_mode != CHAT_PANEL) {
        MSG(void, double)(G.chat_panel, S("setAlphaValue:"), 0.0);
        MSG(void, BOOL)(G.chat_panel, S("setHidden:"), YES);
    }
}

/* Esc hides the bar until the pointer leaves its zone; Return (outside a
 * text field) opens the chat box. */
static void install_event_monitor(void)
{
    unsigned long long mask = (1ULL << 1) | (1ULL << 10);
    id (^handler)(id) = ^id(id event) {
            if (MSG(id)(event, S("window")) != G.window) return event;
            unsigned long type = MSG(unsigned long)(event, S("type"));
            if (type == 10) {                                       /* key down */
                unsigned short key = MSG(unsigned short)(event, S("keyCode"));
                unsigned long mods = MSG(unsigned long)(event, S("modifierFlags")) & (MOD_COMMAND | MOD_CONTROL | MOD_OPTION);
                id fr = MSG(id)(G.window, S("firstResponder"));
                int in_field = fr && MSG(BOOL, id)(fr, S("isKindOfClass:"), C("NSTextView")) &&
                               MSG(id)(fr, S("delegate")) != NULL && MSG(id)(fr, S("delegate")) != G.web;
                DBG("key %u mods %lx in_field %d", key, mods, in_field);
                if (in_field && MSG(id)(fr, S("delegate")) == G.msg_field) G.msgbar_activity = wp_now_ms();
                if (key == 53 && !mods) {                           /* Esc */
                    if (G.results_visible) { results_hide(); return NULL; }
                    if (G.rooms_visible) { rooms_hide(); return NULL; }
                    if (G.msgbar_visible) { msgbar_set(0); return NULL; }
                    if (in_field) MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.web);
                    if (G.bar_visible) { bar_set(0); G.bar_suppressed = 1; }
                    return NULL;
                }
                if ((key == 36 || key == 76) && !mods && !in_field) {   /* Return: start typing a message */
                    if (G.chat_mode == CHAT_PANEL) MSG(BOOL, id)(G.window, S("makeFirstResponder:"), G.chat_input);
                    else msgbar_open();
                    return NULL;
                }
                return event;
            }
            G.last_mouse_move = wp_now_ms();
            return event;
        };
    MSG(id, unsigned long long, void *)(C("NSEvent"), S("addLocalMonitorForEventsMatchingMask:handler:"), mask, (void *)handler);
}

static void ctl_did_finish(id self, SEL cmd, id note)
{
    (void)self; (void)cmd; (void)note;
    id defaults = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    id has_pref = MSG(id, id)(defaults, S("objectForKey:"), nsstr("floatOnTop"));
    G.floating = has_pref ? MSG(BOOL, id)(defaults, S("boolForKey:"), nsstr("floatOnTop")) : 1;
    long cm = MSG(long, id)(defaults, S("integerForKey:"), nsstr("chatMode"));
    G.chat_mode = (cm >= CHAT_PANEL && cm <= CHAT_OFF) ? (int)cm : CHAT_PANEL;
    long fc = MSG(long, id)(defaults, S("integerForKey:"), nsstr("chatFont"));
    long cc = MSG(long, id)(defaults, S("integerForKey:"), nsstr("chatColor"));
    G.font_choice = (fc >= 0 && fc < N_FONTS) ? (int)fc : 0;
    G.color_choice = (cc >= 0 && cc < N_COLORS) ? (int)cc : 0;
    id fw = MSG(id, id)(defaults, S("objectForKey:"), nsstr("fancyWords"));
    RS()->fancy = fw ? MSG(BOOL, id)(defaults, S("boolForKey:"), nsstr("fancyWords")) : 1;   /* on unless turned off */

    build_menus();
    build_status_item();
    build_window();
    install_event_monitor();
    update_info();
    apply_text_style();
    log_line("Welcome to WatchParty. Host a room (⌘N), or see the rooms on your Wi-Fi (⌘J) and ask to join one.");
    log_line("The controls live at the top — move the pointer there. Search YouTube or paste a link. ⇧⌘C switches chat: column → subtitles → off.");
    show_window();
    attention_refresh();
    ui_rooms_changed();                               /* anything found while we were starting up */

    G.hud_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(G.hud_timer, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), 500 * NSEC_PER_MSEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(G.hud_timer, ^{ on_hud_timer(); });
    dispatch_resume(G.hud_timer);
}

static void make_classes(void)
{
    Class k = objc_allocateClassPair((Class)objc_getClass("NSObject"), "WPController", 0);
    class_addMethod(k, S("applicationDidFinishLaunching:"), (IMP)ctl_did_finish, "v@:@");
    class_addMethod(k, S("applicationShouldTerminateAfterLastWindowClosed:"), (IMP)ctl_no, ENC_BOOL "@:@");
    class_addMethod(k, S("applicationSupportsSecureRestorableState:"), (IMP)ctl_yes, ENC_BOOL "@:@");
    class_addMethod(k, S("applicationShouldHandleReopen:hasVisibleWindows:"), (IMP)ctl_reopen, ENC_BOOL "@:@" ENC_BOOL);
    class_addMethod(k, S("applicationWillTerminate:"), (IMP)ctl_will_terminate, "v@:@");
    class_addMethod(k, S("userContentController:didReceiveScriptMessage:"), (IMP)ctl_script_message, "v@:@@");
    class_addMethod(k, S("validateMenuItem:"), (IMP)ctl_validate_menu_item, ENC_BOOL "@:@");
    class_addMethod(k, S("windowDidResize:"), (IMP)ctl_window_resized, "v@:@");
    class_addMethod(k, S("windowDidEnterFullScreen:"), (IMP)ctl_window_resized, "v@:@");
    class_addMethod(k, S("windowDidExitFullScreen:"), (IMP)ctl_window_resized, "v@:@");
    class_addMethod(k, S("mouseMoved:"), (IMP)ctl_mouse_moved, "v@:@");
    class_addMethod(k, S("mouseEntered:"), (IMP)ctl_mouse_moved, "v@:@");
    class_addMethod(k, S("mouseExited:"), (IMP)ctl_mouse_exited, "v@:@");
    class_addMethod(k, S("loadURL:"), (IMP)ctl_load_url, "v@:@");
    class_addMethod(k, S("hostRoom:"), (IMP)ctl_host_room, "v@:@");
    class_addMethod(k, S("joinRoom:"), (IMP)ctl_join_room, "v@:@");
    class_addMethod(k, S("copyInvite:"), (IMP)ctl_copy_invite, "v@:@");
    class_addMethod(k, S("leaveRoom:"), (IMP)ctl_leave_room, "v@:@");
    class_addMethod(k, S("playPause:"), (IMP)ctl_play_pause, "v@:@");
    class_addMethod(k, S("syncNow:"), (IMP)ctl_sync_now, "v@:@");
    class_addMethod(k, S("showWindow:"), (IMP)ctl_show_window, "v@:@");
    class_addMethod(k, S("closeWindow:"), (IMP)ctl_close_window, "v@:@");
    class_addMethod(k, S("toggleHUD:"), (IMP)ctl_toggle_hud, "v@:@");
    class_addMethod(k, S("toggleFloat:"), (IMP)ctl_toggle_float, "v@:@");
    class_addMethod(k, S("chatPanel:"), (IMP)ctl_chat_panel, "v@:@");
    class_addMethod(k, S("chatSubtitles:"), (IMP)ctl_chat_subtitles, "v@:@");
    class_addMethod(k, S("chatOff:"), (IMP)ctl_chat_off, "v@:@");
    class_addMethod(k, S("chatCycle:"), (IMP)ctl_chat_cycle, "v@:@");
    class_addMethod(k, S("sendEmoji:"), (IMP)ctl_send_emoji, "v@:@");
    class_addMethod(k, S("sendReaction:"), (IMP)ctl_send_reaction, "v@:@");
    class_addMethod(k, S("pickResult:"), (IMP)ctl_pick_result, "v@:@");
    class_addMethod(k, S("chatFont:"), (IMP)ctl_chat_font, "v@:@");
    class_addMethod(k, S("chatColor:"), (IMP)ctl_chat_color, "v@:@");
    class_addMethod(k, S("chatEnter:"), (IMP)ctl_chat_enter, "v@:@");
    class_addMethod(k, S("msgEnter:"), (IMP)ctl_msg_enter, "v@:@");
    class_addMethod(k, S("toggleFancy:"), (IMP)ctl_toggle_fancy, "v@:@");
    class_addMethod(k, S("joinCode:"), (IMP)ctl_join_code, "v@:@");
    class_addMethod(k, S("askJoin:"), (IMP)ctl_ask_join, "v@:@");
    class_addMethod(k, S("hideRooms:"), (IMP)ctl_hide_rooms, "v@:@");
    class_addMethod(k, S("askCancel:"), (IMP)ctl_ask_cancel, "v@:@");
    class_addMethod(k, S("knockAllow:"), (IMP)ctl_knock_allow, "v@:@");
    class_addMethod(k, S("knockDeny:"), (IMP)ctl_knock_deny, "v@:@");
    class_addMethod(k, S("cardDismiss:"), (IMP)ctl_card_dismiss, "v@:@");
    class_addMethod(k, S("cardAsk:"), (IMP)ctl_card_ask, "v@:@");
    class_addMethod(k, S("cardCode:"), (IMP)ctl_card_code, "v@:@");
    class_addMethod(k, S("menuNeedsUpdate:"), (IMP)ctl_menu_needs_update, "v@:@");
    objc_registerClassPair(k);

    /* a borderless window only takes keyboard focus if it says it can */
    Class w = objc_allocateClassPair((Class)objc_getClass("NSWindow"), "WPWindow", 0);
    class_addMethod(w, S("canBecomeKeyWindow"), (IMP)ctl_yes0, ENC_BOOL "@:");
    class_addMethod(w, S("canBecomeMainWindow"), (IMP)ctl_yes0, ENC_BOOL "@:");
    objc_registerClassPair(w);

    /* overlay panel: dragging its background moves the window */
    Class h = objc_allocateClassPair((Class)objc_getClass("NSView"), "WPHUDView", 0);
    class_addMethod(h, S("mouseDownCanMoveWindow"), (IMP)ctl_yes0, ENC_BOOL "@:");
    class_addMethod(h, S("acceptsFirstMouse:"), (IMP)ctl_yes, ENC_BOOL "@:@");
    objc_registerClassPair(h);

    /* subtitle box: transparent to the mouse */
    Class p = objc_allocateClassPair((Class)objc_getClass("NSView"), "WPPassView", 0);
    class_addMethod(p, S("hitTest:"), (IMP)ctl_nil1, "@@:{CGPoint=dd}");
    objc_registerClassPair(p);
}

/* ---------------------------------------------------------------- main */

int wp_app_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    void *pool = objc_autoreleasePoolPush();

    make_classes();
    id app = MSG(id)(C("NSApplication"), S("sharedApplication"));
    MSG(BOOL, long)(app, S("setActivationPolicy:"), 0);           /* regular app: Dock icon + menu bar */
    G.ctl = alloc_init("WPController");
    MSG(void, id)(app, S("setDelegate:"), G.ctl);

    wp_room_ui ui = {
        .say = ui_say,
        .note = ui_note,
        .player = ui_player,
        .video_loaded = ui_video_loaded,
        .changed = ui_changed,
        .sticker = ui_sticker,
        .beep = ui_beep,
        .rooms_changed = ui_rooms_changed,
        .knock = ui_knock,
        .knock_gone = ui_knock_gone,
    };
    wp_room_init(&ui, default_name());
    G.floating = 1;

    MSG(void)(app, S("run"));
    objc_autoreleasePoolPop(pool);
    return 0;
}
