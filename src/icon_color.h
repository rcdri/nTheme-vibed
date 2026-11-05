/* Icon recoloring. Pure functions on RGB565 pixel planes, testable on a PC (utils/iconsim.c).
 * Mirrors what dark.c and theme.c do to the color table, adapted for anti-aliased bitmaps. */
#ifndef NTHEME_ICON_COLOR_H
#define NTHEME_ICON_COLOR_H

#include <stdbool.h>
#include <stdint.h>

#define ICON_STOCK_ACCENT 0x2478CF   /* the OS's UI blue, ACCENT_RGB[ACCENT_DEFAULT] */
#define ICON_PLOT_BLUE 0x0000FF      /* the default graph color, which follows the accent */

enum {
	ICON_SEMANTIC = 1,   /* the color is the meaning (plot swatch/marker, color picker): draw-time remap only */
	ICON_PALETTE = 2,    /* with ICON_SEMANTIC: a graph color picker swatch, follows the whole accent palette */
};

#define ICON_TALLY_SLOTS 16384   /* color counters per icon; icons have at most 3/4 of this many pixels */
#define ICON_MEMO_SLOTS 8192     /* distinct colors whose recolor result is cached per apply */
#define ICON_MEMOS 4

struct icon_memo {
	uint16_t *key, *value;
	uint8_t *used;
	int filled;
};

/* Working memory for icons of up to `max_pixels` pixels, kept in small blocks. */
struct icon_scratch {
	int capacity;
	uint8_t *rgb;        /* 3 bytes per pixel */
	uint8_t *flags;
	int *queue;
	uint16_t *accented;  /* the accent pass's output, the dark pass's input */
	uint16_t *tally_key, *tally_count, *tally_stamp, tally_generation;
	struct icon_memo memo[ICON_MEMOS];   /* accent, contrast lift, plot color, palette color results */
	unsigned memo_accent;
	bool memo_dark;
};

bool icon_scratch_init(struct icon_scratch *s, int max_pixels);

/* The dark-mode contrast lift on its own: `rgb` lightened until it reads at 3:1 on #1E1E1E (unchanged if it
 * already does). The draw-time remap uses it for hard-coded colors, so drawn lines match their swatch icons. */
unsigned icon_lift_rgb(unsigned rgb);

/* A graph color as nTheme draws it: with a non-blue accent the default graph colors (f1..f6) move to an
 * accent-derived palette - by exact value, or by hue family with `hues` (3D shading, swatches) - then the color is
 * made readable on the background (3:1 lifted in dark mode, darkened on white when moved in light mode). */
unsigned icon_plot_rgb(unsigned rgb, unsigned accent_rgb, bool dark, bool hues);

/* One of the OS's default graph colors (#0100FF, #FE0000, #010000, #FE00FF, #018000, #FE9933). */
bool icon_is_graph_color(unsigned rgb);
void icon_scratch_free(struct icon_scratch *s);

/* Recolor one icon from its stock pixels into `out` (may not alias `stock`). Alpha is never changed.
 * Light mode with the stock accent reproduces `stock` exactly. */
void icon_recolor(const uint16_t *stock, const uint8_t *alpha, int w, int h, unsigned flags,
                  unsigned accent_rgb, bool dark, uint16_t *out, struct icon_scratch *s);

#endif
