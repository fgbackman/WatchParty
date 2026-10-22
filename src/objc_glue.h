/* objc_glue.h — talking to Cocoa from plain C through the Objective-C runtime.
 *
 * Every message send is cast to the exact non-variadic C prototype of the
 * method being called. (Calling objc_msgSend through a variadic prototype is
 * wrong on arm64, where variadic arguments travel on the stack.) */
#ifndef OBJC_GLUE_H
#define OBJC_GLUE_H

#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>

typedef struct { unsigned long location, length; } WPRange;   /* NSRange */

#define S(name) sel_registerName(name)
#define C(name) ((id)objc_getClass(name))

/* MSG(ret, argtypes...)(receiver, selector, args...) */
#define MSG(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)

/* Struct returns larger than 16 bytes (NSRect) need the _stret entry on x86_64;
 * arm64 has no such variant. */
#if defined(__x86_64__)
#define MSG_STRET(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend_stret)
#else
#define MSG_STRET(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)
#endif

/* Type encodings for methods we add at runtime. */
#if defined(__arm64__) || defined(__aarch64__)
#define ENC_BOOL "B"
#else
#define ENC_BOOL "c"
#endif

static inline id nsstr(const char *s)
{
    return MSG(id, const char *)(C("NSString"), S("stringWithUTF8String:"), s ? s : "");
}

static inline const char *cstr(id ns)
{
    if (!ns) return "";
    const char *s = MSG(const char *)(ns, S("UTF8String"));
    return s ? s : "";
}

static inline id alloc_init(const char *cls)
{
    return MSG(id)(MSG(id)(C(cls), S("alloc")), S("init"));
}

static inline id retain(id o) { return o ? MSG(id)(o, S("retain")) : o; }
static inline void release(id o) { if (o) MSG(void)(o, S("release")); }

static inline CGRect rect(double x, double y, double w, double h)
{
    CGRect r = {{x, y}, {w, h}};
    return r;
}

static inline bool responds(id obj, const char *sel)
{
    return obj && MSG(BOOL, SEL)(obj, S("respondsToSelector:"), S(sel));
}

#endif
