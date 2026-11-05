#include <os.h>
#include <hook.h>
#include "patch.h"
#include "theme.h"
#include "glass.h"
#include "wallpaper.h"
#include "title.h"
#include "icons.h"

#define TRAMPOLINE 0xE51FF004   /* ldr pc,[pc,#-4]: what an installed Ndless hook reads back as */
#define SCREEN_WIDTH 320

/* HOOK_SAVED_REGS layout: r0..r12, lr, then the hooked function's own stack. */
#define REG_LR 13
#define STACK_WORD(n) (14 + (n))
#define GC_REGISTER 4   /* both home painter sites keep the gc in r4 (verified osid 48) */

static const struct os_table *os;   /* hooks take no arguments */

static unsigned pack_rgb(const unsigned *regs)
{
	return ((regs[1] & 0xff) << 16) | ((regs[2] & 0xff) << 8) | (regs[3] & 0xff);
}

static void unpack_rgb(unsigned *regs, unsigned rgb)
{
	regs[1] = rgb >> 16;
	regs[2] = (rgb >> 8) & 0xff;
	regs[3] = rgb & 0xff;
}

/* The gui_gc_setColorRGB wrapper pushes {r4,lr} before dispatching to the driver,
 * so its caller sits one word up the stack; a direct vtable call has the caller in lr. */
static bool called_from_os(const unsigned *regs)
{
	unsigned caller = regs[REG_LR] == os->set_color_wrapper_return ? regs[STACK_WORD(1)] : regs[REG_LR];
	return caller >= os->os_code_begin && caller < os->os_code_end;
}

/* Driver setColorRGB(gc, r, g, b). Ndless programs drawing through the OS keep their colors. */
HOOK_DEFINE(set_color_hook) {
	unsigned *regs = HOOK_SAVED_REGS(set_color_hook);
	if (called_from_os(regs)) {
		unsigned rgb = theme_remap(pack_rgb(regs));
		glass_on_color((Gc)regs[0], rgb);
		unpack_rgb(regs, rgb);
	}
	HOOK_RESTORE_RETURN(set_color_hook);
}

/* Home painter, right after it filled the background (before it draws the app icons). */
HOOK_DEFINE(home_background_hook) {
	icons_run_deferred();
	const void *image = wallpaper_image();
	if (image)
		gui_gc_drawImage((Gc)HOOK_SAVED_REGS(home_background_hook)[GC_REGISTER], (char *)image, 0, WALLPAPER_TOP);
	HOOK_RESTORE_RETURN(home_background_hook);
}

/* 3D graphing, right after the renderer's image was copied to the screen buffer (2D graphs never get here; they
 * draw through the color hook). r7 = graph view + 0x70; the view has its width at +0xc, height at +0x10 and its
 * gc at +0x74 (verified OS 6.4, CX II-T). The image is copied whole each time, so this recolors once per frame. */
#define VIEW_FROM_R7 0x70
#define VIEW_GC 0x74

HOOK_DEFINE(view_3d_hook) {
	unsigned *regs = HOOK_SAVED_REGS(view_3d_hook);
	const unsigned char *view = (const unsigned char *)(regs[7] - VIEW_FROM_R7);
	if ((unsigned)view >= 0x10000000u && (unsigned)view < 0x14000000u)
		theme_recolor_view(*(const void *const *)(view + VIEW_GC), *(const int *)(view + 0xc), *(const int *)(view + 0x10));
	HOOK_RESTORE_RETURN(view_3d_hook);
}

/* Home painter, right before it draws the title text: r1 = utf16, r2 = x. */
HOOK_DEFINE(home_title_hook) {
	unsigned *regs = HOOK_SAVED_REGS(home_title_hook);
	int length;
	const uint16_t *text = title_text(&length);
	if (text) {
		Gc gc = (Gc)regs[GC_REGISTER];
		int width = gui_gc_getStringWidth(gc, os->home_title_font, (char *)text, 0, length);
		regs[1] = (unsigned)text;
		regs[2] = (unsigned)((SCREEN_WIDTH - width) / 2);
	}
	HOOK_RESTORE_RETURN(home_title_hook);
}

#define MAX_SITES 4

static int sites(const struct os_table *t, const struct hook_site *out[])
{
	int n = 0;
	out[n++] = &t->set_color_driver;
	out[n++] = &t->home_background;
	out[n++] = &t->home_title;
	if (t->view_3d.address)
		out[n++] = &t->view_3d;
	return n;
}

enum patch_state patch_probe(const struct os_table *t)
{
	const struct hook_site *site[MAX_SITES];
	int count = sites(t, site), pristine = 0, resident = 0;
	for (int i = 0; i < count; i++) {
		const unsigned *code = (const unsigned *)site[i]->address;
		if (code[0] == TRAMPOLINE)
			resident++;
		else if (code[0] == site[i]->word0 && code[1] == site[i]->word1)
			pristine++;
	}
	if (*(unsigned *)t->menu_literal == t->menu_base)
		pristine++;
	else
		resident++;
	count++;
	if (pristine == count)
		return PATCH_PRISTINE;
	if (resident == count)
		return PATCH_RESIDENT;
	return PATCH_UNEXPECTED;
}

void patch_install(const struct os_table *t)
{
	os = t;
	HOOK_INSTALL(t->set_color_driver.address, set_color_hook);
	HOOK_INSTALL(t->home_background.address, home_background_hook);
	HOOK_INSTALL(t->home_title.address, home_title_hook);
	if (t->view_3d.address) {   /* HOOK_INSTALL is several statements */
		HOOK_INSTALL(t->view_3d.address, view_3d_hook);
	}
}
