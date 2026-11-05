#include <os.h>
#include <stdlib.h>
#include <string.h>
#include "icons.h"
#include "icon_color.h"

#define RAM_BEGIN 0x10000000u
#define RAM_END 0x14000000u
#define FOURCC(a, b, c, d) ((unsigned)(a) << 24 | (unsigned)(b) << 16 | (unsigned)(c) << 8 | (unsigned)(d))
#define MAX_SIDE 512
#define DEFERRED_TRIES 8   /* home paints to retry on, while startup memory is still tight */
#define STOCK_BLOCK 65536  /* the stock copy is packed into blocks of at most this: few, modest allocations */

/* The registry's components, in its order (OS 6.4 phoenix/components). Entries are 16 bytes apart. */
static const unsigned component_ids[] = {
	FOURCC('a','c','b','r'), FOURCC('c','l','n','k'), FOURCC('c','t','l','g'), FOURCC('d','c','o','l'),
	FOURCC('d','l','o','g'), FOURCC('d','t','s','t'), FOURCC('g','e','o','g'), FOURCC('i','n','s','p'),
	FOURCC('m','a','t','h'), FOURCC('m','w','i','z'), FOURCC('n','t','p','d'), FOURCC('p','g','e','d'),
	FOURCC('p','y','e','d'), FOURCC('q','c','k','p'), FOURCC('q','u','e','s'), FOURCC('s','c','p','d'),
	FOURCC('s','r','p','t'), FOURCC('s','y','s','t'), FOURCC('t','b','l','t'),
};
#define COMPONENTS (sizeof component_ids / sizeof component_ids[0])

struct registry_entry {   /* as the OS lays it out, 16 bytes */
	unsigned id, count;
	const unsigned *offsets;
	unsigned char *data;
};

/* Our copy. Deliberately not 16 bytes with the fourcc first: a stale copy on the stack must never look
 * like the registry to find_registry(). */
struct entry {
	unsigned count;
	const unsigned *offsets;
	unsigned char *data;
};

struct icon_header {      /* iconsColor.res; pixels and alpha follow */
	unsigned width, height, zero, pitch;
	unsigned short bpp, has_alpha;
};

struct component {
	unsigned char *data;  /* what the stock copy below was taken from */
	unsigned count;
	uint16_t **stock;     /* per icon, pointing into blocks */
	void **blocks;
	unsigned block_count;
};

static volatile struct registry_entry *registry;
static struct component components[COMPONENTS];
static struct { bool pending; int tries_left; enum theme_mode mode; unsigned accent; } deferred;
static char error[160];

static bool in_ram(const void *p, unsigned size)
{
	unsigned a = (unsigned)p;
	return a >= RAM_BEGIN && a < RAM_END && size <= RAM_END - a;
}

static volatile struct registry_entry *find_registry(void)
{
	for (unsigned a = RAM_BEGIN; a + 16 * COMPONENTS <= RAM_END; a += 4) {
		volatile const unsigned *w = (volatile const unsigned *)a;
		if (w[0] != component_ids[0] || w[4] != component_ids[1])
			continue;
		unsigned k = 2;
		while (k < COMPONENTS && w[4 * k] == component_ids[k])
			k++;
		if (k == COMPONENTS)
			return (volatile struct registry_entry *)a;
	}
	return NULL;
}

static const struct icon_header *icon_at(const struct entry *e, unsigned i)
{
	return (const struct icon_header *)(e->data + e->offsets[i]);
}

/* Every icon header where the format says it is, inside RAM, in order: only then is the entry written to. */
static bool entry_valid(const struct entry *e)
{
	if (!e->count || e->count > 4096 || !in_ram(e->offsets, 4 * e->count) || !in_ram(e->data, 20))
		return false;
	unsigned end = 0;
	for (unsigned i = 0; i < e->count; i++) {
		if (e->offsets[i] < end)
			return false;
		const struct icon_header *h = icon_at(e, i);
		if (!in_ram(h, 20) || h->width - 1 >= MAX_SIDE || h->height - 1 >= MAX_SIDE || h->zero
		    || h->pitch != 2 * h->width || h->bpp != 16 || h->has_alpha != 1)
			return false;
		unsigned size = 20 + 3 * h->width * h->height;
		if (!in_ram(h, size))
			return false;
		end = e->offsets[i] + size;
	}
	return true;
}

/* Icons whose color is the meaning: color picker swatches, Vernier DataQuest plot swatches and markers.
 * Only trusted at the OS 6.4 icon counts they were read from. */
static unsigned icon_flags(unsigned id, const struct entry *e, unsigned i)
{
	if (id == FOURCC('s','y','s','t') && e->count == 470)
		return i >= 389 && i <= 405 ? ICON_SEMANTIC | ICON_PALETTE : 0;
	if (id == FOURCC('d','c','o','l') && e->count == 495)
		return (i >= 8 && i <= 22) || (i >= 107 && i <= 483) ? ICON_SEMANTIC : 0;
	return 0;
}

static void drop_stock(struct component *c)
{
	for (unsigned k = 0; k < c->block_count; k++)
		free(c->blocks[k]);
	free(c->blocks);
	free(c->stock);
	memset(c, 0, sizeof *c);
}

