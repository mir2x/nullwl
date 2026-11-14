/*
 * Popup menus: draws a dbusmenu (see systray/menu.c) as a scene-graph overlay
 * and routes pointer and keyboard input to it while it is open. Included by
 * nullwl.c.
 *
 * All geometry here is in physical pixels relative to the monitor's top left
 * corner; the scene node converts to layout coordinates by dividing by the
 * output scale. At most one popup is open at a time. While it is open it
 * grabs the pointer and the keyboard completely; any click outside it, or
 * Escape, closes it.
 */
#include "systray/menu.h"

#define POP_MAXPANELS 8

typedef struct {
	MenuItem *items;
	int n;
	int x, y, w, h;   /* h is the full content height, vh how much is shown */
	int vh;
	int scroll;
	int hover;        /* row index or -1 */
	int *ry;          /* top of each row inside the content, n + 1 entries */
	int has_icons;
} PopPanel;

typedef struct {
	struct wlr_buffer base;
	Img *image;
	uint32_t data[];
} PopBuf;

static struct {
	MenuReq *req;
	Menu *menu;
	Monitor *mon;
	PopPanel p[POP_MAXPANELS];
	int np;
	struct wlr_scene_buffer *sb;
	Drwl *drw;
	int pressed_in;
	int ax;            /* where it was asked for, in monitor pixels */
	double scale;
	int fh, rowh, sepdh, hpad, bw;
} pop;

static void popbuf_destroy(struct wlr_buffer *b)
{
	PopBuf *pb = wl_container_of(b, pb, base);
	drwl_image_destroy(pb->image);
	free(pb);
}

static bool popbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
		uint32_t *format, size_t *stride)
{
	PopBuf *pb = wl_container_of(b, pb, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = pb->data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)b->width * 4;
	return true;
}

static void popbuf_end(struct wlr_buffer *b) { }

static const struct wlr_buffer_impl popbuf_impl = {
	.destroy = popbuf_destroy,
	.begin_data_ptr_access = popbuf_begin,
	.end_data_ptr_access = popbuf_end,
};

static int
popup_active(void)
{
	return pop.menu != NULL;
}

static void
panel_clear(PopPanel *p)
{
	free(p->ry);
	memset(p, 0, sizeof(*p));
}

static int
row_selectable(const MenuItem *mi)
{
	return !mi->separator && mi->enabled;
}

/* Sizes and row offsets of a panel; position is set by the caller. */
static void
panel_layout(PopPanel *p, MenuItem *items, int n)
{
	int i, tw, maxw = 0, y = pop.bw, W, H;
	Monitor *m = pop.mon;

	memset(p, 0, sizeof(*p));
	p->items = items;
	p->n = n;
	p->hover = -1;
	p->ry = ecalloc(n + 1, sizeof(int));
	drwl_setfont(pop.drw, m->drw->font);
	for (i = 0; i < n; i++) {
		p->ry[i] = y;
		y += items[i].separator ? pop.sepdh : pop.rowh;
		if (items[i].icon)
			p->has_icons = 1;
		if (!items[i].separator && (tw = drwl_font_getwidth(pop.drw, items[i].label)) > maxw)
			maxw = tw;
	}
	p->ry[n] = y;
	p->h = y + pop.bw;
	W = (int)(m->m.width * pop.scale);
	H = (int)(m->m.height * pop.scale);
	/* check column, optional icon column, label, gap, arrow column */
	p->w = pop.hpad + pop.fh + (p->has_icons ? pop.fh + pop.fh / 4 : 0) + pop.fh / 4
		+ maxw + pop.fh + pop.fh + pop.hpad;
	if (p->w < pop.fh * 8)
		p->w = pop.fh * 8;
	if (p->w > W)
		p->w = W;
	p->vh = p->h > H ? H : p->h;
}

static void
panel_clamp_scroll(PopPanel *p)
{
	if (p->scroll > p->h - p->vh)
		p->scroll = p->h - p->vh;
	if (p->scroll < 0)
		p->scroll = 0;
}

