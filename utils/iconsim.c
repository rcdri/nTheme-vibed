/* iconsim: run nTheme's icon recoloring (src/icon_color.c) on a PC, against iconsColor.res files.
 *   cc -O2 -Isrc utils/iconsim.c src/icon_color.c -lm -o iconsim
 *   iconsim COMPONENT dark|light RRGGBB IN.res OUT.res
 * COMPONENT (syst, dcol, ...) selects the semantic icon ranges, exactly as src/icons.c does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "icon_color.h"

static unsigned get32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static bool semantic(const char *component, unsigned i)
{
	if (!strcmp(component, "syst"))
		return i >= 389 && i <= 405;
	if (!strcmp(component, "dcol"))
		return (i >= 8 && i <= 22) || (i >= 107 && i <= 483);
	return false;
}

int main(int argc, char **argv)
{
	if (argc != 6) {
		fprintf(stderr, "usage: iconsim COMPONENT dark|light RRGGBB IN.res OUT.res\n");
		return 2;
	}
	FILE *f = fopen(argv[4], "rb");
	if (!f) { perror(argv[4]); return 1; }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *d = malloc(size);
	if (fread(d, 1, size, f) != (size_t)size) return 1;
	fclose(f);
	unsigned count = get32(d), base = 8 + 4 * count;
	bool dark = !strcmp(argv[2], "dark");
	unsigned accent = (unsigned)strtoul(argv[3], NULL, 16);
	int max_pixels = 0;
	for (unsigned i = 0; i < count; i++) {
		unsigned char *icon = d + base + get32(d + 8 + 4 * i);
		int pixels = (int)(get32(icon) * get32(icon + 4));
		if (pixels > max_pixels) max_pixels = pixels;
	}
	struct icon_scratch s;
	if (!icon_scratch_init(&s, max_pixels)) { fprintf(stderr, "icon_scratch_init failed\n"); return 1; }
	for (unsigned i = 0; i < count; i++) {
		unsigned char *icon = d + base + get32(d + 8 + 4 * i);
		int w = (int)get32(icon), h = (int)get32(icon + 4);
		uint16_t *px = (uint16_t *)(icon + 20);   /* little-endian host */
		uint8_t *alpha = icon + 20 + 2 * w * h;
		uint16_t *stock = malloc(2 * w * h);
		memcpy(stock, px, 2 * w * h);
		unsigned flags = semantic(argv[1], i) ? ICON_SEMANTIC : 0;
		if (flags && !strcmp(argv[1], "syst"))
			flags |= ICON_PALETTE;   /* the graph color picker, as src/icons.c */
		icon_recolor(stock, alpha, w, h, flags, accent, dark, px, &s);
		free(stock);
	}
	f = fopen(argv[5], "wb");
	if (!f || fwrite(d, 1, size, f) != (size_t)size) { perror(argv[5]); return 1; }
	fclose(f);
	return 0;
}
