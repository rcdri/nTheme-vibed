/* The OS's color icons (iconsColor.res of every component), recolored live for dark mode and the accent.
 * The OS keeps them in a heap registry of {fourcc, count, offset table, data} entries and draws straight
 * from that data, so rewriting the pixel planes shows up at the next redraw. */
#ifndef NTHEME_ICONS_H
#define NTHEME_ICONS_H

#include <stdbool.h>
#include "theme.h"

/* Recolor every icon from its stock pixels. With `defer` the work waits for the first home screen paint
 * (memory is tight while startup programs run). Failures are kept for icons_take_error(). */
void icons_apply(enum theme_mode mode, unsigned accent_rgb, bool defer);

/* Hook side (home painter): runs a deferred icons_apply. */
void icons_run_deferred(void);

/* Why recoloring did not happen, once; NULL when it did. */
const char *icons_take_error(void);

#endif
