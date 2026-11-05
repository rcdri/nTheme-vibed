#include <stdlib.h>
#include "theme.h"
#include "dark.h"
#include "icon_color.h"

/* 2D math editor default config: text, background, cursor, and two more colors. Verified osid 48. */
static const unsigned editor_fields[] = { 0x34, 0x38, 0x44, 0x84, 0xa0 };
#define EDITOR_FIELD_COUNT (sizeof editor_fields / sizeof editor_fields[0])
#define EDITOR_COUNT 3

#define DARK_GRID 0x505050
#define ACCENT_TEXT_ON_LIGHT 0x101010
#define LIGHT_ACCENT_LUMINANCE 170   /* above this, white text on the accent is unreadable */

#define THEMED_MAX 640
#define LIFT_CACHE 256

static const struct os_table *os;
/* Every color the theme itself sets, sorted: the draw-time remap leaves these alone. */
static unsigned themed[THEMED_MAX];
static unsigned themed_count;
static struct { unsigned in, out; } lift_cache[LIFT_CACHE];
static unsigned accent = ICON_STOCK_ACCENT;
#define VIEW_CACHE 2048
static struct { uint16_t in, out; } view_cache[VIEW_CACHE];   /* in == out == 0 counts as empty */
static unsigned *stock;
static unsigned stock_editor[EDITOR_COUNT][EDITOR_FIELD_COUNT];
static unsigned stock_grid;
static volatile bool dark;

static volatile unsigned *table(void)
{
	return (volatile unsigned *)os->color_table;
}

static volatile unsigned *editor_field(int editor, unsigned field)
{
	return (volatile unsigned *)(os->editor_config[editor] + editor_fields[field]);
}

static volatile unsigned *plot_grid(void)
{
	return (volatile unsigned *)os->plot_palette + os->ids.plot_grid_index;
}

bool theme_is_stock(const struct os_table *t)
{
	volatile unsigned *editor = (volatile unsigned *)t->editor_config[0];
	return ((volatile unsigned *)t->color_table)[t->ids.home_section_title] == 0xFFFFFF
	    && editor[editor_fields[0] / 4] == 1 && editor[editor_fields[1] / 4] == 0xFFFFFF;
}

bool theme_snapshot(const struct os_table *t)
{
	os = t;
	stock = malloc(os->color_count * sizeof *stock);
	if (!stock)
		return false;
	for (unsigned i = 0; i < os->color_count; i++)
		stock[i] = table()[i];
	for (int e = 0; e < EDITOR_COUNT; e++)
		for (unsigned f = 0; f < EDITOR_FIELD_COUNT; f++)
			stock_editor[e][f] = *editor_field(e, f);
	stock_grid = *plot_grid();
	return true;
}

static bool keep_in_dark(unsigned index)
{
	const struct theme_ids *ids = &os->ids;
	if (index >= ids->keep_first && index <= ids->keep_last)
		return true;
	for (unsigned k = 0; k < ids->keep_count; k++)
		if (ids->keep[k] == index)
			return true;
	return false;
}

static bool is_light(unsigned rgb)
{
	unsigned luminance = (299 * ((rgb >> 16) & 255) + 587 * ((rgb >> 8) & 255) + 114 * (rgb & 255)) / 1000;
	return luminance > LIGHT_ACCENT_LUMINANCE;
}

static void apply_table(bool dark_mode, unsigned accent_rgb)
{
	const struct theme_ids *ids = &os->ids;
	for (unsigned i = 0; i < os->color_count; i++) {
		unsigned v = stock[i];
		table()[i] = !dark_mode ? v : keep_in_dark(i) ? dark_nudge(v) : dark_flip(v);
	}
	if (dark_mode)
		for (unsigned k = 0; k < ids->dim_count; k++)
			table()[ids->dim[k]] = dark_dim(stock[ids->dim[k]]);
	for (unsigned i = 0; i < os->color_count; i++)
		if (stock[i] == ids->stock_accent)
			table()[i] = accent_rgb;
	if (is_light(accent_rgb))
		for (unsigned k = 0; k < ids->accent_text_count; k++)
			table()[ids->accent_text[k]] = ACCENT_TEXT_ON_LIGHT;
}

static void apply_editors(bool dark_mode)
{
	for (int e = 0; e < EDITOR_COUNT; e++)
		for (unsigned f = 0; f < EDITOR_FIELD_COUNT; f++) {
			unsigned v = stock_editor[e][f];
			if (dark_mode)
				v = dark_flip(v == 1 ? 0 : v);
			*editor_field(e, f) = v;
		}
}

static int compare_unsigned(const void *a, const void *b)
{
	unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
	return x < y ? -1 : x > y;
}

static void add_themed(unsigned rgb)
{
	if (themed_count < THEMED_MAX)
		themed[themed_count++] = rgb & 0xFFFFFF;
}