static unsigned plane_bytes(const struct entry *e, unsigned i)
{
	const struct icon_header *h = icon_at(e, i);
	return 2 * h->width * h->height;
}

/* Copy the stock pixels the first time an entry is seen (and again if the OS reloaded it). */
static bool take_stock(struct component *c, const struct entry *e)
{
	if (c->stock && c->data == e->data && c->count == e->count)
		return true;
	drop_stock(c);
	unsigned blocks = 1, used = 0;
	for (unsigned i = 0; i < e->count; i++) {
		unsigned bytes = plane_bytes(e, i);
		if (used + bytes > STOCK_BLOCK)
			blocks++, used = 0;
		used += bytes;
	}
	c->stock = calloc(e->count, sizeof *c->stock);
	c->blocks = calloc(blocks, sizeof *c->blocks);
	if (!c->stock || !c->blocks) {
		drop_stock(c);
		return false;
	}
	unsigned char *block = NULL;
	used = STOCK_BLOCK;
	for (unsigned i = 0; i < e->count; i++) {
		unsigned bytes = plane_bytes(e, i);
		if (used + bytes > STOCK_BLOCK) {
			unsigned size = 0;   /* this block: icons from i on that fit */
			for (unsigned j = i; j < e->count && size + plane_bytes(e, j) <= STOCK_BLOCK; j++)
				size += plane_bytes(e, j);
			block = malloc(size);
			if (!block) {
				drop_stock(c);
				return false;
			}
			c->blocks[c->block_count++] = block;
			used = 0;
		}
		c->stock[i] = (uint16_t *)(block + used);
		memcpy(c->stock[i], icon_at(e, i) + 1, bytes);
		used += bytes;
	}
	c->count = e->count;
	c->data = e->data;
	return true;
}

static bool recolor(enum theme_mode mode, unsigned accent, bool *no_memory)
{
	*no_memory = false;
	if (!registry || registry[0].id != component_ids[0])
		registry = find_registry();
	if (!registry) {
		snprintf(error, sizeof error, "The OS icon table was not found, so icons keep their stock colors.");
		return false;
	}
	struct entry entries[COMPONENTS];
	int max_pixels = 0;
	for (unsigned k = 0; k < COMPONENTS; k++) {
		entries[k].count = registry[k].count;
		entries[k].offsets = registry[k].offsets;
		entries[k].data = registry[k].data;
		if (!entry_valid(&entries[k])) {
			entries[k].count = 0;
			continue;
		}
		for (unsigned i = 0; i < entries[k].count; i++) {
			const struct icon_header *h = icon_at(&entries[k], i);
			if ((int)(h->width * h->height) > max_pixels)
				max_pixels = (int)(h->width * h->height);
		}
	}
	if (max_pixels == 0) {
		snprintf(error, sizeof error, "The OS icon table has an unexpected layout, so icons keep their stock colors.");
		return false;
	}
	struct icon_scratch scratch;
	bool fast = mode == THEME_LIGHT && accent == ICON_STOCK_ACCENT;   /* stock: plain copies */
	if (!fast && !icon_scratch_init(&scratch, max_pixels)) {
		*no_memory = true;
		snprintf(error, sizeof error, "Not enough memory to recolor the icons.");
		return false;
	}
	bool ok = true;
	for (unsigned k = 0; k < COMPONENTS; k++) {
		const struct entry *e = &entries[k];
		if (!e->count)
			continue;
		if (!take_stock(&components[k], e)) {
			*no_memory = true;
			ok = false;
			snprintf(error, sizeof error, "Not enough memory to recolor the icons.");
			continue;
		}
		for (unsigned i = 0; i < e->count; i++) {
			struct icon_header *h = (struct icon_header *)icon_at(e, i);
			int w = (int)h->width, ht = (int)h->height;
			uint16_t *pixels = (uint16_t *)(h + 1);
			const uint8_t *alpha = (const uint8_t *)(pixels + w * ht);
			if (fast)
				memcpy(pixels, components[k].stock[i], 2 * w * ht);
			else
				icon_recolor(components[k].stock[i], alpha, w, ht, icon_flags(component_ids[k], e, i), accent,
				             mode == THEME_DARK, pixels, &scratch);
		}
	}
	if (!fast)
		icon_scratch_free(&scratch);
	return ok;
}

void icons_apply(enum theme_mode mode, unsigned accent_rgb, bool defer)
{
	error[0] = 0;
	deferred.pending = false;
	if (defer) {
		deferred.pending = true;
		deferred.tries_left = DEFERRED_TRIES;
		deferred.mode = mode;
		deferred.accent = accent_rgb;
		return;
	}
	bool no_memory;
	recolor(mode, accent_rgb, &no_memory);
}

void icons_run_deferred(void)
{
	if (!deferred.pending)
		return;
	bool no_memory;
	if (recolor(deferred.mode, deferred.accent, &no_memory) || !no_memory || --deferred.tries_left == 0)
		deferred.pending = false;
	else
		error[0] = 0;   /* out of memory this paint: try again on the next one */
}

const char *icons_take_error(void)
{
	static char taken[sizeof error];
	if (!error[0] || deferred.pending)
		return NULL;
	strcpy(taken, error);
	error[0] = 0;
	return taken;
}