static void
panel_show_row(PopPanel *p, int row)
{
	if (row < 0 || p->vh >= p->h)
		return;
	if (p->ry[row] - p->scroll < pop.bw)
		p->scroll = p->ry[row] - pop.bw;
	if (p->ry[row + 1] - p->scroll > p->vh - pop.bw)
		p->scroll = p->ry[row + 1] - p->vh + pop.bw;
	panel_clamp_scroll(p);
}

/* row under panel-relative y, or -1 */
static int
panel_row_at(const PopPanel *p, int y)
{
	int i;

	y += p->scroll;
	for (i = 0; i < p->n; i++)
		if (y >= p->ry[i] && y < p->ry[i + 1])
			return i;
	return -1;
}

static int
panel_at(int px, int py)
{
	int i;

	for (i = pop.np - 1; i >= 0; i--) {
		PopPanel *p = &pop.p[i];
		if (px >= p->x && px < p->x + p->w && py >= p->y && py < p->y + p->vh)
			return i;
	}
	return -1;
}

static void
popup_draw_icon(const ThemeBitmap *bm, int x, int y, int size)
{
	pixman_image_t *src = pixman_image_create_bits(PIXMAN_a8r8g8b8, bm->w, bm->h, bm->pix, bm->w * 4);
	pixman_transform_t tr;
	double sx = (double)bm->w / size, sy = (double)bm->h / size;

	if (!src)
		return;
	pixman_transform_init_scale(&tr, pixman_double_to_fixed(sx), pixman_double_to_fixed(sy));
	pixman_image_set_transform(src, &tr);
	pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);
	pixman_image_composite32(PIXMAN_OP_OVER, src, NULL, pop.drw->image,
			0, 0, 0, 0, x, y, size, size);
	pixman_image_unref(src);
}

static void
panel_draw(const PopPanel *p, int ox, int oy)
{
	uint32_t *norm = colors[SchemeNorm], border[3], dis[3];
	pixman_region32_t clip;
	int i, x = p->x - ox, y0 = p->y - oy;
	int labelx = pop.hpad + pop.fh + pop.fh / 4 + (p->has_icons ? pop.fh + pop.fh / 4 : 0);

	border[0] = norm[ColBorder]; border[1] = norm[ColBg]; border[2] = norm[ColBorder];
	dis[0] = colors[SchemeHid][ColFg]; dis[1] = norm[ColBg]; dis[2] = norm[ColBorder];

	pixman_region32_init_rect(&clip, x, y0, p->w, p->vh);
	pixman_image_set_clip_region32(pop.drw->image, &clip);

	drwl_setscheme(pop.drw, border);
	drwl_rect(pop.drw, x, y0, p->w, p->vh, 1, 0);
	drwl_setscheme(pop.drw, norm);
	drwl_rect(pop.drw, x + pop.bw, y0 + pop.bw, p->w - 2 * pop.bw, p->vh - 2 * pop.bw, 1, 1);

	for (i = 0; i < p->n; i++) {
		const MenuItem *mi = &p->items[i];
		int ry = y0 + p->ry[i] - p->scroll;
		int rh = p->ry[i + 1] - p->ry[i];
		int rx = x + pop.bw, rw = p->w - 2 * pop.bw;
		uint32_t *sch;
		int tw;

		if (ry + rh <= y0 || ry >= y0 + p->vh)
			continue;
		if (mi->separator) {
			drwl_setscheme(pop.drw, border);
			drwl_rect(pop.drw, rx + pop.hpad / 2, ry + rh / 2, rw - pop.hpad, 1, 1, 0);
			continue;
		}
		sch = !mi->enabled ? dis : i == p->hover ? colors[SchemeSel] : norm;
		drwl_setscheme(pop.drw, sch);
		drwl_text(pop.drw, rx, ry, rw, rh, 0, "", 0); /* background */
		if (mi->toggle == 1) {
			const char *mark = mi->radio ? "●" : "✓";
			tw = drwl_font_getwidth(pop.drw, mark);
			drwl_text(pop.drw, x + pop.hpad, ry, tw + 1, rh, 0, mark, 0);
		} else if (mi->toggle == 0 && !mi->radio) {
			const char *mark = "☐";
			drwl_text(pop.drw, x + pop.hpad, ry, drwl_font_getwidth(pop.drw, mark) + 1, rh, 0, mark, 0);
		}
		if (mi->icon)
			popup_draw_icon(mi->icon, x + pop.hpad + pop.fh + pop.fh / 4,
					ry + (rh - pop.fh) / 2, pop.fh);
		drwl_text(pop.drw, x + labelx, ry, rw - labelx + pop.bw, rh, 0, mi->label, 0);
		if (mi->has_sub) {
			const char *arrow = "›";
			tw = drwl_font_getwidth(pop.drw, arrow);
			drwl_text(pop.drw, x + p->w - pop.bw - pop.hpad - tw, ry, tw + pop.hpad, rh, 0, arrow, 0);
		}
	}
	pixman_image_set_clip_region32(pop.drw->image, NULL);
	pixman_region32_fini(&clip);
}

