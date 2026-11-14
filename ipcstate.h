/*
 * What nullwl exposes over IPC. Included by nullwl.c after rtconf.h.
 *
 * Commands (see handler below) answer from the live state. Events are produced
 * by diffing: the code calls ipc_changed() whenever something that might be
 * visible changed; that only marks the state dirty and schedules one idle
 * callback. The callback takes a snapshot, compares it to the previous one,
 * and emits an event per difference. A bursty change (a tag switch touches
 * dozens of things) therefore costs one pass, and a place that forgets to call
 * ipc_changed() can only delay an event, never produce a wrong one.
 */
#include "ipc.h"
#include "systray/item.h"

#define IPC_PROTOCOL 1

static Ipc *ipc;
static int ipcdirty;

/* ------------------------------------------------------------------ */
/* snapshot                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
	uint32_t id;
	char *title, *appid, *mon;
	uint32_t tags;
	int floating, fullscreen, hidden, urgent;
} CSnap;

typedef struct {
	char *name;
	uint32_t tagset, occupied, urgent;
	char sym[16];
	int nmaster;
	float mfact;
} MSnap;

typedef struct {
	CSnap *c;
	size_t nc;
	MSnap *m;
	size_t nm;
	uint32_t focus;
	int tray;
} Snap;

static Snap prevsnap;
static int havesnap;

static char *
xstrdup(const char *s)
{
	char *r = strdup(s ? s : "");
	if (!r)
		die("strdup:");
	return r;
}

static uint32_t
ipcfocusedid(void)
{
	Client *c = NULL;
	struct wlr_surface *s = seat ? seat->keyboard_state.focused_surface : NULL;

	if (s)
		toplevel_from_wlr_surface(s, &c, NULL);
	return c ? c->id : 0;
}

static void
snapfree(Snap *s)
{
	for (size_t i = 0; i < s->nc; i++) {
		free(s->c[i].title); free(s->c[i].appid); free(s->c[i].mon);
	}
	for (size_t i = 0; i < s->nm; i++)
		free(s->m[i].name);
	free(s->c);
	free(s->m);
	memset(s, 0, sizeof *s);
}

static void
snaptake(Snap *s)
{
	Client *c;
	Monitor *m;
	size_t nc = 0, nm = 0;

	memset(s, 0, sizeof *s);
	wl_list_for_each(c, &clients, link)
		nc++;
	wl_list_for_each(m, &mons, link)
		nm++;
	s->c = ecalloc(nc ? nc : 1, sizeof *s->c);
	s->m = ecalloc(nm ? nm : 1, sizeof *s->m);

	wl_list_for_each(c, &clients, link) {
		CSnap *cs = &s->c[s->nc++];
		cs->id = c->id;
		cs->title = xstrdup(client_get_title(c));
		cs->appid = xstrdup(client_get_appid(c));
		cs->mon = xstrdup(c->mon ? c->mon->wlr_output->name : NULL);
		cs->tags = c->tags;
		cs->floating = c->isfloating;
		cs->fullscreen = c->isfullscreen;
		cs->hidden = c->ishidden;
		cs->urgent = c->isurgent;
	}
	wl_list_for_each(m, &mons, link) {
		MSnap *ms = &s->m[s->nm++];
		ms->name = xstrdup(m->wlr_output->name);
		ms->tagset = m->tagset[m->seltags];
		wl_list_for_each(c, &clients, link)
			if (c->mon == m) {
				ms->occupied |= c->tags;
				if (c->isurgent)
					ms->urgent |= c->tags;
			}
		snprintf(ms->sym, sizeof ms->sym, "%s", m->ltsymbol);
		ms->nmaster = m->nmaster;
		ms->mfact = m->mfact;
	}
	s->focus = ipcfocusedid();
	s->tray = watcher_get_n_items(&watcher);
}

/* ------------------------------------------------------------------ */
/* JSON for the things we describe                                     */
/* ------------------------------------------------------------------ */
static void
jbool(JBuf *b, const char *key, int v)
{
	jb_key(b, key);
	jb_printf(b, v ? "true" : "false");
}

static void
jnum(JBuf *b, const char *key, long v)
{
	jb_key(b, key);
	jb_printf(b, "%ld", v);
}

static void
jstr(JBuf *b, const char *key, const char *v)
{
	jb_key(b, key);
	jb_str(b, v);
}

