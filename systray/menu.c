#include "menu.h"

#include <cairo.h>
#include <dbus/dbus.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DBUSMENU_IFACE "com.canonical.dbusmenu"
#define MAX_DEPTH 8
#define MAX_ITEMS 512 /* per menu, a guard against a broken application */

struct MenuReq {
	Menu *menu;       /* being built */
	char *icondir;
	int icon_px;
	MenuReadyFn cb;
	void *ud;
	int cancelled;
	DBusPendingCall *pending;
};

static void
item_free(MenuItem *mi)
{
	int i;

	for (i = 0; i < mi->nkids; i++)
		item_free(&mi->kids[i]);
	free(mi->kids);
	free(mi->label);
	themeicon_free(mi->icon);
}

void
menu_free(Menu *menu)
{
	if (!menu)
		return;
	item_free(&menu->root);
	if (menu->conn)
		dbus_connection_unref(menu->conn);
	free(menu->busname);
	free(menu->busobj);
	free(menu);
}

/* "_File" -> "File", "A__B" -> "A_B" */
static char *
strip_mnemonic(const char *s)
{
	char *out = malloc(strlen(s) + 1), *o = out;

	if (!out)
		return NULL;
	for (; *s; s++) {
		if (*s == '_') {
			if (s[1] == '_')
				*o++ = *++s;
			continue;
		}
		*o++ = *s;
	}
	*o = '\0';
	return out;
}

typedef struct { const uint8_t *p; size_t left; } PngReader;

static cairo_status_t
png_read(void *c, unsigned char *buf, unsigned int n)
{
	PngReader *r = c;

	if (n > r->left)
		return CAIRO_STATUS_READ_ERROR;
	memcpy(buf, r->p, n);
	r->p += n;
	r->left -= n;
	return CAIRO_STATUS_SUCCESS;
}

static ThemeBitmap *
icon_from_png(const uint8_t *data, size_t len)
{
	PngReader rd = { data, len };
	cairo_surface_t *s = cairo_image_surface_create_from_png_stream(png_read, &rd);
	ThemeBitmap *bm = NULL;
	int w, h, y, stride;
	unsigned char *src;

	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(s);
		return NULL;
	}
	w = cairo_image_surface_get_width(s);
	h = cairo_image_surface_get_height(s);
	stride = cairo_image_surface_get_stride(s);
	src = cairo_image_surface_get_data(s);
	if (w > 0 && h > 0 && w <= 512 && h <= 512 && src && (bm = calloc(1, sizeof(*bm)))) {
		bm->pix = malloc((size_t)w * h * 4);
		if (!bm->pix) {
			free(bm);
			bm = NULL;
		} else {
			bm->w = w;
			bm->h = h;
			for (y = 0; y < h; y++)
				memcpy(bm->pix + (size_t)y * w, src + (size_t)y * stride, (size_t)w * 4);
		}
	}
	cairo_surface_destroy(s);
	return bm;
}

static int
parse_props(DBusMessageIter *dict, MenuItem *mi, MenuReq *req, int *visible, int *submenu)
{
	DBusMessageIter ent, val;
	const char *key, *type = NULL, *toggle_type = NULL, *label = NULL, *icon_name = NULL;
	dbus_bool_t b;
	dbus_int32_t i32;
	int vt;
	const uint8_t *png = NULL;
	int npng = 0;

	do {
		dbus_message_iter_recurse(dict, &ent);
		if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_STRING)
			return -1;
		dbus_message_iter_get_basic(&ent, &key);
		if (!dbus_message_iter_next(&ent) ||
		    dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_VARIANT)
			return -1;
		dbus_message_iter_recurse(&ent, &val);
		vt = dbus_message_iter_get_arg_type(&val);

		if (!strcmp(key, "label") && vt == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&val, &label);
		else if (!strcmp(key, "type") && vt == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&val, &type);
		else if (!strcmp(key, "toggle-type") && vt == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&val, &toggle_type);
		else if (!strcmp(key, "children-display") && vt == DBUS_TYPE_STRING) {
			const char *cd;
			dbus_message_iter_get_basic(&val, &cd);
			*submenu = !strcmp(cd, "submenu");
		} else if (!strcmp(key, "icon-name") && vt == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&val, &icon_name);
		else if (!strcmp(key, "visible") && vt == DBUS_TYPE_BOOLEAN) {
			dbus_message_iter_get_basic(&val, &b);
			*visible = b;
		} else if (!strcmp(key, "enabled") && vt == DBUS_TYPE_BOOLEAN) {
			dbus_message_iter_get_basic(&val, &b);
			mi->enabled = b;
		} else if (!strcmp(key, "toggle-state") && vt == DBUS_TYPE_INT32) {
			dbus_message_iter_get_basic(&val, &i32);
			mi->toggle = i32 > 0 ? 1 : 0;
		} else if (!strcmp(key, "icon-data") && vt == DBUS_TYPE_ARRAY) {
			DBusMessageIter arr;
			dbus_message_iter_recurse(&val, &arr);
			if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_BYTE)
				dbus_message_iter_get_fixed_array(&arr, &png, &npng);
		}
	} while (dbus_message_iter_next(dict));

	mi->separator = type && !strcmp(type, "separator");
	if (!toggle_type || !*toggle_type)
		mi->toggle = -1; /* the state is meaningless without a toggle type */
	else
		mi->radio = !strcmp(toggle_type, "radio");
	mi->label = strip_mnemonic(label ? label : "");
	if (!mi->label)
		return -1;
	if (!mi->separator) {
		if (png && npng > 0)
			mi->icon = icon_from_png(png, (size_t)npng);
		if (!mi->icon && icon_name && *icon_name)
			mi->icon = themeicon_load(icon_name, req->icondir, req->icon_px);
	}
	return 0;
}

