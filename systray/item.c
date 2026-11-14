#include "item.h"

#include "helpers.h"
#include "icon.h"
#include "themeicon.h"
#include "watcher.h"

#include <dbus/dbus.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// IWYU pragma: no_include "dbus/dbus-protocol.h"
// IWYU pragma: no_include "dbus/dbus-shared.h"

#define RULEBSIZE 256
#define ICON_SIZE 32 /* what we ask the theme for; the bar scales it */

static const char *match_string =
	"type='signal',"
	"sender='%s',"
	"interface='" SNI_NAME "'";

static unsigned theme_generation = 1;

void
item_theme_changed(void)
{
	theme_generation++;
}

static DBusConnection *
item_get_connection(const Item *item)
{
	return item && item->watcher ? item->watcher->conn : NULL;
}

static char *
xstrdup_or_null(const char *s)
{
	return s && *s ? strdup(s) : NULL;
}

static void
set_str(char **dst, const char *v)
{
	free(*dst);
	*dst = xstrdup_or_null(v);
}

/* ---- decoding the a{sv} that GetAll returns ---- */

typedef struct {
	char *id, *title, *status, *tip_title, *tip_text;
	char *icon_name, *theme_path, *attention_name, *menu;
	int is_menu;
	Icon *pixmap, *attention_pixmap;
} Props;

/* Pick the entry to use out of IconPixmap's a(iiay): smallest at least as big
 * as we want, else the biggest. Returns an Icon or NULL. */
static Icon *
pick_pixmap(DBusMessageIter *variant)
{
	DBusMessageIter arr, st, bytes;
	Icon *best = NULL;
	int best_w = 0;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_ARRAY)
		return NULL;
	dbus_message_iter_recurse(variant, &arr);
	while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
		dbus_int32_t w, h;
		const uint8_t *buf;
		int len;

		dbus_message_iter_recurse(&arr, &st);
		dbus_message_iter_next(&arr);
		if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
			continue;
		dbus_message_iter_get_basic(&st, &w);
		if (!dbus_message_iter_next(&st) || dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
			continue;
		dbus_message_iter_get_basic(&st, &h);
		if (!dbus_message_iter_next(&st) || dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_ARRAY)
			continue;
		dbus_message_iter_recurse(&st, &bytes);
		if (dbus_message_iter_get_arg_type(&bytes) != DBUS_TYPE_BYTE)
			continue;
		dbus_message_iter_get_fixed_array(&bytes, &buf, &len);
		if (w != h || w < 1 || len <= 0)
			continue;
		/* better: first one big enough, or a bigger one when none is yet */
		if (best && !((best_w < ICON_SIZE && w > best_w) || (w >= ICON_SIZE && w < best_w && best_w >= ICON_SIZE)))
			continue;
		{
			Icon *ic = createicon(buf, w, h, len);
			if (!ic)
				continue;
			if (best)
				destroyicon(best);
			best = ic;
			best_w = w;
		}
	}
	return best;
}

/* ToolTip is (sa(iiay)ss): icon name, icon data, title, description. The
 * description may carry a little HTML (<b>, <br>, entities); keep the text. */