static void
jclient(JBuf *b, const Client *c, uint32_t focusid)
{
	jb_printf(b, "{");
	jnum(b, "id", c->id);
	jstr(b, "app_id", client_get_appid((Client *)c));
	jstr(b, "title", client_get_title((Client *)c));
	jnum(b, "pid", c->pid);
	jstr(b, "monitor", c->mon ? c->mon->wlr_output->name : NULL);
	jnum(b, "tags", c->tags);
	jbool(b, "floating", c->isfloating);
	jbool(b, "fullscreen", c->isfullscreen);
	jbool(b, "hidden", c->ishidden);
	jbool(b, "urgent", c->isurgent);
	jbool(b, "focused", c->id && c->id == focusid);
	jbool(b, "xwayland", c->type == X11);
	jnum(b, "x", c->geom.x);
	jnum(b, "y", c->geom.y);
	jnum(b, "width", c->geom.width);
	jnum(b, "height", c->geom.height);
	jb_printf(b, "}");
}

static const char *
layoutname(const Layout *l)
{
	return l >= layouts && l < layouts + LENGTH(layouts) ? en_layout[l - layouts].name : NULL;
}

static void
jmonitor(JBuf *b, Monitor *m)
{
	Client *c;
	uint32_t occupied = 0, urgent = 0;

	wl_list_for_each(c, &clients, link)
		if (c->mon == m) {
			occupied |= c->tags;
			if (c->isurgent)
				urgent |= c->tags;
		}
	jb_printf(b, "{");
	jstr(b, "name", m->wlr_output->name);
	jbool(b, "focused", m == selmon);
	jnum(b, "x", m->m.x);
	jnum(b, "y", m->m.y);
	jnum(b, "width", m->m.width);
	jnum(b, "height", m->m.height);
	jb_key(b, "scale");
	jb_printf(b, "%g", m->wlr_output->scale);
	jnum(b, "tagset", m->tagset[m->seltags]);
	jnum(b, "occupied", occupied);
	jnum(b, "urgent", urgent);
	jb_key(b, "tags");
	jb_printf(b, "[");
	for (size_t i = 0; i < LENGTH(tags); i++) {
		jb_sep(b);
		jb_str(b, tags[i]);
	}
	jb_printf(b, "]");
	jstr(b, "layout", layoutname(m->lt[m->sellt]));
	jstr(b, "symbol", m->ltsymbol);
	jnum(b, "nmaster", m->nmaster);
	jb_key(b, "mfact");
	jb_printf(b, "%g", m->mfact);
	jb_key(b, "gaps");
	jb_printf(b, "{");
	jnum(b, "inner_h", m->gappih);
	jnum(b, "inner_v", m->gappiv);
	jnum(b, "outer_h", m->gappoh);
	jnum(b, "outer_v", m->gappov);
	jb_printf(b, "}}");
}

