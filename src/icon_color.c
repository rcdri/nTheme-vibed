/* Icon recoloring, ported from the reference tools (darken.py). Order of the passes:
 *   1. semantic icons (the color is the meaning) get what the draw-time remap does to that color:
 *      dark_remap's black/white swap and the contrast lift (theme_remap), so a swatch matches its line
 *   2. tiles (a mid-tone fill over half the canvas: app tiles, keys) keep their identity color
 *   3. accent: saturated TI-blue pixels move onto the accent, keeping hue offset and relative lightness
 *   4. dark: gray pixels on the icon's own background get dark_flip's lightness curve, except gray
 *      enclosed by color and light glyphs drawn for the (kept) dark title bar; dim colors are lifted
 *   5. an icon that would vanish on the dark background keeps its accented stock pixels */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "icon_color.h"

#define CHROMA_LIMIT 28       /* max-min of RGB; HSL saturation over-reports faint tints on light grays */
#define GLYPH_FILL 0.70       /* a light-only region covering less than this of the canvas is a glyph */
#define EDGE_SHARE 0.4        /* ...if at least this share of its pixels touch transparency */
#define TILE_SHARE 0.5        /* a mid-tone color covering this much of the canvas is a tile */
#define MIN_CONTRAST 3.0
#define HUE_FIRST 190.0       /* TI's UI blues and their tints and shades, in degrees */
#define HUE_LAST 228.0
#define ACCENT_MIN_SAT 0.45   /* below this it is steel-gray shading, not an accent detail */
#define PLOT_HUE_FIRST 229.0  /* the plot blue (#0000FF) family: navy, blue and their tints */
#define PLOT_HUE_LAST 252.0

enum { GRAY = 1, REACH = 2, SEEN = 4, REGION = 8 };

static double linear[256];   /* sRGB channel -> linear light */
static double background_luminance;
static uint8_t expand5[32], expand6[64];
static double opacity[256];   /* alpha / 255 */
static uint8_t reduce5[256], reduce6[256];   /* 8-bit channel -> RGB565 field */
static int16_t flip_shift[256];   /* dark_flip's lightness curve as a shift, by lightness */

static void init_tables(void)
{
	if (linear[255] != 0)
		return;
	for (int v = 0; v < 256; v++) {
		double c = v / 255.0;
		linear[v] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
	}
	background_luminance = 0.2126 * linear[30] + 0.7152 * linear[30] + 0.0722 * linear[30];
	for (int v = 0; v < 256; v++) {
		opacity[v] = v / 255.0;
		reduce5[v] = (uint8_t)((v * 31 + 127) / 255);
		reduce6[v] = (uint8_t)((v * 63 + 127) / 255);
		flip_shift[v] = (int16_t)(229 - (199 * v) / 255 - v);
	}
	for (int v = 0; v < 32; v++)
		expand5[v] = (uint8_t)(v * 255 / 31);
	for (int v = 0; v < 64; v++)
		expand6[v] = (uint8_t)(v * 255 / 63);
}

static double luminance(int r, int g, int b)
{
	return 0.2126 * linear[r] + 0.7152 * linear[g] + 0.0722 * linear[b];
}

static double contrast(double a, double b)
{
	return a > b ? (a + 0.05) / (b + 0.05) : (b + 0.05) / (a + 0.05);
}

static void from565(uint16_t p, int *r, int *g, int *b)
{
	*r = expand5[(p >> 11) & 31];
	*g = expand6[(p >> 5) & 63];
	*b = expand5[p & 31];
}

static uint16_t to565(int r, int g, int b)
{
	return (uint16_t)(reduce5[r] << 11 | reduce6[g] << 5 | reduce5[b]);
}

