/* Dark mode color math. Pure functions on 0x00RRGGBB values. */
#ifndef NTHEME_DARK_H
#define NTHEME_DARK_H

#include <stdbool.h>

#define DARK_BACKGROUND 0x1E1E1E
#define DARK_FOREGROUND 0xE5E5E5
#define DARK_LIGHT_GRAY 160   /* hard-coded grays at least this light are flipped */
#define DARK_GRAY_CHROMA 24

/* Invert the lightness of a near-gray color; saturated colors pass through unchanged. */
unsigned dark_flip(unsigned rgb);

/* Scale a light saturated background down so flipped black text stays readable on it. */
unsigned dark_dim(unsigned rgb);

/* Exact white/black become #FEFEFE/#010101: identical on the RGB565 panel, but distinguishable
 * from the hard-coded white/black that dark_remap() rewrites. */
unsigned dark_nudge(unsigned rgb);

/* Hard-coded OS colors: (near-)white becomes the dark background, (near-)black the light foreground. */
unsigned dark_remap(unsigned rgb);

/* Every channel at most DARK_NEAR_BLACK: drawn as black (DARK_FOREGROUND), whatever faint tint it has. */
#define DARK_NEAR_BLACK 0x10
bool dark_is_near_black(unsigned rgb);

/* A hard-coded light gray (lined grid, Data & Statistics bars, DataQuest panels). */
bool dark_is_light_gray(unsigned rgb);

/* dark_flip()'s lightness curve applied as a shift, for grays with a faint tint (which dark_flip() passes through
 * as saturated) and without scaling up the tint. */
unsigned dark_flip_gray(unsigned rgb);

#endif
