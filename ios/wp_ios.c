/* wp_ios.c — WatchParty for iPhone and iPad, written in C (UIKit through the
 * Objective-C runtime), on the same room engine as the Mac app (wp_room.c).
 *
 *   portrait                          landscape
 *   ┌──────────────────────────┐      ┌────────────────────────────────────┐
 *   │ WatchParty     [👥] [⋯]  │      │      ╭ Bob: that part!! ╮          │
 *   │ ● In “Movie night” · 3   │      │                                    │
 *   │ [ Search YouTube…      ] │      │          the video, edge to        │
 *   │ ┌──────────────────────┐ │      │          edge, subtitles on top    │
 *   │ │        video         │ │      │                                    │
 *   │ └──────────────────────┘ │      └────────────────────────────────────┘
 *   │ ╭ Bob wants to join  ──╮ │
 *   │ ╰ [Not now] [Let in]  ─╯ │
 *   │ chat …                   │
 *   │ 😂 ❤️ 🔥 🗿 sigma  ate … │
 *   │ [ Say something…  ] Send │
 *   └──────────────────────────┘ */
#include "objc_glue.h"
#include "wp_net.h"
#include "wp_room.h"
#include "wp_search.h"
#include "wp_util.h"

#include <dispatch/dispatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int UIApplicationMain(int argc, char **argv, id principal, id delegate);
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);
extern id const UIKeyboardWillChangeFrameNotification;
extern id const UIKeyboardFrameEndUserInfoKey;
extern id const UIKeyboardAnimationDurationUserInfoKey;

typedef struct { double top, left, bottom, right; } WPInsets;      /* UIEdgeInsets */
typedef struct { double a, b, c, d, tx, ty; } WPXform;
typedef struct { char s[64]; } name64;                              /* a room name blocks can capture */              /* CGAffineTransform */
static WPXform xf_scale(double k) { WPXform t = { k, 0, 0, k, 0, 0 }; return t; }
static WPXform xf_move(double x, double y) { WPXform t = { 1, 0, 0, 1, x, y }; return t; }

#define TOUCH_UP   (1UL << 6)            /* UIControlEventTouchUpInside */
#define LOG_CAP    (64 * 1024)
#define MAX_CARDS  6
#define TOAST_MS   12000

enum { CARD_NONE, CARD_KNOCK, CARD_ASK, CARD_ROOM };
typedef struct { int kind, key, gen; char room[64]; id view; double h; } card_t;

static struct {
    id window, vc, view, web, ctl;
    id title_lbl, status_lbl, rooms_btn, menu_btn;
    id search;
    id chat, react_bar, input_bar, input, send_btn;
    id results, sub_box, sub_lbl;
    id rooms_vc, rooms_list;
    int rooms_built;
    card_t cards[MAX_CARDS];
    int card_gen;
    char seen[WP_MAX_ROOMS][64];
    int n_seen;

    char *log;
    size_t log_len, log_cap;

    double kb_h;
    int landscape, subs_on;
    int sub_gen;

    wp_result res[WP_MAX_RESULTS];
    id res_thumbs[WP_MAX_RESULTS];
    int nres, res_gen, res_visible;
    char last_query[256];
} G;

static wp_room_state *RS(void) { return wp_room(); }
static void layout(void);
static void rebuild_menu(void);
static void rooms_list_build(void);
static void show_rooms(void);
static void present(id vc);

/* ------------------------------------------------------------ helpers */

static id color(double r, double g, double b, double a)
{
    return MSG(id, double, double, double, double)(C("UIColor"), S("colorWithRed:green:blue:alpha:"), r, g, b, a);
}
static id white(double a) { return MSG(id, double, double)(C("UIColor"), S("colorWithWhite:alpha:"), 1.0, a); }
static id accent(void) { return color(0.52, 0.32, 0.93, 1); }          /* the icon's violet */
static id font(double size, int bold)
{
    return bold ? MSG(id, double)(C("UIFont"), S("boldSystemFontOfSize:"), size)
                : MSG(id, double)(C("UIFont"), S("systemFontOfSize:"), size);
}
static id new_view(CGRect r, id cls_name_unused)
{
    (void)cls_name_unused;
    return MSG(id, CGRect)(MSG(id)(C("UIView"), S("alloc")), S("initWithFrame:"), r);
}
static void set_frame(id v, CGRect r) { if (v) MSG(void, CGRect)(v, S("setFrame:"), r); }
static void set_hidden(id v, int h) { if (v) MSG(void, BOOL)(v, S("setHidden:"), h ? YES : NO); }
static void round_corners(id v, double r)
{
    id layer = MSG(id)(v, S("layer"));
    MSG(void, double)(layer, S("setCornerRadius:"), r);
    MSG(void, BOOL)(layer, S("setMasksToBounds:"), YES);
}
static CGRect bounds_of(id v) { return MSG_STRET(CGRect)(v, S("bounds")); }

static id label(const char *text, double size, int bold, id tint)
{
    id l = MSG(id, CGRect)(MSG(id)(C("UILabel"), S("alloc")), S("initWithFrame:"), rect(0, 0, 10, 10));
    MSG(void, id)(l, S("setText:"), nsstr(text));
    MSG(void, id)(l, S("setFont:"), font(size, bold));
    MSG(void, id)(l, S("setTextColor:"), tint ? tint : white(0.95));
    return l;                                              /* +1 */
}

static CGSize fit(id v, double w) { return MSG(CGSize, CGSize)(v, S("sizeThatFits:"), (CGSize){ w, 2000 }); }

static id button(const char *title, const char *sel, int filled)
{
    id b = MSG(id, long)(C("UIButton"), S("buttonWithType:"), 1L);        /* system */
    MSG(void, id, unsigned long)(b, S("setTitle:forState:"), nsstr(title), 0UL);
    MSG(void, id)(MSG(id)(b, S("titleLabel")), S("setFont:"), font(15, filled));
    if (filled) {
        MSG(void, id)(b, S("setBackgroundColor:"), accent());
        MSG(void, id, unsigned long)(b, S("setTitleColor:forState:"), white(1), 0UL);
        round_corners(b, 10);
    } else {
        MSG(void, id)(b, S("setTintColor:"), white(0.9));
        MSG(void, id)(b, S("setBackgroundColor:"), white(0.12));
        round_corners(b, 10);
    }
    if (sel) MSG(void, id, SEL, unsigned long)(b, S("addTarget:action:forControlEvents:"), G.ctl, S(sel), TOUCH_UP);
    return b;                                              /* autoreleased */
}

static id saved_string(const char *key)
{
    id d = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    return MSG(id, id)(d, S("stringForKey:"), nsstr(key));
}
static void save_string(const char *key, const char *val)
{
    id d = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    MSG(void, id, id)(d, S("setObject:forKey:"), nsstr(val), nsstr(key));
}
static void save_bool(const char *key, int v)
{
    id d = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    MSG(void, BOOL, id)(d, S("setBool:forKey:"), v ? YES : NO, nsstr(key));
}
static int saved_bool(const char *key, int dflt)
{
    id d = MSG(id)(C("NSUserDefaults"), S("standardUserDefaults"));
    if (!MSG(id, id)(d, S("objectForKey:"), nsstr(key))) return dflt;
    return MSG(BOOL, id)(d, S("boolForKey:"), nsstr(key));
}

static const char *my_name(void)
{
    static char name[WP_NAME_MAX + 1];
    id s = saved_string("displayName");
    if (!s || !cstr(s)[0]) return "";
    wp_sanitize_name(name, sizeof name, cstr(s));
    return name;
}

static void haptic(long kind)            /* 0 success, 1 warning, 2 error */
{
    id g = alloc_init("UINotificationFeedbackGenerator");
    MSG(void, long)(g, S("notificationOccurred:"), kind);
    release(g);
}

/* ------------------------------------------------------------ chat log */

static void log_line(const char *text)
{
    size_t need = strlen(text) + 2;
    if (!G.log) { G.log_cap = 8192; G.log = (char *)malloc(G.log_cap); G.log[0] = 0; G.log_len = 0; }
    if (G.log_len + need >= LOG_CAP) {
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
    if (G.chat) {
        MSG(void, id)(G.chat, S("setText:"), nsstr(G.log));
        unsigned long len = MSG(unsigned long)(MSG(id)(G.chat, S("text")), S("length"));
        WPRange r = { len ? len - 1 : 0, 1 };
        MSG(void, WPRange)(G.chat, S("scrollRangeToVisible:"), r);
    }
}

/* landscape: one line at a time over the top of the picture */
static void subtitle(const char *text)
{
    if (!G.sub_box || !G.landscape || !G.subs_on) return;
    MSG(void, id)(G.sub_lbl, S("setText:"), nsstr(text));
    layout();
    set_hidden(G.sub_box, 0);
    MSG(void, double)(G.sub_box, S("setAlpha:"), 1.0);
    int gen = ++G.sub_gen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (gen != G.sub_gen) return;
        void (^fade)(void) = ^{ MSG(void, double)(G.sub_box, S("setAlpha:"), 0.0); };
        MSG(void, double, void *)(C("UIView"), S("animateWithDuration:animations:"), 0.5, (void *)fade);
    });
}

static void say(const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line(buf);
    char *later = strdup(buf);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        subtitle(later);
        free(later);
    });
}

/* ---------------------------------------------------------- stickers */