static char *
strip_markup(const char *s)
{
	char *out = malloc(strlen(s) + 1), *o = out;
	int tag = 0;

	if (!out)
		return NULL;
	for (; *s; s++) {
		if (tag) {
			if (*s == '>')
				tag = 0;
		} else if (*s == '<') {
			tag = 1;
			if (!strncasecmp(s, "<br", 3) || !strncasecmp(s, "</p", 3))
				*o++ = '\n';
		} else if (*s == '&') {
			static const struct { const char *e; char c; } ent[] = {
				{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '} };
			size_t i;

			for (i = 0; i < sizeof(ent) / sizeof(*ent); i++)
				if (!strncmp(s, ent[i].e, strlen(ent[i].e))) {
					*o++ = ent[i].c;
					s += strlen(ent[i].e) - 1;
					break;
				}
			if (i == sizeof(ent) / sizeof(*ent))
				*o++ = '&';
		} else {
			*o++ = *s;
		}
	}
	*o = '\0';
	return out;
}

static void
parse_tooltip(Props *p, DBusMessageIter *var)
{
	DBusMessageIter st;
	const char *title = NULL, *text = NULL;
	int i;

	if (dbus_message_iter_get_arg_type(var) != DBUS_TYPE_STRUCT)
		return;
	dbus_message_iter_recurse(var, &st);
	for (i = 0; i < 4 && dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INVALID; i++) {
		if (i >= 2 && dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&st, i == 2 ? &title : &text);
		dbus_message_iter_next(&st);
	}
	free(p->tip_title); free(p->tip_text);
	p->tip_title = title ? strip_markup(title) : NULL;
	p->tip_text = text ? strip_markup(text) : NULL;
}

static void
parse_props(Props *p, DBusMessageIter *reply)
{
	DBusMessageIter arr, entry, var;

	memset(p, 0, sizeof *p);
	if (dbus_message_iter_get_arg_type(reply) != DBUS_TYPE_ARRAY)
		return;
	dbus_message_iter_recurse(reply, &arr);
	while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
		const char *key = NULL, *str = NULL;
		int type;

		dbus_message_iter_recurse(&arr, &entry);
		dbus_message_iter_next(&arr);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			continue;
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			continue;
		dbus_message_iter_recurse(&entry, &var);
		type = dbus_message_iter_get_arg_type(&var);

		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH)
			dbus_message_iter_get_basic(&var, &str);

		if (!strcmp(key, "Id") && str)                     set_str(&p->id, str);
		else if (!strcmp(key, "Title") && str)             set_str(&p->title, str);
		else if (!strcmp(key, "Status") && str)            set_str(&p->status, str);
		else if (!strcmp(key, "IconName") && str)          set_str(&p->icon_name, str);
		else if (!strcmp(key, "IconThemePath") && str)     set_str(&p->theme_path, str);
		else if (!strcmp(key, "AttentionIconName") && str) set_str(&p->attention_name, str);
		else if (!strcmp(key, "Menu") && str)              set_str(&p->menu, str);
		else if (!strcmp(key, "ItemIsMenu") && type == DBUS_TYPE_BOOLEAN) {
			dbus_bool_t b;
			dbus_message_iter_get_basic(&var, &b);
			p->is_menu = b;
		} else if (!strcmp(key, "ToolTip")) {
			parse_tooltip(p, &var);
		} else if (!strcmp(key, "IconPixmap")) {
			if (p->pixmap) destroyicon(p->pixmap);
			p->pixmap = pick_pixmap(&var);
		} else if (!strcmp(key, "AttentionIconPixmap")) {
			if (p->attention_pixmap) destroyicon(p->attention_pixmap);
			p->attention_pixmap = pick_pixmap(&var);
		}
	}
}

static void
props_free(Props *p)
{
	free(p->id); free(p->title); free(p->status); free(p->tip_title); free(p->tip_text);
	free(p->icon_name); free(p->theme_path); free(p->attention_name); free(p->menu);
	if (p->pixmap) destroyicon(p->pixmap);
	if (p->attention_pixmap) destroyicon(p->attention_pixmap);
}

/* A themed icon if the name resolves, else the pixmap, else nothing. Takes
 * ownership of pixmap. */
static Icon *
resolve_icon(const char *name, const char *theme_path, Icon *pixmap, const char **source)
{
	if (name && *name) {
		ThemeBitmap *bm = themeicon_load(name, theme_path, ICON_SIZE);
		Icon *ic = bm ? createicon_premul(bm->pix, bm->w, bm->h) : NULL;

		if (ic) {
			free(bm); /* pix now belongs to the icon */
			if (pixmap)
				destroyicon(pixmap);
			*source = "theme";
			return ic;
		}
		themeicon_free(bm);
	}
	if (pixmap) {
		*source = "pixmap";
		return pixmap;
	}
	*source = "none";
	return NULL;
}

static int
str_eq(const char *a, const char *b)
{
	return (!a && !b) || (a && b && !strcmp(a, b));
}

static int
pixmaps_equal(const Icon *a, const Icon *b)
{
	if (!a || !b)
		return !a && !b;
	return a->buf_orig && b->buf_orig && a->size_orig == b->size_orig
		&& !memcmp(a->buf_orig, b->buf_orig, a->size_orig);
}

static ItemStatus
parse_status(const char *s)
{
	if (s && !strcmp(s, "Passive"))
		return ItemPassive;
	if (s && !strcmp(s, "NeedsAttention"))
		return ItemAttention;
	return ItemActive;
}