static int max3(int a, int b, int c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }
static int min3(int a, int b, int c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static double wrap1(double x) { return x - floor(x); }

/* Python's colorsys, so the PC reference and the calculator agree. */
static void rgb_to_hls(double r, double g, double b, double *h, double *l, double *s)
{
	double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	double range = mx - mn;
	*l = (mx + mn) / 2.0;
	if (mx == mn) {
		*h = *s = 0.0;
		return;
	}
	*s = *l <= 0.5 ? range / (mx + mn) : range / (2.0 - mx - mn);
	double rc = (mx - r) / range, gc = (mx - g) / range, bc = (mx - b) / range;
	double hue = r == mx ? bc - gc : g == mx ? 2.0 + rc - bc : 4.0 + gc - rc;
	*h = wrap1(hue / 6.0);
}

static double hls_value(double m1, double m2, double hue)
{
	hue = wrap1(hue);
	if (hue < 1.0 / 6.0)
		return m1 + (m2 - m1) * hue * 6.0;
	if (hue < 0.5)
		return m2;
	if (hue < 2.0 / 3.0)
		return m1 + (m2 - m1) * (2.0 / 3.0 - hue) * 6.0;
	return m1;
}

static void hls_to_rgb8(double h, double l, double s, int *r, int *g, int *b)
{
	double vr, vg, vb;
	if (s == 0.0) {
		vr = vg = vb = l;
	} else {
		double m2 = l <= 0.5 ? l * (1.0 + s) : l + s - l * s;
		double m1 = 2.0 * l - m2;
		vr = hls_value(m1, m2, h + 1.0 / 3.0);
		vg = hls_value(m1, m2, h);
		vb = hls_value(m1, m2, h - 1.0 / 3.0);
	}
	*r = (int)rint(vr * 255);   /* round half to even, like Python's round() */
	*g = (int)rint(vg * 255);
	*b = (int)rint(vb * 255);
}

static void rgb8_to_hls(int r, int g, int b, double *h, double *l, double *s)
{
	rgb_to_hls(r / 255.0, g / 255.0, b / 255.0, h, l, s);
}

/* nTheme's dark_flip() lightness curve, applied additively: dark_flip() scales the channels by f/L,
 * which turns RGB565 near-black such as (8,12,8) bright green. */
static void flip(int *r, int *g, int *b)
{
	int lightness = (max3(*r, *g, *b) + min3(*r, *g, *b)) / 2;
	int d = flip_shift[lightness];
	*r = clamp255(*r + d);
	*g = clamp255(*g + d);
	*b = clamp255(*b + d);
}

/* Lighten (dark mode) or darken (light mode) a color until it reaches MIN_CONTRAST on its background. */
static void reach_contrast(int *r, int *g, int *b, bool lighten)
{
	double target = lighten ? background_luminance : 1.0;
	if (contrast(luminance(*r, *g, *b), target) >= MIN_CONTRAST)
		return;
	double h, l, s;
	rgb8_to_hls(*r, *g, *b, &h, &l, &s);
	while (lighten ? l < 0.95 : l > 0.05) {
		l += lighten ? 0.02 : -0.02;
		hls_to_rgb8(h, l, s, r, g, b);
		if (contrast(luminance(*r, *g, *b), target) >= MIN_CONTRAST)
			return;
	}
}

struct accent {
	unsigned rgb;
	double h, l, s;        /* the accent */
	double hr, lr, sr;     /* the reference blue it replaces */
	double first, last;    /* hue window of that blue's family, degrees */
};

static void accent_init_from(struct accent *a, unsigned rgb, unsigned reference, double first, double last)
{
	a->rgb = rgb;
	a->first = first, a->last = last;
	rgb8_to_hls(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, &a->h, &a->l, &a->s);
	rgb8_to_hls(reference >> 16 & 255, reference >> 8 & 255, reference & 255, &a->hr, &a->lr, &a->sr);
}

static void accent_init(struct accent *a, unsigned rgb)
{
	accent_init_from(a, rgb, ICON_STOCK_ACCENT, HUE_FIRST, HUE_LAST);
}

/* A pixel of the reference blue's family onto the accent: hue offset kept, lightness mapped so the reference
 * lands on the accent (tints stay tints, shades stay shades). */
static bool accent_map(const struct accent *a, int *r, int *g, int *b)
{
	if (!(*b > *r && *b > *g))   /* hues 190-252 need blue strictly on top: skips the float math */
		return false;
	double h, l, s;
	rgb8_to_hls(*r, *g, *b, &h, &l, &s);
	if (!(h * 360 >= a->first && h * 360 <= a->last && s >= ACCENT_MIN_SAT && l > 0.04 && l < 0.97))
		return false;
	double h2 = wrap1(a->h + (h - a->hr));
	double s2 = s * a->s / a->sr;
	if (s2 > 1.0)
		s2 = 1.0;
	double l2 = l <= a->lr ? l * a->l / a->lr : a->l + (l - a->lr) * (1 - a->l) / (1 - a->lr);
	int r2, g2, b2;
	hls_to_rgb8(h2, l2, s2, &r2, &g2, &b2);
	if (r2 == *r && g2 == *g && b2 == *b)
		return false;
	*r = r2, *g = g2, *b = b2;
	return true;
}

static unsigned slot_of(uint16_t color, unsigned slots)
{
	return (color * 40503u >> 3) & (slots - 1);
}

/* Results per RGB565 input: icons share few distinct colors, so the float math runs once per color.
 * A full table just stops caching. */
enum { MEMO_ACCENT, MEMO_CONTRAST, MEMO_PLOT, MEMO_PALETTE };

static bool memo_get(const struct icon_scratch *s, int table, uint16_t in, uint16_t *out)
{
	const struct icon_memo *m = &s->memo[table];
	for (unsigned i = slot_of(in, ICON_MEMO_SLOTS); m->used[i]; i = (i + 1) & (ICON_MEMO_SLOTS - 1))
		if (m->key[i] == in) {
			*out = m->value[i];
			return true;
		}
	return false;
}

static void memo_put(struct icon_scratch *s, int table, uint16_t in, uint16_t out)
{
	struct icon_memo *m = &s->memo[table];
	if (m->filled >= ICON_MEMO_SLOTS * 3 / 4)
		return;
	unsigned i = slot_of(in, ICON_MEMO_SLOTS);
	while (m->used[i])
		i = (i + 1) & (ICON_MEMO_SLOTS - 1);
	m->used[i] = 1, m->key[i] = in, m->value[i] = out;
	m->filled++;
}

static void memo_clear(struct icon_scratch *s)
{
	for (int t = 0; t < ICON_MEMOS; t++) {
		memset(s->memo[t].used, 0, ICON_MEMO_SLOTS);
		s->memo[t].filled = 0;
	}
}

/* Counter for `color` in this icon's tally (a fresh generation per icon, so nothing is ever cleared). */
static uint16_t *tally(struct icon_scratch *s, uint16_t color)
{
	unsigned i = slot_of(color, ICON_TALLY_SLOTS);
	while (s->tally_stamp[i] == s->tally_generation && s->tally_key[i] != color)
		i = (i + 1) & (ICON_TALLY_SLOTS - 1);
	if (s->tally_stamp[i] != s->tally_generation) {
		s->tally_stamp[i] = s->tally_generation;
		s->tally_key[i] = color;
		s->tally_count[i] = 0;
	}
	return &s->tally_count[i];
}

/* Most common opaque color (ties: the one seen first) and its count; false when nothing is opaque. */
static bool top_color(const uint16_t *px, const uint8_t *alpha, int n, struct icon_scratch *s,
                      uint16_t *color, int *count)
{
	if (++s->tally_generation == 0) {
		memset(s->tally_stamp, 0, ICON_TALLY_SLOTS * sizeof *s->tally_stamp);
		s->tally_generation = 1;
	}
	int best = 0;
	for (int i = 0; i < n; i++)
		if (alpha[i] >= 128) {
			uint16_t *c = tally(s, px[i]);
			if (++*c > best)
				best = *c;
		}
	if (best == 0)
		return false;
	for (int i = 0; i < n; i++)
		if (alpha[i] >= 128 && *tally(s, px[i]) == best) {
			*color = px[i];
			break;
		}
	*count = best;
	return true;
}

/* The contrast lift of one RGB565 color, cached. */
static uint16_t lifted(uint16_t p, struct icon_scratch *s)
{
	uint16_t out;
	if (!memo_get(s, MEMO_CONTRAST, p, &out)) {
		int r, g, b;
		from565(p, &r, &g, &b);
		reach_contrast(&r, &g, &b, true);
		out = to565(r, g, b);
		memo_put(s, MEMO_CONTRAST, p, out);
	}
	return out;
}

/* A swatch's color as nTheme draws that color (icon_plot_rgb), cached. `hues`: the graph color picker, whose
 * swatches stand for the graph palette by hue; otherwise only the plot blue family moves. */
static uint16_t plotted(uint16_t p, bool hues, struct icon_scratch *s)
{
	uint16_t out;
	int table = hues ? MEMO_PALETTE : MEMO_PLOT;
	if (!memo_get(s, table, p, &out)) {
		int r, g, b;
		from565(p, &r, &g, &b);
		unsigned rgb = icon_plot_rgb((unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b, s->memo_accent,
		                             s->memo_dark, hues);
		out = to565(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
		memo_put(s, table, p, out);
	}
	return out;
}

static bool is_tile(const uint16_t *px, const uint8_t *alpha, int w, int h, struct icon_scratch *s)
{
	uint16_t color;
	int count, r, g, b;
	if (!top_color(px, alpha, w * h, s, &color, &count))
		return false;
	from565(color, &r, &g, &b);
	int lightness = (max3(r, g, b) + min3(r, g, b)) / 2;
	return lightness >= 70 && lightness <= 190 && (double)count / (w * h) >= TILE_SHARE;
}

/* Whether the icon's most visible 5% of opaque pixels reach `threshold` contrast against the dark
 * background: at least k of them do, k = max(1, opaque/20). No sort, no division per pixel. */
static bool visible(const uint16_t *px, const uint8_t *alpha, int n, double threshold)
{
	double bright = threshold * (background_luminance + 0.05) - 0.05;   /* luminance at or above */
	double dim = (background_luminance + 0.05) / threshold - 0.05;      /* luminance at or below */
	int opaque = 0, passing = 0;
	for (int i = 0; i < n; i++) {
		if (alpha[i] < 128)
			continue;
		opaque++;
		int r, g, b;
		from565(px[i], &r, &g, &b);
		double a = opacity[alpha[i]];
		int cr = (int)(r * a + 30 * (1 - a)), cg = (int)(g * a + 30 * (1 - a)), cb = (int)(b * a + 30 * (1 - a));
		double l = luminance(cr, cg, cb);
		if (l >= bright || l <= dim)
			passing++;
	}
	if (opaque == 0)
		return true;
	int k = opaque / 20 > 1 ? opaque / 20 : 1;
	return passing >= k;
}

static void flood(int w, int h, const uint8_t *alpha, struct icon_scratch *s)
{
	int head = 0, tail = 0;
	uint8_t *f = s->flags;
	for (int y = 0, i = 0; y < h; y++)
		for (int x = 0; x < w; x++, i++) {
			bool border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
			if (alpha[i] < 128 || (border && (f[i] & GRAY))) {
				f[i] |= REACH;
				s->queue[tail++] = i;
			}
		}
	while (head < tail) {
		int i = s->queue[head++], y = i / w, x = i - y * w;
		int next[4] = { y > 0 ? i - w : -1, y < h - 1 ? i + w : -1, x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1 };
		for (int k = 0; k < 4; k++) {
			int j = next[k];
			if (j >= 0 && !(f[j] & REACH) && (f[j] & GRAY)) {
				f[j] |= REACH;
				s->queue[tail++] = j;
			}
		}
	}
}

/* Light gray regions that mostly border transparency and cover little of the canvas are glyphs drawn for
 * the dark title bar or the accent, which nTheme keeps: take them out of REACH so they are left alone. */
static void keep_light_glyphs(int w, int h, const uint8_t *alpha, struct icon_scratch *s)
{
	int n = w * h;
	uint8_t *f = s->flags, *rgb = s->rgb;
	for (int i = 0; i < n; i++) {
		int l = (max3(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]) + min3(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2])) / 2;
		if (alpha[i] >= 128 && (f[i] & GRAY) && (f[i] & REACH) && l >= 128)
			f[i] |= REGION;
	}
	for (int start = 0; start < n; start++) {
		if (!(f[start] & REGION) || (f[start] & SEEN))
			continue;
		int head = 0, tail = 0, edge = 0, brightest = 0;
		s->queue[tail++] = start;
		f[start] |= SEEN;
		while (head < tail) {
			int i = s->queue[head++], y = i / w, x = i - y * w;
			int l = (max3(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]) + min3(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2])) / 2;
			if (l > brightest)
				brightest = l;
			if ((y > 0 && alpha[i - w] < 128) || (y < h - 1 && alpha[i + w] < 128)
			    || (x > 0 && alpha[i - 1] < 128) || (x < w - 1 && alpha[i + 1] < 128))
				edge++;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int xx = x + dx, yy = y + dy;
					if (xx < 0 || yy < 0 || xx >= w || yy >= h)
						continue;
					int j = yy * w + xx;
					if ((f[j] & REGION) && !(f[j] & SEEN)) {
						f[j] |= SEEN;
						s->queue[tail++] = j;
					}
				}
		}
		if (brightest >= 200 && edge >= EDGE_SHARE * tail && tail < GLYPH_FILL * n)
			for (int k = 0; k < tail; k++)
				f[s->queue[k]] &= ~REACH;
	}
}