static void collect_themed(void)
{
	themed_count = 0;
	for (unsigned i = 0; i < os->color_count; i++)
		add_themed(table()[i]);
	for (int e = 0; e < EDITOR_COUNT; e++)
		for (unsigned f = 0; f < EDITOR_FIELD_COUNT; f++)
			add_themed(*editor_field(e, f));
	add_themed(*plot_grid());
	qsort(themed, themed_count, sizeof *themed, compare_unsigned);
	for (unsigned k = 0; k < LIFT_CACHE; k++)
		lift_cache[k].in = 0xFFFFFFFF;
	for (unsigned k = 0; k < VIEW_CACHE; k++)
		view_cache[k].in = view_cache[k].out = 0;
}

static bool is_themed(unsigned rgb)
{
	unsigned lo = 0, hi = themed_count;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		if (themed[mid] < rgb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < themed_count && themed[lo] == rgb;
}

void theme_apply(enum theme_mode mode, unsigned accent_rgb)
{
	bool dark_mode = mode == THEME_DARK;
	dark = false;   /* the hook keeps stock behavior while the set below is rebuilt */
	accent = ICON_STOCK_ACCENT;
	apply_table(dark_mode, accent_rgb);
	apply_editors(dark_mode);
	*plot_grid() = dark_mode ? DARK_GRID : stock_grid;
	collect_themed();
	accent = accent_rgb;
	dark = dark_mode;
}

unsigned theme_color(unsigned id)
{
	return table()[id];
}

static unsigned cached_plot(unsigned rgb)
{
	unsigned slot = (rgb * 2654435761u) >> 24;
	if (lift_cache[slot].in != rgb) {
		lift_cache[slot].in = rgb;
		lift_cache[slot].out = icon_plot_rgb(rgb, accent, dark, false);
	}
	return lift_cache[slot].out;
}

/* Colors the OS hard-codes instead of taking from the theme table. Dark mode: exact white and black (dark_remap),
 * light grays get the table's lightness flip (lined grid, Data & Statistics bars, Vernier DataQuest). Both modes:
 * graph colors (icon_plot_rgb) - the default function colors take an accent-derived palette, and in dark mode
 * anything too dark to read on the background is lifted to 3:1. */
unsigned theme_remap(unsigned rgb)
{
	if (!dark && accent == ICON_STOCK_ACCENT)
		return rgb;
	if (dark) {
		unsigned remapped = dark_remap(rgb);
		if (remapped != rgb)
			return remapped;
	}
	if (is_themed(rgb))
		return rgb;
	if (accent != ICON_STOCK_ACCENT && icon_is_graph_color(rgb))   /* f1..f6 take the accent palette */
		return cached_plot(rgb);
	if (dark && dark_is_near_black(rgb))   /* e.g. the "black" graph color, #010000: black, not a red to lift */
		return DARK_FOREGROUND;
	if (dark && dark_is_light_gray(rgb))
		return dark_flip_gray(rgb);
	return cached_plot(rgb);
}

/* One RGB565 pixel of a view the OS renders itself (3D graphing): grays flip in dark mode (white background,
 * black lines and labels), colors take the graph color rules above. */
static uint16_t view_pixel(uint16_t p)
{
	unsigned slot = (p * 40503u >> 5) & (VIEW_CACHE - 1);
	if (view_cache[slot].in == p && (p || view_cache[slot].out))
		return view_cache[slot].out;
	int r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
	unsigned rgb = (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
	int max = r > g ? (r > b ? r : b) : (g > b ? g : b), min = r < g ? (r < b ? r : b) : (g < b ? g : b);
	if (max - min <= DARK_GRAY_CHROMA)
		rgb = dark ? dark_flip_gray(rgb) : rgb;
	else
		rgb = icon_plot_rgb(rgb, accent, dark, true);
	r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
	uint16_t out = (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
	view_cache[slot].in = p;
	view_cache[slot].out = out;
	return out;
}

/* The OS's drawing surface, as a gc points at it (verified OS 6.4, CX II-T). */
struct surface {
	int x, y, w, h;          /* clip rectangle on screen */
	int unknown[4];
	uint16_t *origin;        /* pixel (x, y) */
	unsigned pitch;          /* pixels per row */
	uint16_t *base;
	unsigned pitch_again, height;
};

void theme_recolor_view(const void *gc, int width, int height)
{
	if ((!dark && accent == ICON_STOCK_ACCENT) || !os->gc_surface)
		return;
	const struct surface *s = *(const struct surface *const *)((const char *)gc + os->gc_surface);
	/* Only a surface that is exactly the view, inside a screen-sized buffer, is touched. */
	if ((unsigned)s < 0x10000000u || (unsigned)s >= 0x14000000u || s->pitch != 320 || s->height != 240
	    || s->w != width || s->h != height || width <= 0 || height <= 0 || s->x < 0 || s->y < 0
	    || s->x + width > 320 || s->y + height > 240 || s->origin != s->base + s->y * 320 + s->x)
		return;
	for (int y = 0; y < height; y++) {
		uint16_t *row = s->origin + y * 320;
		for (int x = 0; x < width; x++)
			row[x] = view_pixel(row[x]);
	}
}