static void show_sticker(const char *text, const char *by, const char *image)
{
    if (!G.web) return;
    CGRect vf = MSG_STRET(CGRect)(G.web, S("frame"));
    id box = new_view(rect(0, 0, 10, 10), NULL);
    MSG(void, id)(box, S("setBackgroundColor:"), color(0, 0, 0, 0.6));
    MSG(void, BOOL)(box, S("setUserInteractionEnabled:"), NO);
    round_corners(box, 16);
    id big = label(text, 24, 1, white(1));
    CGSize bs = fit(big, 400);
    id who = label(by ? by : "", 11, 0, white(0.75));
    CGSize ws = fit(who, 400);
    id pic = NULL;
    if (image) {
        char name[40];
        snprintf(name, sizeof name, "%s", image);
        id img = MSG(id, id)(C("UIImage"), S("imageNamed:"), nsstr(name));
        if (img) {
            pic = MSG(id, id)(MSG(id)(C("UIImageView"), S("alloc")), S("initWithImage:"), img);
            MSG(void, long)(pic, S("setContentMode:"), 1L);         /* aspect fit */
        }
    }
    double pw = pic ? 58 : 0;
    double row_w = bs.width + pw, row_h = pic && bs.height < 50 ? 50 : bs.height;
    double w = (row_w > ws.width ? row_w : ws.width) + 30, h = row_h + ws.height + 18;
    double x0 = (w - row_w) / 2;
    if (pic) { set_frame(pic, rect(x0, 8, 50, 50)); MSG(void, id)(box, S("addSubview:"), pic); release(pic); }
    set_frame(big, rect(x0 + pw, 8 + (row_h - bs.height) / 2, bs.width, bs.height));
    set_frame(who, rect((w - ws.width) / 2, 10 + row_h, ws.width, ws.height));
    MSG(void, id)(box, S("addSubview:"), big);
    MSG(void, id)(box, S("addSubview:"), who);
    release(big); release(who);

    static int slot;
    double cx = vf.origin.x + vf.size.width * (0.3 + 0.2 * (slot++ % 3));
    double cy = vf.origin.y + vf.size.height * 0.62;
    set_frame(box, rect(cx - w / 2, cy - h / 2, w, h));
    MSG(void, double)(box, S("setAlpha:"), 0.0);
    MSG(void, WPXform)(box, S("setTransform:"), xf_scale(0.6));
    MSG(void, id)(G.view, S("addSubview:"), box);
    void (^pop)(void) = ^{
        MSG(void, double)(box, S("setAlpha:"), 1.0);
        MSG(void, WPXform)(box, S("setTransform:"), xf_scale(1));
    };
    void (^away)(void) = ^{
        MSG(void, double)(box, S("setAlpha:"), 0.0);
        MSG(void, WPXform)(box, S("setTransform:"), xf_move(0, -70));
    };
    void (^gone)(BOOL) = ^(BOOL fin) { (void)fin; MSG(void)(box, S("removeFromSuperview")); release(box); };
    void (^then)(BOOL) = ^(BOOL fin) {
        (void)fin;
        MSG(void, double, double, unsigned long, void *, void *)(C("UIView"), S("animateWithDuration:delay:options:animations:completion:"),
                                                                 0.7, 1.4, 0UL, (void *)away, (void *)gone);
    };
    MSG(void, double, double, double, double, unsigned long, void *, void *)(C("UIView"),
        S("animateWithDuration:delay:usingSpringWithDamping:initialSpringVelocity:options:animations:completion:"),
        0.45, 0.0, 0.55, 0.8, 0UL, (void *)pop, (void *)then);
}

/* -------------------------------------------------------------- cards */

static void card_remove(int i)
{
    if (i < 0 || i >= MAX_CARDS || G.cards[i].kind == CARD_NONE) return;
    id v = G.cards[i].view;
    memset(&G.cards[i], 0, sizeof G.cards[i]);
    if (v) { MSG(void)(v, S("removeFromSuperview")); release(v); }
    void (^anim)(void) = ^{ layout(); };
    MSG(void, double, void *)(C("UIView"), S("animateWithDuration:animations:"), 0.25, (void *)anim);
}

static int card_find(int kind, int key)
{
    for (int i = 0; i < MAX_CARDS; i++) if (G.cards[i].kind == kind && (kind != CARD_KNOCK || G.cards[i].key == key)) return i;
    return -1;
}

static int card_slot(void)
{
    for (int i = 0; i < MAX_CARDS; i++) if (G.cards[i].kind == CARD_NONE) return i;
    for (int i = 0; i < MAX_CARDS; i++) if (G.cards[i].kind == CARD_ROOM) { card_remove(i); return i; }
    return -1;
}

static void card_build(int i, int kind, const char *title, const char *body,
                       const char *b1, const char *sel1, const char *b2, const char *sel2)
{
    card_t *c = &G.cards[i];
    if (c->view) { MSG(void)(c->view, S("removeFromSuperview")); release(c->view); c->view = NULL; }
    c->kind = kind;
    if (!c->gen) c->gen = ++G.card_gen;
    id v = new_view(rect(0, 0, 300, 100), NULL);
    MSG(void, id)(v, S("setBackgroundColor:"), color(0.13, 0.13, 0.16, 0.96));
    round_corners(v, 14);
    id t = label(title, 15, 1, white(0.97));
    MSG(void, long)(t, S("setNumberOfLines:"), 0L);
    MSG(void, long)(t, S("setTag:"), 1L);
    MSG(void, id)(v, S("addSubview:"), t); release(t);
    id d = label(body ? body : "", 13, 0, white(0.75));
    MSG(void, long)(d, S("setNumberOfLines:"), 0L);
    MSG(void, long)(d, S("setTag:"), 2L);
    MSG(void, id)(v, S("addSubview:"), d); release(d);
    if (b1) { id b = button(b1, sel1, 0); MSG(void, long)(b, S("setTag:"), 100L + i); MSG(void, id)(v, S("addSubview:"), b); }
    if (b2) { id b = button(b2, sel2, 1); MSG(void, long)(b, S("setTag:"), 200L + i); MSG(void, id)(v, S("addSubview:"), b); }
    MSG(void, id)(G.view, S("addSubview:"), v);
    c->view = v;
    void (^anim)(void) = ^{ layout(); };
    MSG(void, double, void *)(C("UIView"), S("animateWithDuration:animations:"), 0.25, (void *)anim);
}

/* lay one card out at width w; returns its height */
static double card_layout(card_t *c, double x, double y, double w)
{
    id t = MSG(id, long)(c->view, S("viewWithTag:"), 1L);
    id d = MSG(id, long)(c->view, S("viewWithTag:"), 2L);
    int idx = (int)(c - G.cards);
    id b1 = MSG(id, long)(c->view, S("viewWithTag:"), 100L + idx);
    id b2 = MSG(id, long)(c->view, S("viewWithTag:"), 200L + idx);
    double inner = w - 28;
    CGSize ts = fit(t, inner), ds = fit(d, inner);
    double yy = 12;
    set_frame(t, rect(14, yy, inner, ts.height)); yy += ts.height + 3;
    set_frame(d, rect(14, yy, inner, ds.height)); yy += ds.height + 10;
    if (b1 || b2) {
        double bw = (inner - 8) / 2;
        if (b1 && b2) { set_frame(b1, rect(14, yy, bw, 38)); set_frame(b2, rect(14 + bw + 8, yy, bw, 38)); }
        else set_frame(b1 ? b1 : b2, rect(14, yy, inner, 38));
        yy += 38 + 12;
    }
    set_frame(c->view, rect(x, y, w, yy));
    c->h = yy;
    return yy;
}

/* -------------------------------------------------------------- layout */

