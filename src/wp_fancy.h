/* wp_fancy.h — the "fancy words" filter: every cuss word becomes the biggest
 * word we could find that starts with the same letter, in the same form
 * ("fucking" -> "flabbergasting"); slurs become compliments ("dyke" -> "dynamo").
 *
 * Catches stretched letters (fuuuck), leetspeak (sh1t, a$$, b!tch), masked
 * letters (f*ck, s**t), any capitalisation, and f-/s-/b-words buried inside
 * other words (absofuckinglutely). Links are left untouched. Pure C. */
#ifndef WP_FANCY_H
#define WP_FANCY_H

#include <stddef.h>

/* Writes the filtered text to out (always NUL-terminated). Returns the number of
 * words replaced. in and out must not overlap. */
int wp_fancy(const char *in, char *out, size_t cap);

#endif