/* Paints every open panel into a new buffer (one reference, the caller's)
 * whose top left is (*bx, *by) in monitor pixels. */
static PopBuf *
popup_paint(int *bxp, int *byp)
{
	int i, bx = 1 << 30, by = 1 << 30, ex = 0, ey = 0, bwid, bhei;
	PopBuf *pb;
	pixman_color_t clear = {0, 0, 0, 0};

	drwl_setfont(pop.drw, pop.mon->drw->font);
	for (i = 0; i < pop.np; i++) {
		PopPanel *p = &pop.p[i];
		bx = MIN(bx, p->x);
		by = MIN(by, p->y);
		ex = MAX(ex, p->x + p->w);
		ey = MAX(ey, p->y + p->vh);
	}
	bwid = ex - bx;
	bhei = ey - by;
	pb = ecalloc(1, sizeof(*pb) + (size_t)bwid * bhei * 4);
	pb->image = drwl_image_create(NULL, bwid, bhei, pb->data);
	wlr_buffer_init(&pb->base, &popbuf_impl, bwid, bhei);
	drwl_setimage(pop.drw, pb->image);
	pixman_image_fill_rectangles(PIXMAN_OP_SRC, pb->image, &clear, 1,
			&(pixman_rectangle16_t){0, 0, bwid, bhei});
	for (i = 0; i < pop.np; i++)
		panel_draw(&pop.p[i], bx, by);
	drwl_setimage(pop.drw, NULL);
	*bxp = bx;
	*byp = by;
	return pb;
}

static void
popup_render(void)
{
	int bx, by;
	PopBuf *pb;

	if (!pop.np)
		return;
	pb = popup_paint(&bx, &by);
	if (!pop.sb) {
		pop.sb = wlr_scene_buffer_create(layers[LyrOverlay], NULL);
		if (!pop.sb) {
			wlr_buffer_drop(&pb->base);
			return;
		}
	}
	wlr_scene_buffer_set_dest_size(pop.sb, (int)lround(pb->base.width / pop.scale),
			(int)lround(pb->base.height / pop.scale));
	wlr_scene_node_set_position(&pop.sb->node,
			pop.mon->m.x + (int)lround(bx / pop.scale), pop.mon->m.y + (int)lround(by / pop.scale));
	wlr_scene_buffer_set_buffer(pop.sb, &pb->base);
	wlr_buffer_drop(&pb->base);
}

static void
popup_dismiss(int restore_pointer)
{
	int i;

	if (pop.req) {
		menu_fetch_cancel(pop.req);
		pop.req = NULL;
	}
	if (!pop.menu)
		return;
	for (i = 0; i < pop.np; i++)
		panel_clear(&pop.p[i]);
	pop.np = 0;
	pop.pressed_in = 0;
	if (pop.sb) {
		wlr_scene_node_destroy(&pop.sb->node);
		pop.sb = NULL;
	}
	menu_free(pop.menu);
	pop.menu = NULL;
	pop.mon = NULL;
	/* hand the pointer back to whatever is under it */
	if (restore_pointer)
		motionnotify(0, NULL, 0, 0, 0, 0);
}