static void layout(void)
{
    if (!G.view) return;
    CGRect b = bounds_of(G.view);
    WPInsets s = MSG_STRET(WPInsets)(G.view, S("safeAreaInsets"));
    double W = b.size.width, H = b.size.height;
    int land = W > H;
    if (land != G.landscape) {
        G.landscape = land;
        MSG(void)(G.vc, S("setNeedsStatusBarAppearanceUpdate"));
        if (responds(G.vc, "setNeedsUpdateOfHomeIndicatorAutoHidden")) MSG(void)(G.vc, S("setNeedsUpdateOfHomeIndicatorAutoHidden"));
        if (land) MSG(void, BOOL)(G.view, S("endEditing:"), YES);
    }
    id portrait_only[] = { G.title_lbl, G.status_lbl, G.rooms_btn, G.menu_btn, G.search, G.chat, G.react_bar, G.input_bar };
    for (size_t i = 0; i < sizeof portrait_only / sizeof portrait_only[0]; i++) set_hidden(portrait_only[i], land);
    set_hidden(G.results, land || !G.res_visible);

    /* cards in the order they arrived */
    int order[MAX_CARDS], n = 0;
    for (int k = 0; k < MAX_CARDS; k++) if (G.cards[k].kind != CARD_NONE) order[n++] = k;
    for (int a = 0; a < n; a++) for (int z = a + 1; z < n; z++)
        if (G.cards[order[z]].gen < G.cards[order[a]].gen) { int t = order[a]; order[a] = order[z]; order[z] = t; }

    if (land) {
        double vw = W, vh = W * 9 / 16;
        if (vh > H) { vh = H; vw = H * 16 / 9; }
        set_frame(G.web, rect((W - vw) / 2, (H - vh) / 2, vw, vh));
        double y = s.top + 10;
        for (int k = 0; k < n; k++) y += card_layout(&G.cards[order[k]], s.left + 12, y, 320) + 8;
        if (G.sub_box) {
            CGSize ss = fit(G.sub_lbl, W * 0.7);
            double bw = ss.width + 28, bh = ss.height + 14;
            set_frame(G.sub_box, rect((W - bw) / 2, s.top + 12, bw, bh));
            set_frame(G.sub_lbl, rect(14, 7, ss.width, ss.height));
        }
        return;
    }
    set_hidden(G.sub_box, 1);

    double x = s.left + 12, w = W - s.left - s.right - 24;
    double y = s.top + 6;
    set_frame(G.title_lbl, rect(x, y, w - 100, 24));
    set_frame(G.rooms_btn, rect(x + w - 92, y + 2, 44, 36));
    set_frame(G.menu_btn, rect(x + w - 44, y + 2, 44, 36));
    set_frame(G.status_lbl, rect(x, y + 25, w - 100, 18));
    y += 48;
    set_frame(G.search, rect(x, y, w, 36));
    y += 44;
    double vw = W - s.left - s.right, vh = vw * 9 / 16;
    set_frame(G.web, rect(s.left, y, vw, vh));
    y += vh + 8;

    double bottom = H - (G.kb_h > s.bottom ? G.kb_h : s.bottom);
    double input_y = bottom - 50;
    set_frame(G.input_bar, rect(s.left, input_y, vw, 50));
    CGRect ib = bounds_of(G.input_bar);
    set_frame(G.input, rect(12, 7, ib.size.width - 24 - 70, 36));
    set_frame(G.send_btn, rect(ib.size.width - 12 - 62, 7, 62, 36));
    double react_y = input_y - 44;
    set_frame(G.react_bar, rect(s.left, react_y, vw, 44));

    for (int k = 0; k < n; k++) y += card_layout(&G.cards[order[k]], x, y, w) + 8;
    double chat_h = react_y - y - 4;
    set_frame(G.chat, rect(s.left + 4, y, vw - 8, chat_h > 40 ? chat_h : 40));
    if (G.results) {
        double top = MSG_STRET(CGRect)(G.web, S("frame")).origin.y + vh + 4;
        if (G.kb_h > 0) top = MSG_STRET(CGRect)(G.search, S("frame")).origin.y + 44;   /* typing: list right under the field */
        set_frame(G.results, rect(s.left, top, vw, bottom - top));
    }
}

/* --------------------------------------------------- status / menus */

static void update_info(void)
{
    const wp_room_state *r = RS();
    char st[300];
    int n = wp_room_watching(), nearby = wp_room_nearby_count();
    if (r->role == WP_ROLE_HOST) {
        int asking = wp_room_pending_knocks();
        snprintf(st, sizeof st, "● Hosting “%s” · %d watching%s", r->room, n, asking ? " · someone's at the door" : "");
    } else if (r->role == WP_ROLE_CLIENT) {
        if (r->connecting) snprintf(st, sizeof st, "◌ Connecting…");
        else snprintf(st, sizeof st, "● In “%s” · %d watching", r->room, n);
    } else if (r->ask != WP_ASK_NONE) snprintf(st, sizeof st, "◌ Asking “%s”…", r->ask_room);
    else if (nearby == 1) snprintf(st, sizeof st, "○ “%s” is on your Wi-Fi — tap 👥", wp_room_nearby_name(0));
    else if (nearby > 1) snprintf(st, sizeof st, "○ %d rooms on your Wi-Fi — tap 👥", nearby);
    else snprintf(st, sizeof st, "○ Not in a room — host one from ⋯");
    MSG(void, id)(G.status_lbl, S("setText:"), nsstr(st));
    MSG(void, BOOL)(MSG(id)(C("UIApplication"), S("sharedApplication")), S("setIdleTimerDisabled:"),
                    (r->role != WP_ROLE_NONE && r->video[0]) ? YES : NO);     /* no auto-lock mid-movie */
    rebuild_menu();
}

static id action(const char *title, const char *symbol, void (^handler)(id), int destructive, int disabled, int on)
{
    id img = symbol ? MSG(id, id)(C("UIImage"), S("systemImageNamed:"), nsstr(symbol)) : NULL;
    id a = MSG(id, id, id, id, void *)(C("UIAction"), S("actionWithTitle:image:identifier:handler:"), nsstr(title), img, NULL, (void *)handler);
    unsigned long attrs = (destructive ? 2UL : 0) | (disabled ? 1UL : 0);
    MSG(void, unsigned long)(a, S("setAttributes:"), attrs);
    if (on) MSG(void, long)(a, S("setState:"), 1L);
    return a;
}

static id array_of(id *items, int n)
{
    return MSG(id, id *, unsigned long)(C("NSArray"), S("arrayWithObjects:count:"), items, (unsigned long)n);
}

static void ask_name_then(const char *why, void (^then)(const char *name));
static void run_host(void);
static void run_join_code(int nearby_index);
static void share_invite(void);

static void rebuild_menu(void)
{
    if (!G.menu_btn) return;
    const wp_room_state *r = RS();
    int in_room = r->role != WP_ROLE_NONE;
    id items[16];
    int n = 0;
    items[n++] = action("Host a Room…", "plus.circle", ^(id a) { (void)a; run_host(); }, 0, 0, 0);
    items[n++] = action("Rooms on This Wi-Fi…", "person.2.wave.2", ^(id a) { (void)a; show_rooms(); }, 0, 0, 0);
    items[n++] = action("Join with Invite Code…", "key", ^(id a) { (void)a; run_join_code(-1); }, 0, 0, 0);
    items[n++] = action("Share Invite…", "square.and.arrow.up", ^(id a) { (void)a; share_invite(); }, 0, r->role != WP_ROLE_HOST, 0);
    items[n++] = action("Sync Now", "arrow.triangle.2.circlepath", ^(id a) { (void)a; wp_room_sync_now(); }, 0, !in_room, 0);
    items[n++] = action("Fancy Words for Cuss Words", "textformat.abc", ^(id a) {
        (void)a;
        RS()->fancy = !RS()->fancy;
        save_bool("fancyWords", RS()->fancy);
        log_line(RS()->fancy ? "Fancy words on: cuss words now appear as their most magniloquent equivalents."
                             : "Fancy words off: messages show exactly as typed.");
        rebuild_menu();
    }, 0, 0, r->fancy);
    items[n++] = action("Subtitles in Landscape", "captions.bubble", ^(id a) {
        (void)a; G.subs_on = !G.subs_on; save_bool("subtitles", G.subs_on); rebuild_menu();
    }, 0, 0, G.subs_on);
    items[n++] = action("Change Your Name…", "person.crop.circle", ^(id a) {
        (void)a; ask_name_then("Your name", ^(const char *nm) { char m[80]; snprintf(m, sizeof m, "You're “%s” now.", nm); log_line(m); });
    }, 0, 0, 0);
    if (r->ask != WP_ASK_NONE) items[n++] = action("Stop Asking to Join", "xmark.circle", ^(id a) { (void)a; wp_room_ask_cancel(); }, 1, 0, 0);
    if (in_room) items[n++] = action(r->role == WP_ROLE_HOST ? "Close Room" : "Leave Room", "rectangle.portrait.and.arrow.right",
                                     ^(id a) { (void)a; wp_room_leave("— Left the room —"); }, 1, 0, 0);
    id menu = MSG(id, id, id)(C("UIMenu"), S("menuWithTitle:children:"), nsstr(""), array_of(items, n));
    MSG(void, id)(G.menu_btn, S("setMenu:"), menu);
}

/* ---------------------------------------------------- presenting */

static id top_vc(void)
{
    id vc = G.vc;
    for (;;) {
        id p = MSG(id)(vc, S("presentedViewController"));
        if (!p || MSG(BOOL)(p, S("isBeingDismissed"))) return vc;
        vc = p;
    }
}

static void present(id vc)
{
    id top = top_vc();
    id pop = MSG(id)(vc, S("popoverPresentationController"));
    if (pop) {                                   /* iPad: anchor share sheets to the ⋯ button */
        MSG(void, id)(pop, S("setSourceView:"), G.menu_btn);
        MSG(void, CGRect)(pop, S("setSourceRect:"), bounds_of(G.menu_btn));
    }
    MSG(void, id, BOOL, void *)(top, S("presentViewController:animated:completion:"), vc, YES, NULL);
}

static id alert(const char *title, const char *msg)
{
    return MSG(id, id, id, long)(C("UIAlertController"), S("alertControllerWithTitle:message:preferredStyle:"),
                                 nsstr(title), msg ? nsstr(msg) : NULL, 1L);
}

static void alert_action(id a, const char *title, long style, void (^handler)(id))
{
    id act = MSG(id, id, long, void *)(C("UIAlertAction"), S("actionWithTitle:style:handler:"), nsstr(title), style, (void *)handler);
    MSG(void, id)(a, S("addAction:"), act);
    if (style == 0) MSG(void, id)(a, S("setPreferredAction:"), act);
}

static void alert_field(id a, const char *placeholder, const char *value, int caps)
{
    void (^cfg)(id) = ^(id tf) {
        MSG(void, id)(tf, S("setPlaceholder:"), nsstr(placeholder));
        if (value) MSG(void, id)(tf, S("setText:"), nsstr(value));
        MSG(void, long)(tf, S("setAutocapitalizationType:"), caps ? 1L : 0L);
        MSG(void, long)(tf, S("setAutocorrectionType:"), 1L);
        MSG(void, long)(tf, S("setClearButtonMode:"), 1L);
    };
    MSG(void, void *)(a, S("addTextFieldWithConfigurationHandler:"), (void *)cfg);
}

