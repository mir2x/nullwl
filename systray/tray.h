#ifndef TRAY_H
#define TRAY_H

#include "watcher.h"

#include <pixman.h>
#include <wayland-util.h>

#include <stdint.h>

typedef void (*TrayNotifyCb)(void *data);

typedef struct {
	pixman_image_t *image;
	struct fcft_font *font;
	uint32_t *scheme;
	TrayNotifyCb cb;
	Watcher *watcher;
	void *monitor;
	int height;
	int spacing;

	struct wl_list link;
} Tray;

Tray *createtray (void *monitor, int height, int spacing, uint32_t *colorscheme,
                  const char **fonts, const char *fontattrs, TrayNotifyCb cb,
                  Watcher *watcher);
void destroytray (Tray *tray);

int tray_get_width (const Tray *tray);
int tray_get_icon_width (const Tray *tray);
void tray_update (Tray *tray);
enum { TrayLeft, TrayMiddle, TrayRight };
int tray_index_at (const Tray *tray, int x);
/* Called to show an item's dbusmenu; the compositor draws it. */
typedef void (*TrayMenuFn)(Watcher *watcher, const char *busname, const char *menu_obj,
                           const char *icondir, int x, int y);
void tray_click (Tray *tray, unsigned int index, int button, int x, int y,
                 TrayMenuFn showmenu);
void tray_scroll (Tray *tray, unsigned int index, int delta, int horizontal);

#endif /* TRAY_H */