static void
popup_close(void)
{
	popup_dismiss(1);
}

static void
popup_open_sub(int pi, int row)
{
	PopPanel *par = &pop.p[pi], *p;
	MenuItem *mi = &par->items[row];
	int W = (int)(pop.mon->m.width * pop.scale), H = (int)(pop.mon->m.height * pop.scale);
	int i;

	if (!mi->has_sub || !mi->enabled || mi->nkids == 0 || pi + 1 >= POP_MAXPANELS)
		return;
	for (i = pi + 1; i < pop.np; i++)
		panel_clear(&pop.p[i]);
	pop.np = pi + 1;
	menu_about_to_show(pop.menu, mi->id);

	p = &pop.p[pop.np++];
	panel_layout(p, mi->kids, mi->nkids);
	p->x = par->x + par->w - pop.bw;
	if (p->x + p->w > W)
		p->x = par->x - p->w + pop.bw;
	if (p->x < 0)
		p->x = 0;
	p->y = par->y + par->ry[row] - par->scroll - pop.bw;
	if (p->y + p->vh > H)
		p->y = H - p->vh;
	if (p->y < 0)
		p->y = 0;
}

static void
popup_move_hover(int dir)
{
	PopPanel *p = &pop.p[pop.np - 1];
	int i, row = p->hover, n = p->n;

	for (i = 0; i < n; i++) {
		row = row < 0 ? (dir > 0 ? 0 : n - 1) : (row + dir + n) % n;
		if (row_selectable(&p->items[row])) {
			p->hover = row;
			panel_show_row(p, row);
			return;
		}
	}
}

/* Enter, Space or a click on the row */
static void
popup_choose(int pi, int row)
{
	MenuItem *mi = &pop.p[pi].items[row];

	if (!row_selectable(mi))
		return;
	if (mi->has_sub) {
		popup_open_sub(pi, row);
		if (pop.np > pi + 1) {
			pop.p[pop.np - 1].hover = -1;
			popup_move_hover(1);
		}
		return;
	}
	menu_event(pop.menu, mi->id, "clicked");
	popup_close();
}

static void
popup_motion(double lx, double ly)
{
	int px = (int)((lx - pop.mon->m.x) * pop.scale), py = (int)((ly - pop.mon->m.y) * pop.scale);
	int pi = panel_at(px, py), row, changed = 0, i;
	PopPanel *p;

	if (pi < 0)
		return;
	p = &pop.p[pi];
	row = panel_row_at(p, py - p->y);
	if (row >= 0 && !row_selectable(&p->items[row]))
		row = -1;
	if (row != p->hover) {
		p->hover = row;
		changed = 1;
	}
	if (changed) {
		/* leaving a row closes whatever it had opened */
		for (i = pi + 1; i < pop.np; i++)
			panel_clear(&pop.p[i]);
		pop.np = pi + 1;
		if (row >= 0)
			popup_open_sub(pi, row);
		popup_render();
	}
}

/* Returns 1 if the event was consumed. */
static int
popup_button(double lx, double ly, int pressed)
{
	int px = (int)((lx - pop.mon->m.x) * pop.scale), py = (int)((ly - pop.mon->m.y) * pop.scale);
	int pi = panel_at(px, py), row;

	if (pressed) {
		if (pi < 0) {
			popup_close();
			return 1;
		}
		pop.pressed_in = 1;
		return 1;
	}
	if (!pop.pressed_in)
		return 0; /* the release of the click that opened us */
	pop.pressed_in = 0;
	if (pi < 0)
		return 1;
	row = panel_row_at(&pop.p[pi], py - pop.p[pi].y);
	if (row >= 0) {
		popup_choose(pi, row);
		if (popup_active())
			popup_render();
	}
	return 1;
}