static const char *alert_text(id a, int i)
{
    id fields = MSG(id)(a, S("textFields"));
    return cstr(MSG(id)(MSG(id, unsigned long)(fields, S("objectAtIndex:"), (unsigned long)i), S("text")));
}

/* First time we need a name, ask for it; after that, use the saved one. */
static void ask_name_then(const char *why, void (^then)(const char *name))
{
    const char *have = my_name();
    int forced = why && strcmp(why, "Your name") == 0;
    if (have[0] && !forced) { then(have); return; }
    id a = alert(forced ? "Your name" : "What should people call you?", why && !forced ? why : "Everyone in the room sees this name.");
    alert_field(a, "Your name", have[0] ? have : NULL, 1);
    alert_action(a, "Cancel", 1, NULL);
    alert_action(a, "OK", 0, ^(id act) {
        (void)act;
        char nm[WP_NAME_MAX + 1];
        wp_sanitize_name(nm, sizeof nm, alert_text(a, 0));
        save_string("displayName", nm);
        then(nm);
    });
    present(a);
}

static void run_host(void)
{
    id a = alert("Host a Room", "People on your Wi-Fi see it and can ask to join — you choose who gets in. Keep WatchParty open while you host.");
    char room[WP_ROOM_MAX + 1] = "";
    if (my_name()[0]) snprintf(room, sizeof room, "%s's room", my_name());
    alert_field(a, "Your name", my_name()[0] ? my_name() : NULL, 1);
    alert_field(a, "Room name (e.g. Movie night)", room[0] ? room : NULL, 1);
    alert_action(a, "Cancel", 1, NULL);
    alert_action(a, "Host", 0, ^(id act) {
        (void)act;
        char nm[WP_NAME_MAX + 1];
        wp_sanitize_name(nm, sizeof nm, alert_text(a, 0));
        save_string("displayName", nm);
        if (G.rooms_vc) MSG(void, BOOL, void *)(G.vc, S("dismissViewControllerAnimated:completion:"), YES, NULL);
        wp_room_host(nm, alert_text(a, 1));
    });
    present(a);
}

static void run_join_code(int nearby_index)
{
    char msg[200];
    const char *rn = nearby_index >= 0 ? wp_room_nearby_name(nearby_index) : NULL;
    name64 rm = { "" };
    if (rn) wp_strlcpy(rm.s, rn, sizeof rm.s);
    if (rn) snprintf(msg, sizeof msg, "Enter the invite code for “%s”.", rn);
    else snprintf(msg, sizeof msg, "Enter the invite code, and the host's address if they're not on your Wi-Fi.");
    id a = alert("Join with Invite Code", msg);
    alert_field(a, "Your name", my_name()[0] ? my_name() : NULL, 1);
    alert_field(a, "abcd-efgh-ijkl-mnop-qrst-uvwx", NULL, 0);
    if (!rn) alert_field(a, "Address, e.g. 192.168.1.20 (optional)", NULL, 0);
    alert_action(a, "Cancel", 1, NULL);
    alert_action(a, "Join", 0, ^(id act) {
        (void)act;
        char nm[WP_NAME_MAX + 1];
        wp_sanitize_name(nm, sizeof nm, alert_text(a, 0));
        save_string("displayName", nm);
        char code[128], addr[256] = "";
        wp_strlcpy(code, alert_text(a, 1), sizeof code);
        if (!rm.s[0]) wp_strlcpy(addr, alert_text(a, 2), sizeof addr);
        wp_trim(addr);
        int idx = -1;
        if (rm.s[0]) for (int i = 0; i < wp_room_nearby_count(); i++) if (strcmp(wp_room_nearby_name(i), rm.s) == 0) idx = i;
        if (!addr[0] && idx < 0 && wp_room_nearby_count() == 1 && !rm.s[0]) idx = 0;  /* one room nearby: that one */
        if (G.rooms_vc) MSG(void, BOOL, void *)(G.vc, S("dismissViewControllerAnimated:completion:"), YES, NULL);
        wp_room_join(nm, idx, addr, code);
    });
    present(a);
}

static void ask_room(int index)
{
    if (index < 0 || index >= wp_room_nearby_count()) return;
    name64 rm;
    wp_strlcpy(rm.s, wp_room_nearby_name(index), sizeof rm.s);
    ask_name_then("The host sees this name when you ask to join.", ^(const char *nm) {
        int idx = -1;
        for (int i = 0; i < wp_room_nearby_count(); i++) if (strcmp(wp_room_nearby_name(i), rm.s) == 0) idx = i;
        if (idx < 0) { say("“%s” isn't on the network any more.", rm.s); return; }
        if (G.rooms_vc) MSG(void, BOOL, void *)(G.vc, S("dismissViewControllerAnimated:completion:"), YES, NULL);
        wp_room_ask(nm, idx);
    });
}

static void share_invite(void)
{
    if (RS()->role != WP_ROLE_HOST) { say("Host a room first — then you can share the invite."); return; }
    char text[900];
    wp_room_invite_text(text, sizeof text);
    id items[1] = { nsstr(text) };
    id vc = MSG(id, id, id)(MSG(id)(C("UIActivityViewController"), S("alloc")), S("initWithActivityItems:applicationActivities:"),
                            array_of(items, 1), NULL);
    present(vc);
    release(vc);
}

/* ---------------------------------------------------- rooms sheet */

static void rooms_list_build(void)
{
    if (!G.rooms_list) return;
    id subs = MSG(id)(G.rooms_list, S("subviews"));
    MSG(void, SEL)(subs, S("makeObjectsPerformSelector:"), S("removeFromSuperview"));
    CGRect b = bounds_of(G.rooms_list);
    double w = b.size.width - 32, y = 20;
    const wp_room_state *rs = RS();

    id h = label("Rooms on your Wi-Fi", 22, 1, NULL);
    set_frame(h, rect(16, y, w, 28)); MSG(void, id)(G.rooms_list, S("addSubview:"), h); release(h);
    y += 32;
    id sub = label("Ask to join and the host lets you in — no code needed.", 14, 0, white(0.6));
    MSG(void, long)(sub, S("setNumberOfLines:"), 0L);
    CGSize ss = fit(sub, w);
    set_frame(sub, rect(16, y, w, ss.height)); MSG(void, id)(G.rooms_list, S("addSubview:"), sub); release(sub);
    y += ss.height + 16;

    int n = wp_room_nearby_count();
    if (!n) {
        id none = label("No rooms yet. When someone on this Wi-Fi hosts one, it shows up here.", 15, 0, white(0.7));
        MSG(void, long)(none, S("setNumberOfLines:"), 0L);
        CGSize ns = fit(none, w);
        set_frame(none, rect(16, y, w, ns.height)); MSG(void, id)(G.rooms_list, S("addSubview:"), none); release(none);
        y += ns.height + 20;
    }
    for (int i = 0; i < n; i++) {
        const char *name = wp_room_nearby_name(i);
        id row = new_view(rect(16, y, w, 104), NULL);
        MSG(void, id)(row, S("setBackgroundColor:"), white(0.08));
        round_corners(row, 14);
        char t[100];
        snprintf(t, sizeof t, "📺  %s", name);
        id l = label(t, 17, 1, NULL);
        set_frame(l, rect(14, 12, w - 28, 24)); MSG(void, id)(row, S("addSubview:"), l); release(l);
        int here = rs->role == WP_ROLE_CLIENT && strcmp(rs->room, name) == 0;
        int asking = rs->ask != WP_ASK_NONE && strcmp(rs->ask_room, name) == 0;
        if (here || asking) {
            id s2 = label(here ? "You're in this room" : "Asking… waiting for the host", 14, 0, white(0.6));
            set_frame(s2, rect(14, 52, w - 28, 20)); MSG(void, id)(row, S("addSubview:"), s2); release(s2);
            set_frame(row, rect(16, y, w, 88));
            y += 88 + 10;
        } else {
            double bw = (w - 28 - 8) / 2;
            id code = button("Code…", "codeRoom:", 0);
            MSG(void, long)(code, S("setTag:"), (long)i);
            set_frame(code, rect(14, 50, bw, 40)); MSG(void, id)(row, S("addSubview:"), code);
            int can = wp_room_nearby_can_ask(i);
            id ask = button(can ? "Ask to join" : "Needs a code", can ? "askRoom:" : "codeRoom:", can);
            MSG(void, long)(ask, S("setTag:"), (long)i);
            set_frame(ask, rect(14 + bw + 8, 50, bw, 40)); MSG(void, id)(row, S("addSubview:"), ask);
            y += 104 + 10;
        }
        MSG(void, id)(G.rooms_list, S("addSubview:"), row); release(row);
    }
    y += 10;
    double bw = (w - 8) / 2;
    id host = button("Host a Room", "hostRoom:", 0);
    set_frame(host, rect(16, y, bw, 44)); MSG(void, id)(G.rooms_list, S("addSubview:"), host);
    id code = button("Join with Code", "codeRoom:", 0);
    MSG(void, long)(code, S("setTag:"), -1L);
    set_frame(code, rect(16 + bw + 8, y, bw, 44)); MSG(void, id)(G.rooms_list, S("addSubview:"), code);
    y += 44 + 30;
    MSG(void, CGSize)(G.rooms_list, S("setContentSize:"), (CGSize){ b.size.width, y });
}