/* node is a struct (i a{sv} av); fills mi. Returns 0, or -1 on a malformed reply. */
static int
parse_node(DBusMessageIter *node, MenuItem *mi, MenuReq *req, int depth, int *visible, int *budget)
{
	DBusMessageIter f, dict, kids, var;
	dbus_int32_t id;
	int submenu = 0, n = 0;

	mi->enabled = 1;
	mi->toggle = -1;
	dbus_message_iter_recurse(node, &f);
	if (dbus_message_iter_get_arg_type(&f) != DBUS_TYPE_INT32)
		return -1;
	dbus_message_iter_get_basic(&f, &id);
	mi->id = id;
	*visible = 1;

	if (!dbus_message_iter_next(&f) || dbus_message_iter_get_arg_type(&f) != DBUS_TYPE_ARRAY)
		return -1;
	dbus_message_iter_recurse(&f, &dict);
	if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		if (parse_props(&dict, mi, req, visible, &submenu) < 0)
			return -1;
	} else if (!(mi->label = strdup(""))) {
		return -1;
	}

	if (!dbus_message_iter_next(&f) || dbus_message_iter_get_arg_type(&f) != DBUS_TYPE_ARRAY)
		return -1;
	dbus_message_iter_recurse(&f, &kids);
	if (depth >= MAX_DEPTH)
		return 0;
	while (dbus_message_iter_get_arg_type(&kids) == DBUS_TYPE_VARIANT) {
		MenuItem *k;
		int kvis;

		if (--*budget < 0)
			return -1;
		dbus_message_iter_recurse(&kids, &var);
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRUCT)
			return -1;
		k = realloc(mi->kids, (size_t)(n + 1) * sizeof(*k));
		if (!k)
			return -1;
		mi->kids = k;
		k = &mi->kids[n];
		memset(k, 0, sizeof(*k));
		mi->nkids = ++n; /* counted first so a failure below still frees it */
		if (parse_node(&var, k, req, depth + 1, &kvis, budget) < 0)
			return -1;
		if (!kvis) { /* hidden: drop it again */
			item_free(k);
			mi->nkids = --n;
		}
		dbus_message_iter_next(&kids);
	}
	mi->has_sub = submenu || n > 0;
	return 0;
}

static void
req_done(MenuReq *req, int ok)
{
	if (!req->cancelled)
		req->cb(ok ? req->menu : NULL, req->ud);
	else
		menu_free(req->menu);
	if (req->pending)
		dbus_pending_call_unref(req->pending);
	free(req->icondir);
	free(req);
}

static void
layout_ready(DBusPendingCall *pending, void *data)
{
	MenuReq *req = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	DBusMessageIter it;
	dbus_uint32_t rev;
	int ok = 0, vis, budget = MAX_ITEMS;

	if (!req->cancelled && reply && dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR &&
	    dbus_message_iter_init(reply, &it) &&
	    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
		dbus_message_iter_get_basic(&it, &rev);
		if (dbus_message_iter_next(&it) &&
		    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRUCT) {
			/* the reply's struct is the root node (id 0), which holds only children */
			MenuItem root = {0};

			ok = parse_node(&it, &root, req, 0, &vis, &budget) == 0 && root.nkids > 0;
			if (ok) {
				req->menu->root.kids = root.kids;
				req->menu->root.nkids = root.nkids;
				root.kids = NULL;
				root.nkids = 0;
			}
			item_free(&root);
		}
	}
	if (reply)
		dbus_message_unref(reply);
	req_done(req, ok);
}