static void darken(const uint16_t *px, const uint8_t *alpha, int w, int h, bool semantic, bool palette,
                   uint16_t *out, struct icon_scratch *s)
{
	int n = w * h;
	if (semantic) {
		uint16_t color;
		int count;
		/* A black series behaves like a flip (anti-aliasing included); any other color stays as drawn. */
		if (!top_color(px, alpha, n, s, &color, &count) || color != 0x0000) {
			for (int i = 0; i < n; i++) {
				int r, g, b;
				from565(px[i], &r, &g, &b);
				if (px[i] == 0x0000)
					out[i] = to565(229, 229, 229);
				else if (px[i] == 0xFFFF)
					out[i] = to565(30, 30, 30);
				else if (alpha[i] && max3(r, g, b) - min3(r, g, b) > CHROMA_LIMIT)
					out[i] = plotted(px[i], palette, s);
				else
					out[i] = px[i];
			}
			return;
		}
	}
	if (is_tile(px, alpha, w, h, s)) {
		memcpy(out, px, n * sizeof *out);
		return;
	}
	for (int i = 0; i < n; i++) {
		int r, g, b;
		from565(px[i], &r, &g, &b);
		s->rgb[3 * i] = (uint8_t)r, s->rgb[3 * i + 1] = (uint8_t)g, s->rgb[3 * i + 2] = (uint8_t)b;
		s->flags[i] = max3(r, g, b) - min3(r, g, b) <= CHROMA_LIMIT ? GRAY : 0;
	}
	flood(w, h, alpha, s);
	keep_light_glyphs(w, h, alpha, s);
	for (int i = 0; i < n; i++) {
		if (alpha[i] == 0) {
			out[i] = px[i];
			continue;
		}
		int r = s->rgb[3 * i], g = s->rgb[3 * i + 1], b = s->rgb[3 * i + 2];
		if (s->flags[i] & GRAY) {
			if (s->flags[i] & REACH)
				flip(&r, &g, &b);
		} else {
			out[i] = semantic ? plotted(px[i], palette, s) : lifted(px[i], s);
			continue;
		}
		out[i] = to565(r, g, b);
	}
	if (!visible(out, alpha, n, 1.4) && visible(px, alpha, n, 2.0))
		memcpy(out, px, n * sizeof *out);
}