static void rooms_vc_layout(id self, SEL cmd)
{
    (void)cmd;
    struct objc_super sup = { self, (Class)objc_getClass("UIViewController") };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, S("viewDidLayoutSubviews"));
    id v = MSG(id)(self, S("view"));
    CGRect b = bounds_of(v);
    static double built_w = -1;
    set_frame(G.rooms_list, b);
    if (!G.rooms_list) return;
    if (G.rooms_built || built_w != b.size.width) { G.rooms_built = 0; built_w = b.size.width; rooms_list_build(); }
}

static void rooms_closed(void);

static void show_rooms(void)
{
    if (G.rooms_vc && !MSG(id)(G.rooms_vc, S("presentingViewController"))) rooms_closed();   /* never made it up */
    if (G.rooms_vc) return;
    id vc = MSG(id)(MSG(id)((id)objc_getClass("WPRoomsVC"), S("alloc")), S("init"));
    id v = MSG(id)(vc, S("view"));
    MSG(void, id)(v, S("setBackgroundColor:"), color(0.07, 0.07, 0.09, 1));
    G.rooms_list = MSG(id, CGRect)(MSG(id)(C("UIScrollView"), S("alloc")), S("initWithFrame:"), bounds_of(v));
    MSG(void, BOOL)(G.rooms_list, S("setAlwaysBounceVertical:"), YES);
    MSG(void, id)(v, S("addSubview:"), G.rooms_list);
    release(G.rooms_list);
    id sheet = responds(vc, "sheetPresentationController") ? MSG(id)(vc, S("sheetPresentationController")) : NULL;
    if (sheet) {
        id dets[2] = { MSG(id)(C("UISheetPresentationControllerDetent"), S("mediumDetent")),
                       MSG(id)(C("UISheetPresentationControllerDetent"), S("largeDetent")) };
        MSG(void, id)(sheet, S("setDetents:"), array_of(dets, 2));
        MSG(void, BOOL)(sheet, S("setPrefersGrabberVisible:"), YES);
    }
    G.rooms_vc = vc;                                   /* keeps our +1 until rooms_closed() */
    G.rooms_built = 1;                                 /* build on the first layout */
    present(vc);
    wp_net_browse_start();
}

static void rooms_closed(void)
{
    id vc = G.rooms_vc;
    G.rooms_vc = NULL;
    G.rooms_list = NULL;
    if (vc) dispatch_async(dispatch_get_main_queue(), ^{ release(vc); });   /* not from inside its own callback */
}

static void rooms_vc_gone(id self, SEL cmd, BOOL animated)
{
    (void)cmd;
    struct objc_super sup = { self, (Class)objc_getClass("UIViewController") };
    ((void (*)(struct objc_super *, SEL, BOOL))objc_msgSendSuper)(&sup, S("viewDidDisappear:"), animated);
    if (self == G.rooms_vc) rooms_closed();           /* swiped away or dismissed by us */
}

/* --------------------------------------------------- search results */

static void results_hide(void)
{
    G.res_visible = 0;
    G.res_gen++;
    set_hidden(G.results, 1);
}

static void on_thumb(int row, int gen, const void *bytes, size_t len)
{
    if (!bytes || gen != G.res_gen || row < 0 || row >= WP_MAX_RESULTS || !G.res_thumbs[row]) return;
    id data = MSG(id, const void *, unsigned long)(C("NSData"), S("dataWithBytes:length:"), bytes, (unsigned long)len);
    id img = MSG(id, id)(C("UIImage"), S("imageWithData:"), data);
    if (img) MSG(void, id)(G.res_thumbs[row], S("setImage:"), img);
}

static void results_show(const char *head)
{
    id subs = MSG(id)(G.results, S("subviews"));
    MSG(void, SEL)(subs, S("makeObjectsPerformSelector:"), S("removeFromSuperview"));
    memset(G.res_thumbs, 0, sizeof G.res_thumbs);
    layout();
    CGRect b = bounds_of(G.results);
    double w = b.size.width, y = 10;
    id h = label(head, 13, 1, white(0.7));
    MSG(void, long)(h, S("setNumberOfLines:"), 0L);
    CGSize hs = fit(h, w - 64);
    set_frame(h, rect(14, y, w - 64, hs.height)); MSG(void, id)(G.results, S("addSubview:"), h); release(h);
    id close = button("✕", "closeResults:", 0);
    set_frame(close, rect(w - 46, y - 4, 34, 30)); MSG(void, id)(G.results, S("addSubview:"), close);
    y += (hs.height > 26 ? hs.height : 26) + 8;
    for (int i = 0; i < G.nres; i++) {
        id row = MSG(id, long)(C("UIButton"), S("buttonWithType:"), 0L);
        set_frame(row, rect(8, y, w - 16, 76));
        MSG(void, long)(row, S("setTag:"), (long)i);
        MSG(void, id, SEL, unsigned long)(row, S("addTarget:action:forControlEvents:"), G.ctl, S("pickResult:"), TOUCH_UP);
        id thumb = MSG(id, CGRect)(MSG(id)(C("UIImageView"), S("alloc")), S("initWithFrame:"), rect(6, 6, 114, 64));
        MSG(void, long)(thumb, S("setContentMode:"), 2L);                  /* aspect fill */
        MSG(void, id)(thumb, S("setBackgroundColor:"), white(0.1));
        round_corners(thumb, 8);
        MSG(void, id)(row, S("addSubview:"), thumb);
        G.res_thumbs[i] = thumb;
        release(thumb);
        id t = label(G.res[i].title, 14, 1, NULL);
        MSG(void, long)(t, S("setNumberOfLines:"), 2L);
        set_frame(t, rect(130, 6, w - 16 - 136, 38)); MSG(void, id)(row, S("addSubview:"), t); release(t);
        id m = label(G.res[i].meta, 12, 0, white(0.55));
        MSG(void, long)(m, S("setNumberOfLines:"), 2L);
        set_frame(m, rect(130, 44, w - 16 - 136, 30)); MSG(void, id)(row, S("addSubview:"), m); release(m);
        MSG(void, id)(G.results, S("addSubview:"), row);
        char url[128];
        snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", G.res[i].id);
        wp_fetch(url, i, G.res_gen, on_thumb);
        y += 80;
    }
    MSG(void, CGSize)(G.results, S("setContentSize:"), (CGSize){ w, y + 10 });
    G.res_visible = 1;
    set_hidden(G.results, 0);
}

static void on_results(int gen, const wp_result *r, int n, int ok)
{
    if (gen != G.res_gen) return;
    G.nres = n;
    if (n) memcpy(G.res, r, (size_t)n * sizeof *r);
    char head[300];
    if (!ok) snprintf(head, sizeof head, "Search failed — check the connection, or paste a link");
    else if (!n) snprintf(head, sizeof head, "No results for “%s”", G.last_query);
    else snprintf(head, sizeof head, "Tap one to play it for everyone");
    results_show(head);
}

static int looks_like_link(const char *text)
{
    if (wp_is_video_id(text)) return 1;
    return strstr(text, "youtube.com/") || strstr(text, "youtu.be/") || strstr(text, "://") || strncmp(text, "www.", 4) == 0;
}

static void search_submit(void)
{
    char q[1024];
    wp_strlcpy(q, cstr(MSG(id)(G.search, S("text"))), sizeof q);
    wp_trim(q);
    if (!q[0]) return;
    if (looks_like_link(q)) {
        results_hide();
        MSG(BOOL)(G.search, S("resignFirstResponder"));
        wp_room_load(q);
        return;
    }
    if (G.res_visible && G.nres > 0 && strcmp(q, G.last_query) == 0) {   /* Search again: play the top hit */
        char vid[WP_VIDEO_ID_LEN + 1];
        wp_strlcpy(vid, G.res[0].id, sizeof vid);
        results_hide();
        MSG(BOOL)(G.search, S("resignFirstResponder"));
        wp_room_load(vid);
        return;
    }
    wp_strlcpy(G.last_query, q, sizeof G.last_query);
    int gen = ++G.res_gen;
    G.nres = 0;
    char head[300];
    snprintf(head, sizeof head, "Searching YouTube for “%s”…", q);
    results_show(head);
    wp_yt_search(q, gen, on_results);
}

/* ---------------------------------------------- engine → iPhone UI */

static void ui_say(const char *line) { say("%s", line); }
static void ui_note(const char *line) { log_line(line); }
static void ui_player(const char *js)
{
    if (G.web) MSG(void, id, void *)(G.web, S("evaluateJavaScript:completionHandler:"), nsstr(js), NULL);
}
static void ui_video_loaded(const char *vid)
{
    char url[80];
    snprintf(url, sizeof url, "youtu.be/%s", vid);
    if (G.search) MSG(void, id)(G.search, S("setText:"), nsstr(url));
    update_info();
}
static void ui_sticker(const char *text, const char *by, const char *image) { show_sticker(text, by, image); }
static void ui_beep(void) { haptic(2); }