static void
joptionvalue(JBuf *b, const Option *o)
{
	char col[16];
	const EnumName *e;
	int v;

	switch (o->type) {
	case OptBool:   jb_printf(b, *(int *)o->ptr ? "true" : "false"); break;
	case OptInt:    jb_printf(b, "%d", *(int *)o->ptr); break;
	case OptUInt:   jb_printf(b, "%u", *(unsigned *)o->ptr); break;
	case OptFloat:  jb_printf(b, "%g", (double)*(float *)o->ptr); break;
	case OptDouble: jb_printf(b, "%g", *(double *)o->ptr); break;
	case OptColor:
		snprintf(col, sizeof col, "#%08x", *(uint32_t *)o->ptr);
		jb_str(b, col);
		break;
	case OptColorF: {
		const float *f = o->ptr;
		snprintf(col, sizeof col, "#%02x%02x%02x%02x", (int)(f[0] * 255 + .5f), (int)(f[1] * 255 + .5f),
				(int)(f[2] * 255 + .5f), (int)(f[3] * 255 + .5f));
		jb_str(b, col);
		break;
	}
	case OptEnum:
		v = *(int *)o->ptr;
		for (e = o->names; e->name; e++)
			if (e->val == v) { jb_str(b, e->name); return; }
		jb_printf(b, "%d", v);
		break;
	case OptStr:
		jb_str(b, *(char **)o->ptr);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */
static void
emit(const char *name, JBuf *b)
{
	ipc_emit(ipc, name, b->s);
	jb_free(b);
}

static const CSnap *
findc(const Snap *s, uint32_t id)
{
	for (size_t i = 0; i < s->nc; i++)
		if (s->c[i].id == id)
			return &s->c[i];
	return NULL;
}

static const MSnap *
findm(const Snap *s, const char *name)
{
	for (size_t i = 0; i < s->nm; i++)
		if (!strcmp(s->m[i].name, name))
			return &s->m[i];
	return NULL;
}

static Client *
clientbyid(uint32_t id)
{
	Client *c;

	if (!id)
		return NULL;
	wl_list_for_each(c, &clients, link)
		if (c->id == id)
			return c;
	return NULL;
}

static void
ipcdiff(const Snap *old, const Snap *cur)
{
	JBuf b;

	for (size_t i = 0; i < cur->nm; i++)
		if (!findm(old, cur->m[i].name)) {
			jb_init(&b); jb_printf(&b, "{"); jstr(&b, "name", cur->m[i].name); jb_printf(&b, "}");
			emit("monitor-add", &b);
		}
	for (size_t i = 0; i < old->nm; i++)
		if (!findm(cur, old->m[i].name)) {
			jb_init(&b); jb_printf(&b, "{"); jstr(&b, "name", old->m[i].name); jb_printf(&b, "}");
			emit("monitor-remove", &b);
		}

	for (size_t i = 0; i < cur->nc; i++) {
		const CSnap *n = &cur->c[i], *o = findc(old, n->id);
		Client *c = clientbyid(n->id);

		if (!c)
			continue;
		if (!o) {
			jb_init(&b); jclient(&b, c, cur->focus); emit("window-open", &b);
			continue;
		}
		if (strcmp(o->title, n->title)) {
			jb_init(&b); jb_printf(&b, "{"); jnum(&b, "id", n->id); jstr(&b, "title", n->title); jb_printf(&b, "}");
			emit("title", &b);
		}
		if (o->tags != n->tags || o->floating != n->floating || o->fullscreen != n->fullscreen
				|| o->hidden != n->hidden || o->urgent != n->urgent || strcmp(o->mon, n->mon)) {
			jb_init(&b); jclient(&b, c, cur->focus); emit("window-state", &b);
		}
	}
	for (size_t i = 0; i < old->nc; i++)
		if (!findc(cur, old->c[i].id)) {
			jb_init(&b); jb_printf(&b, "{"); jnum(&b, "id", old->c[i].id); jb_printf(&b, "}");
			emit("window-close", &b);
		}

	for (size_t i = 0; i < cur->nm; i++) {
		const MSnap *n = &cur->m[i], *o = findm(old, n->name);
		if (!o)
			continue;
		if (o->tagset != n->tagset || o->occupied != n->occupied || o->urgent != n->urgent) {
			jb_init(&b); jb_printf(&b, "{");
			jstr(&b, "monitor", n->name); jnum(&b, "tagset", n->tagset);
			jnum(&b, "occupied", n->occupied); jnum(&b, "urgent", n->urgent);
			jb_printf(&b, "}");
			emit("tags", &b);
		}
		if (strcmp(o->sym, n->sym) || o->nmaster != n->nmaster || o->mfact != n->mfact) {
			jb_init(&b); jb_printf(&b, "{");
			jstr(&b, "monitor", n->name); jstr(&b, "symbol", n->sym); jnum(&b, "nmaster", n->nmaster);
			jb_key(&b, "mfact"); jb_printf(&b, "%g", n->mfact);
			jb_printf(&b, "}");
			emit("layout", &b);
		}
	}

	if (old->focus != cur->focus) {
		Client *c = clientbyid(cur->focus);
		jb_init(&b);
		if (c)
			jclient(&b, c, cur->focus);
		else
			jb_printf(&b, "null");
		emit("focus", &b);
	}
	if (old->tray != cur->tray) {
		jb_init(&b); jb_printf(&b, "{"); jnum(&b, "items", cur->tray); jb_printf(&b, "}");
		emit("tray", &b);
	}
}

static void
ipcflush(void *data)
{
	Snap cur;

	ipcdirty = 0;
	if (!ipc || !ipc_nsubscribers(ipc))
		return;
	snaptake(&cur);
	if (havesnap)
		ipcdiff(&prevsnap, &cur);
	snapfree(&prevsnap);
	prevsnap = cur;
	havesnap = 1;
}

/* Something visible may have changed. Cheap: safe to call from hot paths. */
static void
ipc_changed(void)
{
	if (ipcdirty || !ipc || !ipc_nsubscribers(ipc))
		return;
	ipcdirty = 1;
	wl_event_loop_add_idle(event_loop, ipcflush, NULL);
}

static void
ipc_config_reloaded(int errors)
{
	JBuf b;

	if (!ipc || !ipc_nsubscribers(ipc))
		return;
	jb_init(&b);
	jb_printf(&b, "{");
	jnum(&b, "errors", errors > 0 ? errors : 0);
	jb_printf(&b, "}");
	emit("config-reload", &b);
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */
static void
jerrors(JBuf *b)
{
	jb_printf(b, "[");
	for (int i = 0; i < conf_errors_count(); i++) {
		jb_sep(b);
		jb_str(b, conf_errors_get(i));
	}
	jb_printf(b, "]");
}

static void
cmdget(IpcClient *cl, const IpcReq *req)
{
	const char *what = ipc_req_get(req, "what");
	uint32_t focus = ipcfocusedid();
	JBuf b;
	Client *c;
	Monitor *m;
	Item *it;

	if (!what) {
		ipc_reply_err(cl, "get: missing 'what' (monitors, clients, focused, tray, errors, options, actions)");
		return;
	}
	jb_init(&b);
	if (!strcmp(what, "monitors")) {
		jb_printf(&b, "[");
		wl_list_for_each(m, &mons, link) { jb_sep(&b); jmonitor(&b, m); }
		jb_printf(&b, "]");
	} else if (!strcmp(what, "clients")) {
		jb_printf(&b, "[");
		wl_list_for_each(c, &clients, link) { jb_sep(&b); jclient(&b, c, focus); }
		jb_printf(&b, "]");
	} else if (!strcmp(what, "focused")) {
		if ((c = clientbyid(focus)))
			jclient(&b, c, focus);
		else
			jb_printf(&b, "null");
	} else if (!strcmp(what, "tray")) {
		jb_printf(&b, "[");
		wl_list_for_each(it, &watcher.items, link) {
			jb_sep(&b);
			jb_printf(&b, "{");
			jstr(&b, "bus_name", it->busname);
			jstr(&b, "object", it->busobj);
			jstr(&b, "app_id", it->appid);
			jbool(&b, "ready", it->ready);
			jb_printf(&b, "}");
		}
		jb_printf(&b, "]");
	} else if (!strcmp(what, "errors")) {
		jb_printf(&b, "{");
		jstr(&b, "file", cfgpath);
		jb_key(&b, "errors");
		jerrors(&b);
		jb_printf(&b, "}");
	} else if (!strcmp(what, "options")) {
		jb_printf(&b, "[");
		for (size_t i = 0; i < LENGTH(options); i++) {
			jb_sep(&b);
			jb_printf(&b, "{");
			jstr(&b, "name", options[i].name);
			jb_key(&b, "value");
			joptionvalue(&b, &options[i]);
			jbool(&b, "restart", options[i].fx & FxRestart);
			jb_printf(&b, "}");
		}
		jb_printf(&b, "]");
	} else if (!strcmp(what, "actions")) {
		jb_printf(&b, "[");
		for (size_t i = 0; i < LENGTH(actions); i++) {
			jb_sep(&b);
			jb_str(&b, actions[i].name);
		}
		jb_printf(&b, "]");
	} else {
		ipc_reply_err(cl, "get: unknown '%s'", what);
		jb_free(&b);
		return;
	}
	ipc_reply_ok(cl, b.s);
	jb_free(&b);
}

static void
cmddispatch(IpcClient *cl, const IpcReq *req)
{
	const char *name = ipc_req_get(req, "action"), *arg = ipc_req_get(req, "arg");
	const ActionDef *a;
	char err[200], *argcopy;
	ConfPos pos = { "ipc", 0 };
	Arg parsed;
	int bad;

	if (!name) {
		ipc_reply_err(cl, "dispatch: missing 'action'");
		return;
	}
	if (!(a = findaction(name))) {
		ipc_reply_err(cl, "dispatch: unknown action '%s' (try: get actions)", name);
		return;
	}
	argcopy = xstrdup(arg);
	conf_capture(err, sizeof err);
	bad = parseargument(a, argcopy, &parsed, &pos);
	conf_capture(NULL, 0);
	free(argcopy);
	if (bad) {
		ipc_reply_err(cl, "%s", err);
		return;
	}
	ipc_reply_ok(cl, NULL); /* before running it: some actions (quit) end the session */
	a->func(&parsed);
}

static void
cmdset(IpcClient *cl, const IpcReq *req)
{
	const char *name = ipc_req_get(req, "option"), *val = ipc_req_get(req, "value");
	const Option *o = NULL;
	unsigned char tmp[16];
	char err[200], *valcopy;
	ConfPos pos = { "ipc", 0 };
	int bad;

	if (!name || !val) {
		ipc_reply_err(cl, "set: needs 'option' and 'value'");
		return;
	}
	for (size_t i = 0; i < LENGTH(options); i++)
		if (!strcmp(options[i].name, name))
			o = &options[i];
	if (!o) {
		ipc_reply_err(cl, "set: unknown option '%s' (try: get options)", name);
		return;
	}
	if (o->fx & FxRestart) {
		ipc_reply_err(cl, "set: '%s' is only read at startup", name);
		return;
	}
	valcopy = xstrdup(val);
	conf_capture(err, sizeof err);
	bad = parseoption(o, valcopy, tmp, &pos);
	conf_capture(NULL, 0);
	free(valcopy);
	if (bad) {
		ipc_reply_err(cl, "%s", err);
		return;
	}
	memcpy(o->ptr, tmp, optsize(o->type));
	applyconfig();
	ipc_reply_ok(cl, NULL);
}

static void
cmdfocus(IpcClient *cl, const IpcReq *req, int close)
{
	const char *ids = ipc_req_get(req, "id");
	long id;
	Client *c;

	if (!ids || conf_parse_long(ids, 1, 0x7fffffff, &id)) {
		ipc_reply_err(cl, "needs a window 'id' (see: get clients)");
		return;
	}
	if (!(c = clientbyid((uint32_t)id))) {
		ipc_reply_err(cl, "no window with id %ld", id);
		return;
	}
	if (close) {
		client_send_close(c);
	} else if (c->mon) {
		if (c->ishidden) {
			c->ishidden = 0;
			arrange(c->mon);
		}
		selmon = c->mon;
		if (!VISIBLEON(c, c->mon)) {
			Arg a = { .ui = c->tags & TAGMASK };
			view(&a);
		}
		focusclient(c, 1);
	}
	ipc_reply_ok(cl, NULL);
}

static void
ipchandler(IpcClient *cl, const IpcReq *req, void *data)
{
	const char *cmd = ipc_req_get(req, "cmd");
	JBuf b;

	if (!cmd) {
		ipc_reply_err(cl, "missing 'cmd'");
	} else if (!strcmp(cmd, "version")) {
		jb_init(&b);
		jb_printf(&b, "{");
		jstr(&b, "name", "nullwl");
		jstr(&b, "version", VERSION);
		jnum(&b, "protocol", IPC_PROTOCOL);
		jb_printf(&b, "}");
		ipc_reply_ok(cl, b.s);
		jb_free(&b);
	} else if (!strcmp(cmd, "get")) {
		cmdget(cl, req);
	} else if (!strcmp(cmd, "dispatch")) {
		cmddispatch(cl, req);
	} else if (!strcmp(cmd, "set")) {
		cmdset(cl, req);
	} else if (!strcmp(cmd, "focus")) {
		cmdfocus(cl, req, 0);
	} else if (!strcmp(cmd, "close")) {
		cmdfocus(cl, req, 1);
	} else if (!strcmp(cmd, "reload")) {
		int errors = loadconfig(1);
		applyconfig();
		ipc_config_reloaded(errors);
		jb_init(&b);
		jb_printf(&b, "{");
		jnum(&b, "errors", errors > 0 ? errors : 0);
		jb_key(&b, "messages");
		jerrors(&b);
		jb_printf(&b, "}");
		ipc_reply_ok(cl, b.s);
		jb_free(&b);
	} else if (!strcmp(cmd, "subscribe")) {
		if (!ipc_nsubscribers(ipc)) { /* first one: the baseline to diff against */
			snapfree(&prevsnap);
			snaptake(&prevsnap);
			havesnap = 1;
		}
		ipc_subscribe(cl, ipc_req_get(req, "events"));
		ipc_reply_ok(cl, NULL);
	} else {
		ipc_reply_err(cl, "unknown command '%s'", cmd);
	}
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */
static void
ipcstart(const char *display)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[256];

	if (!rt)
		return;
	snprintf(path, sizeof path, "%s/nullwl.%s.sock", rt, display);
	if (!(ipc = ipc_create(event_loop, path, ipchandler, NULL))) {
		fprintf(stderr, "nullwl: ipc disabled\n");
		return;
	}
	setenv("NULLWL_SOCKET", path, 1); /* inherited by everything we start */
}

static void
ipcstop(void)
{
	Ipc *i = ipc;

	ipc = NULL; /* hooks fired during teardown must not touch it */
	ipc_destroy(i);
	snapfree(&prevsnap);
}