void icon_recolor(const uint16_t *stock, const uint8_t *alpha, int w, int h, unsigned flags,
                  unsigned accent_rgb, bool dark, uint16_t *out, struct icon_scratch *s)
{
	int n = w * h;
	init_tables();
	if (s->memo_accent != accent_rgb || s->memo_dark != dark) {   /* cached answers depend on both */
		memo_clear(s);
		s->memo_accent = accent_rgb;
		s->memo_dark = dark;
	}
	if (flags & ICON_SEMANTIC) {
		if (dark) {
			darken(stock, alpha, w, h, true, flags & ICON_PALETTE, out, s);
		} else {
			for (int i = 0; i < n; i++) {
				int r, g, b;
				from565(stock[i], &r, &g, &b);
				out[i] = alpha[i] && max3(r, g, b) - min3(r, g, b) > CHROMA_LIMIT ? plotted(stock[i], flags & ICON_PALETTE, s) : stock[i];
			}
		}
		return;
	}
	if (is_tile(stock, alpha, w, h, s)) {
		memcpy(out, stock, n * sizeof *out);
		return;
	}
	uint16_t *accented = dark ? s->accented : out;
	memcpy(accented, stock, n * sizeof *accented);
	if (accent_rgb != ICON_STOCK_ACCENT) {
		struct accent a;
		accent_init(&a, accent_rgb);
		for (int i = 0; i < n; i++) {
			if (alpha[i] == 0)
				continue;
			uint16_t mapped;
			if (!memo_get(s, MEMO_ACCENT, stock[i], &mapped)) {
				int r, g, b;
				from565(stock[i], &r, &g, &b);
				mapped = stock[i];
				if (accent_map(&a, &r, &g, &b)) {
					if (!dark)
						reach_contrast(&r, &g, &b, false);
					mapped = to565(r, g, b);
				}
				memo_put(s, MEMO_ACCENT, stock[i], mapped);
			}
			accented[i] = mapped;
		}
	}
	if (dark)
		darken(accented, alpha, w, h, false, false, out, s);
}