static void ask_card_refresh(void)
{
    const wp_room_state *rs = RS();
    if (rs->role != WP_ROLE_NONE || rs->ask != WP_ASK_NONE)
        for (int k = 0; k < MAX_CARDS; k++) if (G.cards[k].kind == CARD_ROOM) card_remove(k);
    int i = card_find(CARD_ASK, 0);
    if (rs->ask == WP_ASK_NONE) { if (i >= 0) card_remove(i); return; }
    int state = rs->ask * 10000 + atoi(rs->ask_check);
    if (i >= 0 && G.cards[i].key == state) return;
    if (i < 0 && (i = card_slot()) < 0) return;
    char title[120], body[300];
    if (rs->ask == WP_ASK_WAITING) {
        snprintf(title, sizeof title, "Waiting for “%s” to let you in", rs->ask_room);
        snprintf(body, sizeof body, "Your check code is %s — the host sees the same code, so they know it's you.", rs->ask_check);
        haptic(0);
    } else {
        snprintf(title, sizeof title, "Asking “%s”…", rs->ask_room);
        snprintf(body, sizeof body, "Knocking on the host's door.");
    }
    card_build(i, CARD_ASK, title, body, NULL, NULL, "Cancel", "askCancel:");
    G.cards[i].key = state;
}

static void ui_changed(void)
{
    update_info();
    ask_card_refresh();
    if (G.rooms_vc) rooms_list_build();
}

static void ui_rooms_changed(void)
{
    if (!G.view) return;
    if (G.rooms_vc) rooms_list_build();
    const wp_room_state *rs = RS();
    int n = wp_room_nearby_count();
    for (int i = 0; i < MAX_CARDS; i++) {
        if (G.cards[i].kind != CARD_ROOM) continue;
        int still = 0;
        for (int j = 0; j < n; j++) if (strcmp(wp_room_nearby_name(j), G.cards[i].room) == 0) still = 1;
        if (!still) card_remove(i);
    }
    for (int j = 0; j < n; j++) {
        const char *name = wp_room_nearby_name(j);
        int seen = 0;
        for (int k = 0; k < G.n_seen; k++) if (strcmp(G.seen[k], name) == 0) seen = 1;
        if (seen) continue;
        if (G.n_seen < WP_MAX_ROOMS) wp_strlcpy(G.seen[G.n_seen++], name, sizeof G.seen[0]);
        if (rs->role != WP_ROLE_NONE || rs->ask != WP_ASK_NONE || G.rooms_vc) continue;
        int i = card_slot();
        if (i < 0) continue;
        char title[120];
        snprintf(title, sizeof title, "“%s” is on your Wi-Fi", name);
        int can = wp_room_nearby_can_ask(j);
        card_build(i, CARD_ROOM, title, "Someone nearby is hosting a watch party.", "Not now", "cardDismiss:",
                   can ? "Ask to join" : "Join with code", can ? "cardAsk:" : "cardCode:");
        wp_strlcpy(G.cards[i].room, name, sizeof G.cards[i].room);
        int gen = G.cards[i].gen;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)TOAST_MS * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (G.cards[i].kind == CARD_ROOM && G.cards[i].gen == gen) card_remove(i);
        });
    }
    update_info();
}

static void ui_knock(int knock_id, const char *name, const char *check)
{
    int i = card_slot();
    if (i < 0) return;
    char title[96], body[300];
    snprintf(title, sizeof title, "%s wants to join", name);
    snprintf(body, sizeof body, "Check code %s — it's on %s's screen too. If it matches, it's really them.", check, name);
    card_build(i, CARD_KNOCK, title, body, "Not now", "knockDeny:", "Let in", "knockAllow:");
    G.cards[i].key = knock_id;
    haptic(1);
    update_info();
}

static void ui_knock_gone(int knock_id)
{
    int i = card_find(CARD_KNOCK, knock_id);
    if (i >= 0) card_remove(i);
    update_info();
}

/* ------------------------------------------------- controller actions */

static long tag_of(id sender) { return MSG(long)(sender, S("tag")); }

static void ctl_send(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd; (void)sender;
    char text[WP_TEXT_MAX + 1];
    wp_strlcpy(text, cstr(MSG(id)(G.input, S("text"))), sizeof text);
    wp_trim(text);
    if (!text[0]) return;
    MSG(void, id)(G.input, S("setText:"), nsstr(""));
    wp_room_chat(text);
}

static BOOL ctl_should_return(id self, SEL cmd, id field)
{
    (void)self; (void)cmd;
    if (field == G.search) search_submit();
    else if (field == G.input) ctl_send(NULL, NULL, NULL);
    return NO;
}

static void ctl_react(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd;
    long t = tag_of(sender);
    if (t >= 1000) {                                   /* favourite emoji: a plain chat message */
        id title = MSG(id, unsigned long)(sender, S("titleForState:"), 0UL);
        wp_room_chat(cstr(title));
    } else wp_room_react((int)t);
}

static void ctl_pick_result(id self, SEL cmd, id sender)
{
    (void)self; (void)cmd;
    long i = tag_of(sender);
    if (i < 0 || i >= G.nres) return;
    char vid[WP_VIDEO_ID_LEN + 1];
    wp_strlcpy(vid, G.res[i].id, sizeof vid);
    results_hide();
    MSG(BOOL)(G.search, S("resignFirstResponder"));
    wp_room_load(vid);
}

static void ctl_close_results(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; results_hide(); MSG(BOOL)(G.search, S("resignFirstResponder")); }
static void ctl_rooms(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; show_rooms(); }
static void ctl_host(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; run_host(); }
static void ctl_ask_room(id self, SEL cmd, id sender) { (void)self; (void)cmd; ask_room((int)tag_of(sender)); }
static void ctl_code_room(id self, SEL cmd, id sender) { (void)self; (void)cmd; run_join_code((int)tag_of(sender)); }
static void ctl_ask_cancel(id self, SEL cmd, id sender) { (void)self; (void)cmd; (void)sender; wp_room_ask_cancel(); }

static int card_of(id sender) { long t = tag_of(sender); return (int)(t >= 200 ? t - 200 : t - 100); }

static void ctl_knock(id sender, int allow)
{
    int i = card_of(sender);
    if (i < 0 || i >= MAX_CARDS || G.cards[i].kind != CARD_KNOCK) return;
    wp_room_answer(G.cards[i].key, allow);
}
static void ctl_knock_allow(id self, SEL cmd, id sender) { (void)self; (void)cmd; ctl_knock(sender, 1); }
static void ctl_knock_deny(id self, SEL cmd, id sender) { (void)self; (void)cmd; ctl_knock(sender, 0); }
static void ctl_card_dismiss(id self, SEL cmd, id sender) { (void)self; (void)cmd; card_remove(card_of(sender)); }
static void ctl_card_room(id sender, int ask)
{
    int i = card_of(sender);
    if (i < 0 || i >= MAX_CARDS || G.cards[i].kind != CARD_ROOM) return;
    char room[64];
    wp_strlcpy(room, G.cards[i].room, sizeof room);
    card_remove(i);
    for (int j = 0; j < wp_room_nearby_count(); j++)
        if (strcmp(wp_room_nearby_name(j), room) == 0) { if (ask) ask_room(j); else run_join_code(j); return; }
    say("“%s” isn't on the network any more.", room);
}
static void ctl_card_ask(id self, SEL cmd, id sender) { (void)self; (void)cmd; ctl_card_room(sender, 1); }
static void ctl_card_code(id self, SEL cmd, id sender) { (void)self; (void)cmd; ctl_card_room(sender, 0); }

static void ctl_script_message(id self, SEL cmd, id ucc, id message)
{
    (void)self; (void)cmd; (void)ucc;
    id body = MSG(id)(message, S("body"));
    if (body && MSG(BOOL, id)(body, S("isKindOfClass:"), C("NSString"))) wp_room_player_event(cstr(body));
}

static void ctl_keyboard(id self, SEL cmd, id note)
{
    (void)self; (void)cmd;
    id info = MSG(id)(note, S("userInfo"));
    id v = MSG(id, id)(info, S("objectForKey:"), UIKeyboardFrameEndUserInfoKey);
    CGRect kb = MSG_STRET(CGRect)(v, S("CGRectValue"));          /* screen coordinates */
    id screen_space = MSG(id)(MSG(id)(G.window, S("screen")), S("coordinateSpace"));
    if (screen_space) kb = MSG_STRET(CGRect, CGRect, id)(G.view, S("convertRect:fromCoordinateSpace:"), kb, screen_space);
    double h = bounds_of(G.view).size.height - kb.origin.y;
    if (kb.size.height <= 0) h = 0;
    G.kb_h = h > 0 ? h : 0;
    double dur = MSG(double)(MSG(id, id)(info, S("objectForKey:"), UIKeyboardAnimationDurationUserInfoKey), S("doubleValue"));
    void (^anim)(void) = ^{ layout(); };
    MSG(void, double, void *)(C("UIView"), S("animateWithDuration:animations:"), dur > 0 ? dur : 0.25, (void *)anim);
}

static void ctl_tap_video_area(id self, SEL cmd, id g) { (void)self; (void)cmd; (void)g; MSG(void, BOOL)(G.view, S("endEditing:"), YES); }

/* ----------------------------------------------------- building views */