static void
popup_scroll(double lx, double ly, double delta)
{
	int px = (int)((lx - pop.mon->m.x) * pop.scale), py = (int)((ly - pop.mon->m.y) * pop.scale);
	int pi = panel_at(px, py);
	PopPanel *p;

	if (pi < 0)
		return;
	p = &pop.p[pi];
	if (p->vh >= p->h)
		return;
	p->scroll += (delta > 0 ? 1 : -1) * pop.rowh;
	panel_clamp_scroll(p);
	popup_render();
}

/* A key press; every key is swallowed while the popup is open. */
static void
popup_key(xkb_keysym_t sym)
{
	PopPanel *p = &pop.p[pop.np - 1];
	int dirty = 1;

	switch (sym) {
	case XKB_KEY_Escape:
		popup_close();
		return;
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
		popup_move_hover(1);
		break;
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
		popup_move_hover(-1);
		break;
	case XKB_KEY_Home:
		p->hover = -1;
		popup_move_hover(1);
		break;
	case XKB_KEY_End:
		p->hover = -1;
		popup_move_hover(-1);
		break;
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
		if (pop.np > 1)
			panel_clear(&pop.p[--pop.np]);
		else
			dirty = 0;
		break;
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
		if (p->hover >= 0 && p->items[p->hover].has_sub)
			popup_choose(pop.np - 1, p->hover);
		else
			dirty = 0;
		break;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space:
		if (p->hover >= 0)
			popup_choose(pop.np - 1, p->hover);
		else
			dirty = 0;
		break;
	default:
		dirty = 0;
	}
	if (dirty && popup_active())
		popup_render();
}

static void
popup_ready(Menu *menu, void *ud)
{
	Monitor *m = ud;
	PopPanel *p;
	int W, H;

	pop.req = NULL;
	if (!menu || !m->wlr_output->enabled) {
		menu_free(menu);
		return;
	}
	pop.menu = menu;
	pop.mon = m;
	pop.scale = m->wlr_output->scale;
	if (!pop.drw && !(pop.drw = drwl_create()))
		die("drwl_create:");
	drwl_setfont(pop.drw, m->drw->font);
	pop.fh = m->drw->font->height;
	pop.rowh = pop.fh + pop.fh / 2;
	pop.sepdh = pop.fh / 2;
	pop.hpad = pop.fh * 2 / 3;
	pop.bw = MAX(1, (int)lround(pop.scale));

	W = (int)(m->m.width * pop.scale);
	H = (int)(m->m.height * pop.scale);
	p = &pop.p[pop.np++];
	panel_layout(p, menu->root.kids, menu->root.nkids);
	p->x = pop.ax;
	if (p->x + p->w > W)
		p->x = W - p->w;
	if (p->x < 0)
		p->x = 0;
	if (!showbar)
		p->y = 0;
	else if (topbar)
		p->y = m->b.height;
	else
		p->y = H - m->b.height - p->vh;
	if (p->y + p->vh > H)
		p->y = H - p->vh;
	if (p->y < 0)
		p->y = 0;

	wlr_seat_pointer_notify_clear_focus(seat);
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	popup_render();
}

/* Called by the tray to show an item's menu. x is where the pointer was. */
static void
popup_open(Watcher *w, const char *busname, const char *menu_obj, const char *icondir, int x, int y)
{
	Monitor *m = xytomon(x, y);

	if (!m)
		m = selmon;
	if (!m || !m->drw || !m->drw->font)
		return;
	popup_close();
	pop.ax = (int)((x - m->m.x) * m->wlr_output->scale);
	pop.req = menu_fetch(w->conn, busname, menu_obj, icondir, m->drw->font->height, popup_ready, m);
}

/* IPC "popup" command, for scripting and tests:
 *   op=state | key name= | motion x= y= | click x= y= | scroll x= y= delta= | dump path= | close
 * Coordinates are layout coordinates, as reported by "state". */