static void
about_ready(DBusPendingCall *pending, void *data)
{
	MenuReq *req = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	DBusMessage *msg;
	DBusMessageIter it, strings;
	dbus_int32_t parent = 0, depth = -1;
	DBusPendingCall *next = NULL;

	/* An error here is not fatal: not every application implements AboutToShow. */
	if (reply)
		dbus_message_unref(reply);
	dbus_pending_call_unref(req->pending);
	req->pending = NULL;
	if (req->cancelled)
		goto fail;

	msg = dbus_message_new_method_call(req->menu->busname, req->menu->busobj,
			DBUSMENU_IFACE, "GetLayout");
	if (!msg)
		goto fail;
	dbus_message_iter_init_append(msg, &it);
	if (!dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &parent) ||
	    !dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &depth) ||
	    !dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &strings) ||
	    !dbus_message_iter_close_container(&it, &strings) ||
	    !dbus_connection_send_with_reply(req->menu->conn, msg, &next, 5000) || !next) {
		dbus_message_unref(msg);
		goto fail;
	}
	dbus_message_unref(msg);
	req->pending = next;
	if (!dbus_pending_call_set_notify(next, layout_ready, req, NULL))
		goto fail;
	return;
fail:
	req_done(req, 0);
}

MenuReq *
menu_fetch(DBusConnection *conn, const char *busname, const char *busobj,
           const char *icondir, int icon_px, MenuReadyFn cb, void *ud)
{
	MenuReq *req = calloc(1, sizeof(*req));
	DBusMessage *msg = NULL;
	dbus_int32_t parent = 0;

	if (!req)
		return NULL;
	req->menu = calloc(1, sizeof(Menu));
	req->icondir = icondir ? strdup(icondir) : NULL;
	req->icon_px = icon_px;
	req->cb = cb;
	req->ud = ud;
	if (!req->menu)
		goto fail;
	req->menu->conn = dbus_connection_ref(conn);
	req->menu->busname = strdup(busname);
	req->menu->busobj = strdup(busobj);
	if (!req->menu->busname || !req->menu->busobj)
		goto fail;

	msg = dbus_message_new_method_call(busname, busobj, DBUSMENU_IFACE, "AboutToShow");
	if (!msg || !dbus_message_append_args(msg, DBUS_TYPE_INT32, &parent, DBUS_TYPE_INVALID) ||
	    !dbus_connection_send_with_reply(conn, msg, &req->pending, 5000) || !req->pending ||
	    !dbus_pending_call_set_notify(req->pending, about_ready, req, NULL))
		goto fail;
	dbus_message_unref(msg);
	return req;
fail:
	if (msg)
		dbus_message_unref(msg);
	if (req->pending) {
		dbus_pending_call_cancel(req->pending);
		dbus_pending_call_unref(req->pending);
	}
	menu_free(req->menu);
	free(req->icondir);
	free(req);
	return NULL;
}

void
menu_fetch_cancel(MenuReq *req)
{
	if (req)
		req->cancelled = 1; /* freed when the reply (or timeout) arrives */
}

void
menu_event(Menu *menu, int itemid, const char *event)
{
	DBusMessage *msg = dbus_message_new_method_call(menu->busname, menu->busobj,
			DBUSMENU_IFACE, "Event");
	DBusMessageIter it, sub;
	dbus_int32_t id = itemid;
	dbus_uint32_t ts = (dbus_uint32_t)time(NULL);
	const char *data = "";

	if (!msg)
		return;
	dbus_message_iter_init_append(msg, &it);
	if (dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &id) &&
	    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &event) &&
	    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &sub)) {
		dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &data);
		dbus_message_iter_close_container(&it, &sub);
		if (dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &ts))
			dbus_connection_send(menu->conn, msg, NULL);
	}
	dbus_message_unref(msg);
}

void
menu_about_to_show(Menu *menu, int itemid)
{
	DBusMessage *msg = dbus_message_new_method_call(menu->busname, menu->busobj,
			DBUSMENU_IFACE, "AboutToShow");
	dbus_int32_t id = itemid;

	if (!msg)
		return;
	if (dbus_message_append_args(msg, DBUS_TYPE_INT32, &id, DBUS_TYPE_INVALID))
		dbus_connection_send(menu->conn, msg, NULL);
	dbus_message_unref(msg);
}