static void
props_ready(DBusPendingCall *pending, void *data)
{
	Item *item = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	DBusMessageIter iter;
	Props p;
	const char *src_n = "none", *src_a = "none";
	int same_normal;

	dbus_pending_call_unref(pending);
	item->pending = NULL;
	item->refreshing = 0;

	if (!reply)
		return;
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR || !dbus_message_iter_init(reply, &iter)) {
		dbus_message_unref(reply);
		return;
	}
	parse_props(&p, &iter);
	dbus_message_unref(reply);

	set_str(&item->appid, p.id);
	set_str(&item->title, p.title);
	set_str(&item->tip_title, p.tip_title);
	set_str(&item->tip_text, p.tip_text);
	item->status = parse_status(p.status);
	item->is_menu = p.is_menu;
	free(item->menu_busobj);
	/* "/NO_DBUSMENU" is the sentinel Qt uses for "no menu" */
	item->menu_busobj = p.menu && strcmp(p.menu, "/NO_DBUSMENU") && strcmp(p.menu, "/") ? strdup(p.menu) : NULL;

	/* keep the decoded icon when nothing it depends on changed: a themed icon depends
	 * on its name and the theme, a pixmap on its bytes */
	same_normal = item->ready && item->theme_gen == theme_generation
		&& str_eq(p.icon_name, item->icon_name) && str_eq(p.theme_path, item->theme_path)
		&& ((item->icon_source && !strcmp(item->icon_source, "theme")) || pixmaps_equal(item->icon_normal, p.pixmap));

	if (same_normal) {
		if (p.pixmap) destroyicon(p.pixmap);
		p.pixmap = NULL;
	} else {
		if (item->icon_normal) destroyicon(item->icon_normal);
		item->icon_normal = resolve_icon(p.icon_name, p.theme_path, p.pixmap, &src_n);
		p.pixmap = NULL;
		item->icon_source = src_n;
	}
	/* the attention icon is rare, so it is simply rebuilt */
	if (item->icon_attention) destroyicon(item->icon_attention);
	item->icon_attention = resolve_icon(p.attention_name, p.theme_path, p.attention_pixmap, &src_a);
	p.attention_pixmap = NULL;
	set_str(&item->icon_name, p.icon_name);
	set_str(&item->theme_path, p.theme_path);
	set_str(&item->attention_name, p.attention_name);
	item->theme_gen = theme_generation;

	item->icon = item->status == ItemAttention && item->icon_attention ? item->icon_attention : item->icon_normal;
	if (!item->icon)
		item->icon_source = "none";
	item->ready = 1;
	props_free(&p);

	if (item->refresh_again) {
		item->refresh_again = 0;
		item_refresh(item);
	}
	watcher_update_trays(item->watcher);
}

void
item_refresh(Item *item)
{
	DBusConnection *conn = item_get_connection(item);
	DBusMessage *msg;
	const char *iface = SNI_IFACE;

	if (!item->owner)
		return; /* the owner lookup has not answered yet; it refreshes when it does */
	if (item->refreshing) {
		item->refresh_again = 1; /* coalesce a burst of New* signals into one more read */
		return;
	}
	if (!(msg = dbus_message_new_method_call(item->busname, item->busobj, DBUS_INTERFACE_PROPERTIES, "GetAll")))
		return;
	if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)
			&& dbus_connection_send_with_reply(conn, msg, &item->pending, 5000) && item->pending) {
		item->refreshing = 1;
		if (!dbus_pending_call_set_notify(item->pending, props_ready, item, NULL)) {
			dbus_pending_call_cancel(item->pending);
			dbus_pending_call_unref(item->pending);
			item->pending = NULL;
			item->refreshing = 0;
		}
	}
	dbus_message_unref(msg);
}

static void
owner_ready(DBusPendingCall *pending, void *data)
{
	Item *item = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pending);
	const char *owner = NULL;

	dbus_pending_call_unref(pending);
	item->pending = NULL;
	if (reply && dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR
			&& dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) && owner)
		item->owner = strdup(owner);
	if (reply)
		dbus_message_unref(reply);
	if (item->owner)
		item_refresh(item);
}

static void
resolve_owner(Item *item)
{
	DBusConnection *conn = item_get_connection(item);
	DBusMessage *msg;
	const char *name = item->busname;

	if (item->busname[0] == ':') { /* already a unique name */
		item->owner = strdup(item->busname);
		item_refresh(item);
		return;
	}
	if (!(msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner")))
		return;
	if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)
			&& dbus_connection_send_with_reply(conn, msg, &item->pending, 5000) && item->pending
			&& !dbus_pending_call_set_notify(item->pending, owner_ready, item, NULL)) {
		dbus_pending_call_cancel(item->pending);
		dbus_pending_call_unref(item->pending);
		item->pending = NULL;
	}
	dbus_message_unref(msg);
}

