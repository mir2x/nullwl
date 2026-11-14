#ifndef MENU_H
#define MENU_H

#include "themeicon.h"

#include <dbus/dbus.h>

/*
 * The model of a com.canonical.dbusmenu menu: fetching it, and telling the
 * application what was chosen. Drawing and input live in the compositor
 * (popup.h); nothing here knows about wayland.
 */

typedef struct MenuItem MenuItem;
struct MenuItem {
	int id;
	char *label;       /* mnemonics removed; never NULL */
	int separator;
	int enabled;
	int toggle;        /* -1 not a toggle, 0 off, 1 on */
	int radio;         /* toggle is a radio button rather than a checkmark */
	int has_sub;       /* has children (kids may still be empty) */
	ThemeBitmap *icon; /* may be NULL */
	MenuItem *kids;
	int nkids;
};

typedef struct Menu Menu;
struct Menu {
	MenuItem root;     /* only .kids and .nkids are used */
	DBusConnection *conn;
	char *busname;
	char *busobj;
};

typedef struct MenuReq MenuReq;
/* Called once when the layout arrived (menu != NULL, the callee owns it) or
 * when it could not be fetched (menu == NULL). Never called after
 * menu_fetch_cancel(). */
typedef void (*MenuReadyFn)(Menu *menu, void *ud);

/* icondir: the item's IconThemePath (may be NULL); icon_px: icon size wanted. */
MenuReq *menu_fetch(DBusConnection *conn, const char *busname, const char *busobj,
                    const char *icondir, int icon_px, MenuReadyFn cb, void *ud);
void menu_fetch_cancel(MenuReq *req);

/* Send an Event ("clicked", "opened", "closed", "hovered") for an item. */
void menu_event(Menu *menu, int id, const char *event);
/* Ask the application to refresh a submenu before showing it. */
void menu_about_to_show(Menu *menu, int id);
void menu_free(Menu *menu);

#endif /* MENU_H */