static void
popup_ipc(IpcClient *cl, const IpcReq *req)
{
	const char *op = ipc_req_get(req, "op"), *v;
	double x = (v = ipc_req_get(req, "x")) ? atof(v) : 0, y = (v = ipc_req_get(req, "y")) ? atof(v) : 0;
	JBuf b;
	int i, j;

	if (!op || !strcmp(op, "state")) {
		jb_init(&b);
		jb_printf(&b, "{");
		jbool(&b, "open", popup_active());
		jb_key(&b, "panels");
		jb_printf(&b, "[");
		for (i = 0; popup_active() && i < pop.np; i++) {
			PopPanel *p = &pop.p[i];

			jb_sep(&b);
			jb_printf(&b, "{");
			jnum(&b, "x", pop.mon->m.x + lround(p->x / pop.scale));
			jnum(&b, "y", pop.mon->m.y + lround(p->y / pop.scale));
			jnum(&b, "w", lround(p->w / pop.scale));
			jnum(&b, "h", lround(p->vh / pop.scale));
			jnum(&b, "scroll", lround(p->scroll / pop.scale));
			jnum(&b, "hover", p->hover);
			jb_key(&b, "rows");
			jb_printf(&b, "[");
			for (j = 0; j < p->n; j++) {
				MenuItem *mi = &p->items[j];

				jb_sep(&b);
				jb_printf(&b, "{");
				jstr(&b, "label", mi->label);
				jnum(&b, "y", pop.mon->m.y + lround((p->y + p->ry[j] - p->scroll) / pop.scale));
				jnum(&b, "h", lround((p->ry[j + 1] - p->ry[j]) / pop.scale));
				jbool(&b, "separator", mi->separator);
				jbool(&b, "enabled", mi->enabled);
				jbool(&b, "submenu", mi->has_sub);
				jbool(&b, "icon", mi->icon != NULL);
				jnum(&b, "toggle", mi->toggle);
				jb_printf(&b, "}");
			}
			jb_printf(&b, "]}");
		}
		jb_printf(&b, "]}");
		ipc_reply_ok(cl, b.s);
		jb_free(&b);
		return;
	}
	if (!popup_active()) {
		ipc_reply_err(cl, "popup: no menu is open");
		return;
	}
	if (!strcmp(op, "key")) {
		xkb_keysym_t sym = (v = ipc_req_get(req, "name")) ? xkb_keysym_from_name(v, XKB_KEYSYM_NO_FLAGS) : XKB_KEY_NoSymbol;

		if (sym == XKB_KEY_NoSymbol) {
			ipc_reply_err(cl, "popup key: unknown key name");
			return;
		}
		popup_key(sym);
	} else if (!strcmp(op, "motion")) {
		popup_motion(x, y);
	} else if (!strcmp(op, "click")) {
		popup_button(x, y, 1);
		if (popup_active())
			popup_button(x, y, 0);
	} else if (!strcmp(op, "scroll")) {
		popup_scroll(x, y, (v = ipc_req_get(req, "delta")) ? atof(v) : 1);
	} else if (!strcmp(op, "close")) {
		popup_close();
	} else if (!strcmp(op, "dump")) {
		const char *path = ipc_req_get(req, "path");
		int bx, by;
		PopBuf *pb;
		cairo_surface_t *s;
		cairo_status_t st;

		if (!path || !*path) {
			ipc_reply_err(cl, "popup dump: needs a 'path'");
			return;
		}
		pb = popup_paint(&bx, &by);
		s = cairo_image_surface_create_for_data((unsigned char *)pb->data, CAIRO_FORMAT_ARGB32,
				pb->base.width, pb->base.height, pb->base.width * 4);
		st = cairo_surface_write_to_png(s, path);
		cairo_surface_destroy(s);
		wlr_buffer_drop(&pb->base);
		if (st != CAIRO_STATUS_SUCCESS) {
			ipc_reply_err(cl, "popup dump: cannot write %s", path);
			return;
		}
	} else {
		ipc_reply_err(cl, "popup: unknown op '%s'", op);
		return;
	}
	ipc_reply_ok(cl, NULL);
}