/* Every block stays small: the OS heap hands out chunks of a few hundred KB at most, fragmented. */
/* The default graph colors (f1..f6 as the OS assigns them) and where each one goes relative to the accent:
 * related hues with different lightness, so curves look like the accent and stay apart from each other. */
static const struct plot_slot {
	unsigned reference;      /* the OS's color for that function (one off from the plain color on purpose) */
	double first, last;      /* its hue family in degrees, for 3D shading and swatches; first > last wraps */
	double hue_offset;       /* degrees from the accent */
	double light_delta;      /* lightness from the accent's */
} plot_slots[] = {
	{ 0x0100FF, 229, 252,   0,  0.00 },   /* f1 blue -> the accent; navy and tints follow */
	{ 0xFE0000, 350,  10,  30,  0.12 },   /* f2 red */
	{ 0x010000,  -1,  -1,   0, -0.22 },   /* f3 black: exact only */
	{ 0xFE00FF, 290, 310, -30,  0.12 },   /* f4 magenta */
	{ 0x018000, 105, 135,  60, -0.06 },   /* f5 green */
	{ 0xFE9933,  24,  38, -60,  0.02 },   /* f6 orange */
};
#define PLOT_SLOTS (sizeof plot_slots / sizeof plot_slots[0])

static double clamp_unit(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Map `r,g,b` from slot `k`'s family onto its accent-derived color, like accent_map(). */
static bool plot_slot_map(unsigned k, unsigned accent_rgb, bool hues, int *r, int *g, int *b)
{
	const struct plot_slot *p = &plot_slots[k];
	unsigned rgb = (unsigned)*r << 16 | (unsigned)*g << 8 | (unsigned)*b;
	double ha, la, sa, hr, lr, sr, h, l, s;
	rgb8_to_hls(accent_rgb >> 16 & 255, accent_rgb >> 8 & 255, accent_rgb & 255, &ha, &la, &sa);
	double ht = wrap1(ha + p->hue_offset / 360.0), lt = clamp_unit(la + p->light_delta, 0.18, 0.82);
	double st = sa > 0.55 ? sa : 0.55;
	if (rgb == p->reference) {
		hls_to_rgb8(ht, lt, st, r, g, b);
		return true;
	}
	if (!hues || p->first < 0)
		return false;
	rgb8_to_hls(*r, *g, *b, &h, &l, &s);
	double deg = h * 360;
	bool inside = p->first <= p->last ? deg >= p->first && deg <= p->last : deg >= p->first || deg <= p->last;
	if (!inside || s < ACCENT_MIN_SAT || l <= 0.04 || l >= 0.97)
		return false;
	rgb8_to_hls(p->reference >> 16 & 255, p->reference >> 8 & 255, p->reference & 255, &hr, &lr, &sr);
	double dh = h - hr;
	if (dh > 0.5) dh -= 1.0;
	if (dh < -0.5) dh += 1.0;
	double h2 = wrap1(ht + dh), s2 = s * st / sr;
	if (s2 > 1.0)
		s2 = 1.0;
	double l2 = l <= lr ? l * lt / lr : lt + (l - lr) * (1 - lt) / (1 - lr);
	int r2, g2, b2;
	hls_to_rgb8(h2, l2, s2, &r2, &g2, &b2);
	if (r2 == *r && g2 == *g && b2 == *b)
		return false;
	*r = r2, *g = g2, *b = b2;
	return true;
}

/* A graph color as nTheme draws it. With an accent other than the stock blue, the default graph colors move to
 * the accent's palette: by exact value (2D drawing, where the same hues also mean other things) or by hue family
 * (`hues`: 3D shading, color picker swatches); the plot blue always by family. Then readable on the background:
 * lifted to 3:1 on the dark background, darkened to 3:1 on white when it was moved. */
unsigned icon_plot_rgb(unsigned rgb, unsigned accent_rgb, bool dark, bool hues)
{
	init_tables();
	int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
	bool moved = false;
	if (accent_rgb != ICON_STOCK_ACCENT)
		for (unsigned k = 0; k < PLOT_SLOTS && !moved; k++)
			moved = plot_slot_map(k, accent_rgb, hues || k == 0, &r, &g, &b);
	if (dark)
		reach_contrast(&r, &g, &b, true);
	else if (moved)
		reach_contrast(&r, &g, &b, false);
	return (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
}

bool icon_is_graph_color(unsigned rgb)
{
	for (unsigned k = 0; k < PLOT_SLOTS; k++)
		if (plot_slots[k].reference == rgb)
			return true;
	return false;
}

unsigned icon_lift_rgb(unsigned rgb)
{
	init_tables();
	int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
	reach_contrast(&r, &g, &b, true);
	return (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
}

bool icon_scratch_init(struct icon_scratch *s, int max_pixels)
{
	memset(s, 0, sizeof *s);
	if (max_pixels > ICON_TALLY_SLOTS * 3 / 4)
		return false;
	s->capacity = max_pixels;
	s->rgb = malloc(3 * max_pixels);
	s->flags = malloc(max_pixels);
	s->queue = malloc(max_pixels * sizeof *s->queue);
	s->accented = malloc(max_pixels * sizeof *s->accented);
	s->tally_key = malloc(ICON_TALLY_SLOTS * sizeof *s->tally_key);
	s->tally_count = malloc(ICON_TALLY_SLOTS * sizeof *s->tally_count);
	s->tally_stamp = calloc(ICON_TALLY_SLOTS, sizeof *s->tally_stamp);
	bool ok = s->rgb && s->flags && s->queue && s->accented && s->tally_key && s->tally_count && s->tally_stamp;
	for (int t = 0; t < ICON_MEMOS; t++) {
		s->memo[t].key = malloc(ICON_MEMO_SLOTS * sizeof *s->memo[t].key);
		s->memo[t].value = malloc(ICON_MEMO_SLOTS * sizeof *s->memo[t].value);
		s->memo[t].used = calloc(ICON_MEMO_SLOTS, 1);
		ok = ok && s->memo[t].key && s->memo[t].value && s->memo[t].used;
	}
	s->memo_accent = ICON_STOCK_ACCENT;
	s->memo_dark = false;
	if (ok)
		return true;
	icon_scratch_free(s);
	return false;
}

void icon_scratch_free(struct icon_scratch *s)
{
	free(s->rgb);
	free(s->flags);
	free(s->queue);
	free(s->accented);
	free(s->tally_key);
	free(s->tally_count);
	free(s->tally_stamp);
	for (int t = 0; t < ICON_MEMOS; t++) {
		free(s->memo[t].key);
		free(s->memo[t].value);
		free(s->memo[t].used);
	}
	memset(s, 0, sizeof *s);
}
