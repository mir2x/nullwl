#include "tray.h"

#include "icon.h"
#include "item.h"
#include "menu.h"
#include "watcher.h"

#include <fcft/fcft.h>
#include <pixman.h>
#include <wayland-util.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PIXMAN_COLOR(hex)                        \
	{ .red = ((hex >> 24) & 0xff) * 0x101,   \
	  .green = ((hex >> 16) & 0xff) * 0x101, \
	  .blue = ((hex >> 8) & 0xff) * 0x101,   \
	  .alpha = (hex & 0xff) * 0x101 }

static Watcher *
tray_get_watcher(const Tray *tray)
{
	if (!tray)
		return NULL;

	return tray->watcher;
}

/* the idx-th item that is shown (Passive ones are not) */
static Item *
nth_visible(Watcher *watcher, unsigned idx)
{
	Item *item;
	unsigned n = 0;

	wl_list_for_each(item, &watcher->items, link) {
		if (!item_visible(item))
			continue;
		if (n++ == idx)
			return item;
	}
	return NULL;
}

static int
count_visible(Watcher *watcher)
{
	Item *item;
	int n = 0;

	wl_list_for_each(item, &watcher->items, link)
		n += item_visible(item);
	return n;
}

static pixman_image_t *
createcanvas(int width, int height, int bgcolor)
{
	pixman_image_t *src, *dest;
	pixman_color_t bgcolor_pix = PIXMAN_COLOR(bgcolor);

	dest = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, NULL,
	                                0);
	src = pixman_image_create_solid_fill(&bgcolor_pix);

	pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, dest, 0, 0, 0, 0, 0,
	                         0, width, height);

	pixman_image_unref(src);
	return dest;
}

void
tray_update(Tray *tray)
{
	Item *item;
	Watcher *watcher;
	int icon_size, i = 0, canvas_width, canvas_height, n_items, spacing;
	pixman_image_t *canvas = NULL, *img;

	watcher = tray_get_watcher(tray);
	n_items = count_visible(watcher);

	if (!n_items) {
		if (tray->image) {
			pixman_image_unref(tray->image);
			tray->image = NULL;
		}
		tray->cb(tray->monitor);
		return;
	}

	icon_size = tray->height;
	spacing = tray->spacing;
	canvas_width = n_items * (icon_size + spacing) + spacing;
	canvas_height = tray->height;

	canvas = createcanvas(canvas_width, canvas_height, tray->scheme[1]);
	if (!canvas)
		goto fail;

	wl_list_for_each(item, &watcher->items, link) {
		int slot_x_start = spacing + i * (icon_size + spacing);
		int slot_x_end = slot_x_start + icon_size + spacing;
		int slot_x_width = slot_x_end - slot_x_start;

		int slot_y_start = 0;
		int slot_y_end = canvas_height;
		int slot_y_width = slot_y_end - slot_y_start;

		if (!item_visible(item))
			continue;

		if (item->icon) {
			/* Real icon */
			img = item->icon->img;
			if (resize_image(img, icon_size, icon_size) < 0)
				goto fail;
			pixman_image_composite32(PIXMAN_OP_OVER, img, NULL,
			                         canvas, 0, 0, 0, 0,
			                         slot_x_start, 0, canvas_width,
			                         canvas_height);

		} else if (item->appid) {
			/* Font glyph alpha mask */
			const struct fcft_glyph *g;
			int pen_y, pen_x;
			pixman_color_t fg_color = PIXMAN_COLOR(tray->scheme[0]);
			pixman_image_t *fg;

			if (item->fallback_icon) {
				g = item->fallback_icon;
			} else {
				g = createfallbackicon(item->appid,
				                       item->fgcolor,
				                       tray->font);
				if (!g)
					goto fail;
				item->fallback_icon = g;
			}

			pen_x = slot_x_start + (slot_x_width - g->width) / 2;
			pen_y = slot_y_start + (slot_y_width - g->height) / 2;

			fg = pixman_image_create_solid_fill(&fg_color);
			pixman_image_composite32(PIXMAN_OP_OVER, fg, g->pix,
			                         canvas, 0, 0, 0, 0, pen_x,
			                         pen_y, canvas_width,
			                         canvas_height);
			pixman_image_unref(fg);
		}
		i++;
	}

	if (tray->image)
		pixman_image_unref(tray->image);
	tray->image = canvas;
	tray->cb(tray->monitor);

	return;

fail:
	if (canvas)
		pixman_image_unref(canvas);
	return;
}

void
destroytray(Tray *tray)
{
	if (tray->image)
		pixman_image_unref(tray->image);
	if (tray->font)
		fcft_destroy(tray->font);
	free(tray);
}

Tray *
createtray(void *monitor, int height, int spacing, uint32_t *colorscheme,
           const char **fonts, const char *fontattrs, TrayNotifyCb cb,
           Watcher *watcher)
{
	Tray *tray = NULL;
	char fontattrs_my[128];
	struct fcft_font *font = NULL;

	sprintf(fontattrs_my, "%s:%s", fontattrs, "weight:bold");

	tray = calloc(1, sizeof(Tray));
	font = fcft_from_name(1, fonts, fontattrs_my);
	if (!tray || !font)
		goto fail;

	tray->monitor = monitor;
	tray->height = height;
	tray->spacing = spacing;
	tray->scheme = colorscheme;
	tray->cb = cb;
	tray->watcher = watcher;
	tray->font = font;

	return tray;

fail:
	if (font)
		fcft_destroy(font);
	free(tray);
	return NULL;
}

int
tray_get_width(const Tray *tray)
{
	if (tray && tray->image)
		return pixman_image_get_width(tray->image);
	else
		return 0;
}

int
tray_get_icon_width(const Tray *tray)
{
	if (!tray)
		return 0;

	return tray->height;
}

/* x is relative to the left edge of the tray; returns the index among shown items, or -1 */
int
tray_index_at(const Tray *tray, int x)
{
	int n, slot;

	if (!tray || !tray->image || x < 0)
		return -1;
	n = count_visible(tray->watcher);
	slot = tray->height + tray->spacing;
	if (!n || slot <= 0)
		return -1;
	x -= tray->spacing / 2;
	return x < 0 ? 0 : x / slot >= n ? n - 1 : x / slot;
}

void
tray_click(Tray *tray, unsigned int index, int button, int x, int y, const char **menucmd)
{
	Watcher *watcher = tray_get_watcher(tray);
	Item *item = nth_visible(watcher, index);

	if (!item)
		return;
	switch (button) {
	case TrayLeft:
		/* menu-only items (ItemIsMenu) have nothing to activate */
		if (item->is_menu && item_has_menu(item))
			menu_show(watcher->conn, watcher->loop, item->busname, item->menu_busobj, menucmd);
		else
			item_activate(item, x, y);
		break;
	case TrayMiddle:
		item_secondary_activate(item, x, y);
		break;
	case TrayRight:
		if (item_has_menu(item))
			menu_show(watcher->conn, watcher->loop, item->busname, item->menu_busobj, menucmd);
		else
			item_context_menu(item, x, y);
		break;
	}
}

void
tray_scroll(Tray *tray, unsigned int index, int delta, int horizontal)
{
	Item *item = nth_visible(tray_get_watcher(tray), index);

	if (item)
		item_scroll(item, delta, horizontal);
}
