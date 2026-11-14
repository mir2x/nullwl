#ifndef ITEM_H
#define ITEM_H

#include "icon.h"
#include "watcher.h"

#include <dbus/dbus.h>
#include <wayland-util.h>

/*
 * The FDO spec says "org.freedesktop.StatusNotifierItem"[1],
 * but both the client libraries[2,3] actually use "org.kde.StatusNotifierItem"
 *
 * [1] https://www.freedesktop.org/wiki/Specifications/StatusNotifierItem/
 * [2] https://github.com/AyatanaIndicators/libayatana-appindicator-glib
 * [3] https://invent.kde.org/frameworks/kstatusnotifieritem
 *
 */
#define SNI_NAME "org.kde.StatusNotifierItem"
#define SNI_OPATH "/StatusNotifierItem"
#define SNI_IFACE "org.kde.StatusNotifierItem"

typedef enum { ItemPassive, ItemActive, ItemAttention } ItemStatus;

typedef struct Item {
	struct wl_list icons;
	char *busname;       /* as registered: unique or well-known */
	char *busobj;
	char *owner;         /* the unique name behind busname; signal senders are this */
	char *menu_busobj;   /* dbusmenu object path, or NULL when the item has none */
	char *appid;         /* the Id property */
	char *title;
	char *icon_name, *theme_path, *attention_name;
	Icon *icon;          /* what is drawn; points at one of the two below */
	Icon *icon_normal, *icon_attention;
	const char *icon_source; /* "theme", "pixmap" or "none", for diagnostics */
	FallbackIcon *fallback_icon;
	ItemStatus status;
	int is_menu;         /* ItemIsMenu: a left click should open the menu */
	unsigned theme_gen;  /* icon theme generation the icons were resolved with */

	Watcher *watcher;

	int fgcolor;
	int ready;           /* the properties have been read at least once */

	DBusPendingCall *pending; /* the call in flight, cancelled on destroy */
	int refreshing, refresh_again;

	struct wl_list link;
} Item;

Item *createitem (const char *busname, const char *busobj, Watcher *watcher);
void destroyitem (Item *item);

/* Re-read every property, e.g. after the icon theme changed. */
void item_refresh (Item *item);
/* Passive items are not shown. */
int item_visible (const Item *item);
/* Has a dbusmenu we can show (not just ContextMenu). */
int item_has_menu (const Item *item);

/* x, y are the pointer position in layout coordinates. */
void item_activate (Item *item, int x, int y);
void item_secondary_activate (Item *item, int x, int y);
void item_context_menu (Item *item, int x, int y);
void item_scroll (Item *item, int delta, int horizontal);
/* The icon theme (or its configuration) changed: icons are looked up again on the next refresh. */
void item_theme_changed (void);

#endif /* ITEM_H */