static void build_ui(void)
{
    id v = G.view;
    MSG(void, id)(v, S("setBackgroundColor:"), MSG(id)(C("UIColor"), S("blackColor")));

    G.title_lbl = label("WatchParty", 22, 1, NULL);
    MSG(void, id)(v, S("addSubview:"), G.title_lbl);
    G.status_lbl = label("", 13, 0, white(0.6));
    MSG(void, long)(G.status_lbl, S("setLineBreakMode:"), 4L);
    MSG(void, id)(v, S("addSubview:"), G.status_lbl);

    G.rooms_btn = MSG(id, long)(C("UIButton"), S("buttonWithType:"), 1L);
    id rimg = MSG(id, id)(C("UIImage"), S("systemImageNamed:"), nsstr("person.2.wave.2.fill"));
    if (!rimg) rimg = MSG(id, id)(C("UIImage"), S("systemImageNamed:"), nsstr("person.2.fill"));
    MSG(void, id, unsigned long)(G.rooms_btn, S("setImage:forState:"), rimg, 0UL);
    MSG(void, id)(G.rooms_btn, S("setTintColor:"), white(0.95));
    MSG(void, id, SEL, unsigned long)(G.rooms_btn, S("addTarget:action:forControlEvents:"), G.ctl, S("showRooms:"), TOUCH_UP);
    MSG(void, id)(v, S("addSubview:"), G.rooms_btn);

    G.menu_btn = MSG(id, long)(C("UIButton"), S("buttonWithType:"), 1L);
    MSG(void, id, unsigned long)(G.menu_btn, S("setImage:forState:"), MSG(id, id)(C("UIImage"), S("systemImageNamed:"), nsstr("ellipsis.circle")), 0UL);
    MSG(void, id)(G.menu_btn, S("setTintColor:"), white(0.95));
    MSG(void, BOOL)(G.menu_btn, S("setShowsMenuAsPrimaryAction:"), YES);
    MSG(void, id)(v, S("addSubview:"), G.menu_btn);

    G.search = MSG(id, CGRect)(MSG(id)(C("UITextField"), S("alloc")), S("initWithFrame:"), rect(0, 0, 10, 10));
    MSG(void, id)(G.search, S("setPlaceholder:"), nsstr("Search YouTube or paste a link"));
    MSG(void, long)(G.search, S("setBorderStyle:"), 3L);
    MSG(void, long)(G.search, S("setReturnKeyType:"), 6L);              /* Search */
    MSG(void, long)(G.search, S("setClearButtonMode:"), 1L);
    MSG(void, long)(G.search, S("setAutocorrectionType:"), 1L);
    MSG(void, long)(G.search, S("setKeyboardType:"), 0L);
    MSG(void, id)(G.search, S("setDelegate:"), G.ctl);
    MSG(void, id)(v, S("addSubview:"), G.search);

    /* the player */
    id cfg = alloc_init("WKWebViewConfiguration");
    MSG(void, BOOL)(cfg, S("setAllowsInlineMediaPlayback:"), YES);
    MSG(void, unsigned long)(cfg, S("setMediaTypesRequiringUserActionForPlayback:"), 0UL);
    MSG(void, id)(cfg, S("setApplicationNameForUserAgent:"), nsstr("Version/18.6 Mobile/15E148 Safari/604.1"));
    id ucc = MSG(id)(cfg, S("userContentController"));
    MSG(void, id, id)(ucc, S("addScriptMessageHandler:name:"), G.ctl, nsstr("wp"));
    id script = MSG(id, id, long, BOOL)(MSG(id)(C("WKUserScript"), S("alloc")), S("initWithSource:injectionTime:forMainFrameOnly:"),
        nsstr("var m=document.getElementById('msg');if(m)m.innerHTML='Search YouTube above, or paste a link.<br>Everyone in the room watches it together.';"),
        1L, YES);
    MSG(void, id)(ucc, S("addUserScript:"), script);
    release(script);
    G.web = MSG(id, CGRect, id)(MSG(id)(C("WKWebView"), S("alloc")), S("initWithFrame:configuration:"), rect(0, 0, 320, 180), cfg);
    release(cfg);
    MSG(void, BOOL)(G.web, S("setOpaque:"), NO);
    MSG(void, id)(G.web, S("setBackgroundColor:"), MSG(id)(C("UIColor"), S("blackColor")));
    MSG(void, BOOL)(MSG(id)(G.web, S("scrollView")), S("setScrollEnabled:"), NO);
    MSG(void, id)(v, S("addSubview:"), G.web);
    id bundle = MSG(id)(C("NSBundle"), S("mainBundle"));
    id path = MSG(id, id, id)(bundle, S("pathForResource:ofType:"), nsstr("player"), nsstr("html"));
    id html = path ? MSG(id, id, unsigned long, void *)(C("NSString"), S("stringWithContentsOfFile:encoding:error:"), path, 4UL, NULL) : NULL;
    if (!html) html = nsstr("<html><body style='background:#000;color:#aaa;font-family:sans-serif;padding:24px'>player.html is missing.</body></html>");
    MSG(id, id, id)(G.web, S("loadHTMLString:baseURL:"), html, MSG(id, id)(C("NSURL"), S("URLWithString:"), nsstr("https://localhost/")));

    /* chat */
    G.chat = MSG(id, CGRect)(MSG(id)(C("UITextView"), S("alloc")), S("initWithFrame:"), rect(0, 0, 10, 10));
    MSG(void, BOOL)(G.chat, S("setEditable:"), NO);
    MSG(void, id)(G.chat, S("setBackgroundColor:"), MSG(id)(C("UIColor"), S("clearColor")));
    MSG(void, id)(G.chat, S("setTextColor:"), white(0.92));
    MSG(void, id)(G.chat, S("setFont:"), font(15, 0));
    MSG(void, id)(v, S("addSubview:"), G.chat);
    id tap = MSG(id, id, SEL)(MSG(id)(C("UITapGestureRecognizer"), S("alloc")), S("initWithTarget:action:"), G.ctl, S("tapChat:"));
    MSG(void, id)(G.chat, S("addGestureRecognizer:"), tap);
    release(tap);

    /* reactions strip: favourite emoji, then the lingo stickers */
    G.react_bar = MSG(id, CGRect)(MSG(id)(C("UIScrollView"), S("alloc")), S("initWithFrame:"), rect(0, 0, 10, 44));
    MSG(void, BOOL)(G.react_bar, S("setShowsHorizontalScrollIndicator:"), NO);
    MSG(void, id)(v, S("addSubview:"), G.react_bar);
    static const char *fav[] = { "😂", "❤️", "🔥", "👏", "😮", "😢" };
    double x = 10;
    for (int i = 0; i < 6; i++) {
        id b = MSG(id, long)(C("UIButton"), S("buttonWithType:"), 1L);
        MSG(void, id, unsigned long)(b, S("setTitle:forState:"), nsstr(fav[i]), 0UL);
        MSG(void, id)(MSG(id)(b, S("titleLabel")), S("setFont:"), font(24, 0));
        MSG(void, long)(b, S("setTag:"), 1000L + i);
        MSG(void, id, SEL, unsigned long)(b, S("addTarget:action:forControlEvents:"), G.ctl, S("react:"), TOUCH_UP);
        set_frame(b, rect(x, 2, 40, 40));
        MSG(void, id)(G.react_bar, S("addSubview:"), b);
        x += 42;
    }
    for (int i = 0; i < WP_N_REACTIONS; i++) {
        const wp_reaction *r = &WP_REACTIONS[i];
        char t[64];
        if (r->emoji[0]) snprintf(t, sizeof t, "%s %s", r->emoji, r->word); else snprintf(t, sizeof t, "%s", r->word);
        id b = button(t, "react:", 0);
        MSG(void, long)(b, S("setTag:"), (long)i);
        MSG(void, id)(MSG(id)(b, S("titleLabel")), S("setFont:"), font(14, 1));
        if (r->image) {
            id img = MSG(id, id)(C("UIImage"), S("imageNamed:"), nsstr(r->image));
            if (img) {
                CGSize sz = { 22, 22 };
                id fmt = MSG(id)(C("UIGraphicsImageRendererFormat"), S("defaultFormat"));
                id rend = MSG(id, CGSize, id)(MSG(id)(C("UIGraphicsImageRenderer"), S("alloc")), S("initWithSize:format:"), sz, fmt);
                id small = MSG(id, void *)(rend, S("imageWithActions:"), (void *)^(id ctx) {
                    (void)ctx; MSG(void, CGRect)(img, S("drawInRect:"), rect(0, 0, 22, 22));
                });
                release(rend);
                small = MSG(id, long)(small, S("imageWithRenderingMode:"), 1L);   /* original colours */
                MSG(void, id, unsigned long)(b, S("setImage:forState:"), small, 0UL);
            }
        }
        CGSize s = fit(b, 400);
        double bw = s.width + 22;
        set_frame(b, rect(x, 5, bw, 34));
        MSG(void, id)(G.react_bar, S("addSubview:"), b);
        x += bw + 6;
    }
    MSG(void, CGSize)(G.react_bar, S("setContentSize:"), (CGSize){ x + 10, 44 });

    /* message box */
    G.input_bar = new_view(rect(0, 0, 10, 50), NULL);
    MSG(void, id)(G.input_bar, S("setBackgroundColor:"), color(0.06, 0.06, 0.08, 1));
    G.input = MSG(id, CGRect)(MSG(id)(C("UITextField"), S("alloc")), S("initWithFrame:"), rect(0, 0, 10, 10));
    MSG(void, id)(G.input, S("setPlaceholder:"), nsstr("Say something…"));
    MSG(void, long)(G.input, S("setBorderStyle:"), 3L);
    MSG(void, long)(G.input, S("setReturnKeyType:"), 7L);               /* Send */
    MSG(void, id)(G.input, S("setDelegate:"), G.ctl);
    MSG(void, id)(G.input_bar, S("addSubview:"), G.input);
    G.send_btn = button("Send", "send:", 1);
    MSG(void, id)(G.input_bar, S("addSubview:"), G.send_btn);
    MSG(void, id)(v, S("addSubview:"), G.input_bar);

    /* search results over the chat */
    G.results = MSG(id, CGRect)(MSG(id)(C("UIScrollView"), S("alloc")), S("initWithFrame:"), rect(0, 0, 10, 10));
    MSG(void, id)(G.results, S("setBackgroundColor:"), color(0.05, 0.05, 0.07, 0.98));
    MSG(void, long)(G.results, S("setKeyboardDismissMode:"), 1L);       /* on drag */
    set_hidden(G.results, 1);
    MSG(void, id)(v, S("addSubview:"), G.results);

    /* landscape subtitles */
    G.sub_box = new_view(rect(0, 0, 10, 10), NULL);
    MSG(void, id)(G.sub_box, S("setBackgroundColor:"), color(0, 0, 0, 0.62));
    MSG(void, BOOL)(G.sub_box, S("setUserInteractionEnabled:"), NO);
    round_corners(G.sub_box, 10);
    G.sub_lbl = label("", 17, 1, color(1, 0.93, 0.25, 1));
    MSG(void, long)(G.sub_lbl, S("setNumberOfLines:"), 0L);
    MSG(void, long)(G.sub_lbl, S("setTextAlignment:"), 1L);
    MSG(void, id)(G.sub_box, S("addSubview:"), G.sub_lbl);
    set_hidden(G.sub_box, 1);
    MSG(void, id)(v, S("addSubview:"), G.sub_box);

    id nc = MSG(id)(C("NSNotificationCenter"), S("defaultCenter"));
    MSG(void, id, SEL, id, id)(nc, S("addObserver:selector:name:object:"), G.ctl, S("keyboard:"), UIKeyboardWillChangeFrameNotification, NULL);

    log_line("Welcome to WatchParty. Rooms on your Wi-Fi show up under 👥 — ask to join and the host lets you in.");
    log_line("Or host your own from ⋯. Search YouTube above; whatever you play, everyone watches together. Turn sideways for full screen.");
}