static DBusHandlerResult
filter_bus(DBusConnection *conn, DBusMessage *msg, void *data)
{
	Item *item = data;
	const char *sender;

	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL
			|| !dbus_message_has_interface(msg, SNI_IFACE))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	sender = dbus_message_get_sender(msg);
	/* the sender of a signal is always the unique name, never a well-known one */
	if (!sender || !item->owner || strcmp(sender, item->owner))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (dbus_message_is_signal(msg, SNI_IFACE, "NewIcon")
			|| dbus_message_is_signal(msg, SNI_IFACE, "NewAttentionIcon")
			|| dbus_message_is_signal(msg, SNI_IFACE, "NewOverlayIcon")
			|| dbus_message_is_signal(msg, SNI_IFACE, "NewStatus")
			|| dbus_message_is_signal(msg, SNI_IFACE, "NewTitle")
			|| dbus_message_is_signal(msg, SNI_IFACE, "NewToolTip")) {
		item_refresh(item);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

Item *
createitem(const char *busname, const char *busobj, Watcher *watcher)
{
	DBusConnection *conn = watcher->conn;
	Item *item = calloc(1, sizeof(Item));
	char match_rule[RULEBSIZE];

	if (!item)
		return NULL;
	item->busname = strdup(busname);
	item->busobj = strdup(busobj);
	item->watcher = watcher;
	item->status = ItemActive;
	item->icon_source = "none";
	if (!item->busname || !item->busobj
			|| snprintf(match_rule, sizeof match_rule, match_string, busname) >= RULEBSIZE
			|| !dbus_connection_add_filter(conn, filter_bus, item, NULL)) {
		free(item->busname);
		free(item->busobj);
		free(item);
		return NULL;
	}
	dbus_bus_add_match(conn, match_rule, NULL);
	resolve_owner(item);
	return item;
}

void
destroyitem(Item *item)
{
	DBusConnection *conn = item_get_connection(item);
	char match_rule[RULEBSIZE];

	if (item->pending) { /* its notify callback would get a freed item */
		dbus_pending_call_cancel(item->pending);
		dbus_pending_call_unref(item->pending);
	}
	if (snprintf(match_rule, sizeof match_rule, match_string, item->busname) < RULEBSIZE) {
		dbus_bus_remove_match(conn, match_rule, NULL);
		dbus_connection_remove_filter(conn, filter_bus, item);
	}
	if (item->icon_normal)
		destroyicon(item->icon_normal);
	if (item->icon_attention)
		destroyicon(item->icon_attention);
	free(item->menu_busobj);
	free(item->busname);
	free(item->busobj);
	free(item->owner);
	free(item->appid);
	free(item->title);
	free(item->tip_title);
	free(item->tip_text);
	free(item->icon_name);
	free(item->theme_path);
	free(item->attention_name);
	free(item);
}

int
item_visible(const Item *item)
{
	return item->status != ItemPassive;
}

int
item_has_menu(const Item *item)
{
	return item->menu_busobj != NULL;
}

/* ---- calling the item ---- */

static void
call_item(Item *item, const char *method, int type1, const void *a1, int type2, const void *a2)
{
	DBusConnection *conn = item_get_connection(item);
	DBusMessage *msg = dbus_message_new_method_call(item->busname, item->busobj, SNI_IFACE, method);

	if (!msg)
		return;
	if (dbus_message_append_args(msg, type1, a1, type2, a2, DBUS_TYPE_INVALID))
		dbus_connection_send(conn, msg, NULL); /* fire and forget */
	dbus_message_unref(msg);
}

void
item_activate(Item *item, int x, int y)
{
	dbus_int32_t px = x, py = y;
	call_item(item, "Activate", DBUS_TYPE_INT32, &px, DBUS_TYPE_INT32, &py);
}

void
item_secondary_activate(Item *item, int x, int y)
{
	dbus_int32_t px = x, py = y;
	call_item(item, "SecondaryActivate", DBUS_TYPE_INT32, &px, DBUS_TYPE_INT32, &py);
}

void
item_context_menu(Item *item, int x, int y)
{
	dbus_int32_t px = x, py = y;
	call_item(item, "ContextMenu", DBUS_TYPE_INT32, &px, DBUS_TYPE_INT32, &py);
}

void
item_scroll(Item *item, int delta, int horizontal)
{
	dbus_int32_t d = delta;
	const char *orient = horizontal ? "horizontal" : "vertical";
	call_item(item, "Scroll", DBUS_TYPE_INT32, &d, DBUS_TYPE_STRING, &orient);
}