/* -------------------------------------------------------- the classes */

static void vc_did_layout(id self, SEL cmd)
{
    (void)cmd;
    struct objc_super sup = { self, (Class)objc_getClass("UIViewController") };
    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, S("viewDidLayoutSubviews"));
    layout();
}
static BOOL vc_hide_status(id self, SEL cmd) { (void)self; (void)cmd; return G.landscape ? YES : NO; }
static long vc_status_style(id self, SEL cmd) { (void)self; (void)cmd; return 1; }       /* light content */
static unsigned long vc_orientations(id self, SEL cmd) { (void)self; (void)cmd; return 26; } /* all but upside down */

static void start_app(id scene)
{
    G.window = MSG(id, id)(MSG(id)(C("UIWindow"), S("alloc")), S("initWithWindowScene:"), scene);
    MSG(void, long)(G.window, S("setOverrideUserInterfaceStyle:"), 2L);   /* dark */
    MSG(void, id)(G.window, S("setTintColor:"), accent());
    G.vc = MSG(id)(MSG(id)((id)objc_getClass("WPRootVC"), S("alloc")), S("init"));
    MSG(void, id)(G.window, S("setRootViewController:"), G.vc);
    G.view = MSG(id)(G.vc, S("view"));
    build_ui();
    MSG(void)(G.window, S("makeKeyAndVisible"));

    G.subs_on = saved_bool("subtitles", 1);
    wp_room_ui ui = {
        .say = ui_say, .note = ui_note, .player = ui_player, .video_loaded = ui_video_loaded,
        .changed = ui_changed, .sticker = ui_sticker, .beep = ui_beep, .rooms_changed = ui_rooms_changed,
        .knock = ui_knock, .knock_gone = ui_knock_gone,
    };
    wp_room_init(&ui, my_name()[0] ? my_name() : "Guest");
    RS()->fancy = saved_bool("fancyWords", 1);
    update_info();
    layout();
}

static void scene_connect(id self, SEL cmd, id scene, id session, id options)
{
    (void)self; (void)cmd; (void)session; (void)options;
    if (!G.window) start_app(scene);
}
static id scene_window(id self, SEL cmd) { (void)self; (void)cmd; return G.window; }
static void scene_set_window(id self, SEL cmd, id w) { (void)self; (void)cmd; (void)w; }
static BOOL app_did_finish(id self, SEL cmd, id app, id opts) { (void)self; (void)cmd; (void)app; (void)opts; return YES; }
static void app_will_terminate(id self, SEL cmd, id app) { (void)self; (void)cmd; (void)app; wp_room_shutdown(); }

static void add_proto(Class c, const char *name)
{
    Protocol *p = objc_getProtocol(name);
    if (p) class_addProtocol(c, p);
}

static void make_classes(void)
{
    Class a = objc_allocateClassPair((Class)objc_getClass("UIResponder"), "WPAppDelegate", 0);
    class_addMethod(a, S("application:didFinishLaunchingWithOptions:"), (IMP)app_did_finish, ENC_BOOL "@:@@");
    class_addMethod(a, S("applicationWillTerminate:"), (IMP)app_will_terminate, "v@:@");
    add_proto(a, "UIApplicationDelegate");
    objc_registerClassPair(a);

    Class s = objc_allocateClassPair((Class)objc_getClass("UIResponder"), "WPSceneDelegate", 0);
    class_addMethod(s, S("scene:willConnectToSession:options:"), (IMP)scene_connect, "v@:@@@");
    class_addMethod(s, S("window"), (IMP)scene_window, "@@:");
    class_addMethod(s, S("setWindow:"), (IMP)scene_set_window, "v@:@");
    add_proto(s, "UIWindowSceneDelegate");
    objc_registerClassPair(s);

    Class v = objc_allocateClassPair((Class)objc_getClass("UIViewController"), "WPRootVC", 0);
    class_addMethod(v, S("viewDidLayoutSubviews"), (IMP)vc_did_layout, "v@:");
    class_addMethod(v, S("prefersStatusBarHidden"), (IMP)vc_hide_status, ENC_BOOL "@:");
    class_addMethod(v, S("prefersHomeIndicatorAutoHidden"), (IMP)vc_hide_status, ENC_BOOL "@:");
    class_addMethod(v, S("preferredStatusBarStyle"), (IMP)vc_status_style, "q@:");
    class_addMethod(v, S("supportedInterfaceOrientations"), (IMP)vc_orientations, "Q@:");
    objc_registerClassPair(v);

    Class r = objc_allocateClassPair((Class)objc_getClass("UIViewController"), "WPRoomsVC", 0);
    class_addMethod(r, S("viewDidLayoutSubviews"), (IMP)rooms_vc_layout, "v@:");
    class_addMethod(r, S("viewDidDisappear:"), (IMP)rooms_vc_gone, "v@:" ENC_BOOL);
    objc_registerClassPair(r);

    Class k = objc_allocateClassPair((Class)objc_getClass("NSObject"), "WPCtl", 0);
    class_addMethod(k, S("send:"), (IMP)ctl_send, "v@:@");
    class_addMethod(k, S("textFieldShouldReturn:"), (IMP)ctl_should_return, ENC_BOOL "@:@");
    class_addMethod(k, S("react:"), (IMP)ctl_react, "v@:@");
    class_addMethod(k, S("pickResult:"), (IMP)ctl_pick_result, "v@:@");
    class_addMethod(k, S("closeResults:"), (IMP)ctl_close_results, "v@:@");
    class_addMethod(k, S("showRooms:"), (IMP)ctl_rooms, "v@:@");
    class_addMethod(k, S("hostRoom:"), (IMP)ctl_host, "v@:@");
    class_addMethod(k, S("askRoom:"), (IMP)ctl_ask_room, "v@:@");
    class_addMethod(k, S("codeRoom:"), (IMP)ctl_code_room, "v@:@");
    class_addMethod(k, S("askCancel:"), (IMP)ctl_ask_cancel, "v@:@");
    class_addMethod(k, S("knockAllow:"), (IMP)ctl_knock_allow, "v@:@");
    class_addMethod(k, S("knockDeny:"), (IMP)ctl_knock_deny, "v@:@");
    class_addMethod(k, S("cardDismiss:"), (IMP)ctl_card_dismiss, "v@:@");
    class_addMethod(k, S("cardAsk:"), (IMP)ctl_card_ask, "v@:@");
    class_addMethod(k, S("cardCode:"), (IMP)ctl_card_code, "v@:@");
    class_addMethod(k, S("userContentController:didReceiveScriptMessage:"), (IMP)ctl_script_message, "v@:@@");
    class_addMethod(k, S("keyboard:"), (IMP)ctl_keyboard, "v@:@");
    class_addMethod(k, S("tapChat:"), (IMP)ctl_tap_video_area, "v@:@");
    add_proto(k, "WKScriptMessageHandler");
    add_proto(k, "UITextFieldDelegate");
    objc_registerClassPair(k);
}

int main(int argc, char **argv)
{
    void *pool = objc_autoreleasePoolPush();
    make_classes();
    G.ctl = alloc_init("WPCtl");
    id delegate = retain(nsstr("WPAppDelegate"));
    objc_autoreleasePoolPop(pool);
    return UIApplicationMain(argc, argv, NULL, delegate);
}
