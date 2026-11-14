/*
 * Runtime configuration. Included by nullwl.c right after config.h, so it
 * edits the same statics.
 *
 * config.h        built-in defaults (always valid, always boots)
 * nullwl.conf     overrides, re-read on reload
 *
 * loadconfig() resets everything to the built-in defaults and then applies the
 * file, so a line that is deleted from the file really goes away on reload.
 * Bad lines are logged as "file:line: message" and skipped; the rest applies.
 */
#include "conf.h"

static void reloadconfig(const Arg *arg);

/* ------------------------------------------------------------------ */
/* arena: everything the config file allocates, freed on every reload  */
/* ------------------------------------------------------------------ */
static void **cfgmem;
static size_t ncfgmem, capcfgmem;

static void *
cfgalloc(size_t n)
{
	void *p = ecalloc(1, n ? n : 1);
	if (ncfgmem == capcfgmem) {
		capcfgmem = capcfgmem ? capcfgmem * 2 : 64;
		if (!(cfgmem = realloc(cfgmem, capcfgmem * sizeof(*cfgmem))))
			die("realloc:");
	}
	cfgmem[ncfgmem++] = p;
	return p;
}

static char *
cfgstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	return memcpy(cfgalloc(n), s, n);
}

static void
cfgfreeall(void)
{
	while (ncfgmem)
		free(cfgmem[--ncfgmem]);
}

/* growable arrays living in the arena */
#define CFG_APPEND(arr, n, cap, item) do { \
	if ((n) == (cap)) { \
		size_t _nc = (cap) ? (cap) * 2 : 16; \
		__typeof__(arr) _na = cfgalloc(_nc * sizeof(*(arr))); \
		if (n) memcpy(_na, (arr), (n) * sizeof(*(arr))); \
		(arr) = _na; (cap) = _nc; \
	} \
	(arr)[(n)++] = (item); \
} while (0)

/* ------------------------------------------------------------------ */
/* options                                                             */
/* ------------------------------------------------------------------ */
enum { OptBool, OptInt, OptUInt, OptFloat, OptDouble, OptColor, OptColorF, OptEnum, OptStr };
enum { FxNone = 0, FxRestart = 1 << 0 }; /* FxRestart: only read at startup */

typedef struct { const char *name; int val; } EnumName;
typedef struct {
	const char *name;
	int type;
	void *ptr;
	unsigned fx;
	double lo, hi;          /* range for numeric types */
	const EnumName *names;  /* for OptEnum, NULL terminated */
} Option;

static const EnumName en_scroll[] = {
	{"none", LIBINPUT_CONFIG_SCROLL_NO_SCROLL}, {"2fg", LIBINPUT_CONFIG_SCROLL_2FG},
	{"edge", LIBINPUT_CONFIG_SCROLL_EDGE}, {"button", LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN}, {NULL, 0}};
static const EnumName en_click[] = {
	{"none", LIBINPUT_CONFIG_CLICK_METHOD_NONE}, {"buttonareas", LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS},
	{"clickfinger", LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER}, {NULL, 0}};
static const EnumName en_sendevents[] = {
	{"enabled", LIBINPUT_CONFIG_SEND_EVENTS_ENABLED}, {"disabled", LIBINPUT_CONFIG_SEND_EVENTS_DISABLED},
	{"disabled-on-external-mouse", LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE}, {NULL, 0}};
static const EnumName en_accel[] = {
	{"flat", LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT}, {"adaptive", LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE}, {NULL, 0}};
static const EnumName en_tapmap[] = {
	{"lrm", LIBINPUT_CONFIG_TAP_MAP_LRM}, {"lmr", LIBINPUT_CONFIG_TAP_MAP_LMR}, {NULL, 0}};
static const EnumName en_loglevel[] = {
	{"silent", WLR_SILENT}, {"error", WLR_ERROR}, {"info", WLR_INFO}, {"debug", WLR_DEBUG}, {NULL, 0}};

#define OPT_COLOR(n, scheme, col) { n, OptColor, &colors[scheme][col], FxNone, 0, 0, NULL }
static const Option options[] = {
	/* behaviour */
	{ "sloppyfocus",              OptBool,   &sloppyfocus,              FxNone,    0, 1, NULL },
	{ "bypass_surface_visibility",OptBool,   &bypass_surface_visibility,FxNone,    0, 1, NULL },
	{ "autoswallow",              OptBool,   &enableautoswallow,        FxNone,    0, 1, NULL },
	{ "swallowborder",            OptFloat,  &swallowborder,            FxNone,    0, 10, NULL },
	{ "log_level",                OptEnum,   &log_level,                FxNone,    0, 0, en_loglevel },
	/* look */
	{ "borderpx",                 OptUInt,   &borderpx,                 FxNone,    0, 50, NULL },
	{ "gap_inner_h",              OptUInt,   &gappih,                   FxNone,    0, 500, NULL },
	{ "gap_inner_v",              OptUInt,   &gappiv,                   FxNone,    0, 500, NULL },
	{ "gap_outer_h",              OptUInt,   &gappoh,                   FxNone,    0, 500, NULL },
	{ "gap_outer_v",              OptUInt,   &gappov,                   FxNone,    0, 500, NULL },
	{ "smartgaps",                OptBool,   &smartgaps,                FxNone,    0, 1, NULL },
	{ "monoclegaps",              OptBool,   &monoclegaps,              FxNone,    0, 1, NULL },
	{ "rootcolor",                OptColorF, rootcolor,                 FxNone,    0, 0, NULL },
	{ "fullscreen_bg",            OptColorF, fullscreen_bg,             FxNone,    0, 0, NULL },
	OPT_COLOR("color.norm.fg",     SchemeNorm, ColFg),
	OPT_COLOR("color.norm.bg",     SchemeNorm, ColBg),
	OPT_COLOR("color.norm.border", SchemeNorm, ColBorder),
	OPT_COLOR("color.sel.fg",      SchemeSel,  ColFg),
	OPT_COLOR("color.sel.bg",      SchemeSel,  ColBg),
	OPT_COLOR("color.sel.border",  SchemeSel,  ColBorder),
	OPT_COLOR("color.hid.fg",      SchemeHid,  ColFg),
	OPT_COLOR("color.hid.bg",      SchemeHid,  ColBg),
	OPT_COLOR("color.hid.border",  SchemeHid,  ColBorder),
	OPT_COLOR("color.urg.fg",      SchemeUrg,  ColFg),
	OPT_COLOR("color.urg.bg",      SchemeUrg,  ColBg),
	OPT_COLOR("color.urg.border",  SchemeUrg,  ColBorder),
	/* bar and tray (read at startup) */
	{ "font",                     OptStr,    &fonts[0],                 FxRestart, 0, 0, NULL },
	{ "showbar",                  OptBool,   &showbar,                  FxRestart, 0, 1, NULL },
	{ "topbar",                   OptBool,   &topbar,                   FxRestart, 0, 1, NULL },
	{ "showsystray",              OptBool,   &showsystray,              FxRestart, 0, 1, NULL },
	{ "systray_spacing",          OptUInt,   &systrayspacing,           FxRestart, 0, 100, NULL },
	{ "cursor_size",              OptUInt,   &cursor_size,              FxRestart, 1, 512, NULL },
	/* keyboard and pointer */
	{ "repeat_rate",              OptInt,    &repeat_rate,              FxNone,    0, 1000, NULL },
	{ "repeat_delay",             OptInt,    &repeat_delay,             FxNone,    0, 10000, NULL },
	{ "tap_to_click",             OptBool,   &tap_to_click,             FxNone,    0, 1, NULL },
	{ "tap_and_drag",             OptBool,   &tap_and_drag,             FxNone,    0, 1, NULL },
	{ "drag_lock",                OptBool,   &drag_lock,                FxNone,    0, 1, NULL },
	{ "natural_scrolling",        OptBool,   &natural_scrolling,        FxNone,    0, 1, NULL },
	{ "disable_while_typing",     OptBool,   &disable_while_typing,     FxNone,    0, 1, NULL },
	{ "left_handed",              OptBool,   &left_handed,              FxNone,    0, 1, NULL },
	{ "middle_button_emulation",  OptBool,   &middle_button_emulation,  FxNone,    0, 1, NULL },
	{ "scroll_method",            OptEnum,   &scroll_method,            FxNone,    0, 0, en_scroll },
	{ "click_method",             OptEnum,   &click_method,             FxNone,    0, 0, en_click },
	{ "send_events_mode",         OptEnum,   &send_events_mode,         FxNone,    0, 0, en_sendevents },
	{ "accel_profile",            OptEnum,   &accel_profile,            FxNone,    0, 0, en_accel },
	{ "accel_speed",              OptDouble, &accel_speed,              FxNone,    -1, 1, NULL },
	{ "tap_button_map",           OptEnum,   &button_map,               FxNone,    0, 0, en_tapmap },
};

static size_t
optsize(int type)
{
	switch (type) {
	case OptDouble: return sizeof(double);
	case OptColorF: return 4 * sizeof(float);
	case OptStr:    return sizeof(char *);
	default:        return 4; /* int, unsigned, float, uint32_t and enums are all 4 bytes */
	}
}

static unsigned char optdefaults[LENGTH(options)][16];
static char *tagdefaults[LENGTH(tags)];
static int optsnapshot;

/* parse val for option o into out (at least 16 bytes). 0 on success */
static int
parseoption(const Option *o, char *val, unsigned char *out, const ConfPos *pos)
{
	long l; double d; uint32_t c; int b; const EnumName *e;
	float f;

	switch (o->type) {
	case OptBool:
		if (conf_parse_bool(val, &b)) { conf_error(pos, "%s: expected true/false, got '%s'", o->name, val); return -1; }
		memcpy(out, &b, sizeof b);
		return 0;
	case OptInt:
		if (conf_parse_long(val, (long)o->lo, (long)o->hi, &l)) { conf_error(pos, "%s: expected integer %ld..%ld, got '%s'", o->name, (long)o->lo, (long)o->hi, val); return -1; }
		b = (int)l;
		memcpy(out, &b, sizeof b);
		return 0;
	case OptUInt:
		if (conf_parse_long(val, (long)o->lo, (long)o->hi, &l)) { conf_error(pos, "%s: expected integer %ld..%ld, got '%s'", o->name, (long)o->lo, (long)o->hi, val); return -1; }
		{ unsigned u = (unsigned)l; memcpy(out, &u, sizeof u); }
		return 0;
	case OptFloat:
		if (conf_parse_double(val, o->lo, o->hi, &d)) { conf_error(pos, "%s: expected number %g..%g, got '%s'", o->name, o->lo, o->hi, val); return -1; }
		f = (float)d;
		memcpy(out, &f, sizeof f);
		return 0;
	case OptDouble:
		if (conf_parse_double(val, o->lo, o->hi, &d)) { conf_error(pos, "%s: expected number %g..%g, got '%s'", o->name, o->lo, o->hi, val); return -1; }
		memcpy(out, &d, sizeof d);
		return 0;
	case OptColor:
		if (conf_parse_color(val, &c)) { conf_error(pos, "%s: expected #rrggbb or #rrggbbaa, got '%s'", o->name, val); return -1; }
		memcpy(out, &c, sizeof c);
		return 0;
	case OptColorF: {
		float rgba[4];
		if (conf_parse_color(val, &c)) { conf_error(pos, "%s: expected #rrggbb or #rrggbbaa, got '%s'", o->name, val); return -1; }
		rgba[0] = ((c >> 24) & 0xff) / 255.0f; rgba[1] = ((c >> 16) & 0xff) / 255.0f;
		rgba[2] = ((c >> 8) & 0xff) / 255.0f;  rgba[3] = (c & 0xff) / 255.0f;
		memcpy(out, rgba, sizeof rgba);
		return 0;
	}
	case OptEnum:
		for (e = o->names; e->name; e++)
			if (!strcasecmp(val, e->name)) {
				memcpy(out, &e->val, sizeof e->val);
				return 0;
			}
		conf_error(pos, "%s: unknown value '%s'", o->name, val);
		return -1;
	case OptStr: {
		char *s = strdup(val);
		if (!s) die("strdup:");
		memcpy(out, &s, sizeof s);
		return 0;
	}
	}
	return -1;
}

static int
setoption(const Option *o, char *val, const ConfPos *pos, int reload)
{
	unsigned char tmp[16];

	if (parseoption(o, val, tmp, pos))
		return -1;
	if (reload && (o->fx & FxRestart)) {
		if (memcmp(tmp, o->ptr, optsize(o->type)) && o->type != OptStr)
			conf_error(pos, "%s changed: needs a restart to take effect", o->name);
		else if (o->type == OptStr) {
			char *old = *(char **)o->ptr, *new = *(char **)tmp;
			if (strcmp(old, new))
				conf_error(pos, "%s changed: needs a restart to take effect", o->name);
			free(new);
		}
		return 0;
	}
	memcpy(o->ptr, tmp, optsize(o->type));
	return 0;
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */
enum { ArgNone, ArgInt, ArgUInt, ArgFloat, ArgTag, ArgShell, ArgLayout, ArgDir, ArgCursor };

typedef struct {
	const char *name;
	void (*func)(const Arg *);
	int argtype;
} ActionDef;

static const ActionDef actions[] = {
	{ "spawn",             spawn,             ArgShell },
	{ "zoom",              zoom,              ArgNone },
	{ "togglebar",         togglebar,         ArgNone },
	{ "focusstackvis",     focusstackvis,     ArgInt },
	{ "focusstackhid",     focusstackhid,     ArgInt },
	{ "hide",              hide,              ArgNone },
	{ "show",              show,              ArgNone },
	{ "showall",           showall,           ArgNone },
	{ "movestack",         movestack,         ArgInt },
	{ "incnmaster",        incnmaster,        ArgInt },
	{ "setmfact",          setmfact,          ArgFloat },
	{ "incgaps",           incgaps,           ArgInt },
	{ "togglegaps",        togglegaps,        ArgNone },
	{ "defaultgaps",       defaultgaps,       ArgNone },
	{ "view",              view,              ArgTag },
	{ "toggleview",        toggleview,        ArgTag },
	{ "tag",               tag,               ArgTag },
	{ "toggletag",         toggletag,         ArgTag },
	{ "killclient",        killclient,        ArgNone },
	{ "addscratchpad",     addscratchpad,     ArgNone },
	{ "togglescratchpad",  togglescratchpad,  ArgNone },
	{ "removescratchpad",  removescratchpad,  ArgNone },
	{ "setscratchpad",     setscratchpad,     ArgInt },
	{ "setlayout",         setlayout,         ArgLayout },
	{ "togglefloating",    togglefloating,    ArgNone },
	{ "togglefullscreen",  togglefullscreen,  ArgNone },
	{ "setruleisfloating", setruleisfloating, ArgNone },
	{ "toggleswallow",     toggleswallow,     ArgNone },
	{ "toggleautoswallow", toggleautoswallow, ArgNone },
	{ "focusmon",          focusmon,          ArgDir },
	{ "tagmon",            tagmon,            ArgDir },
	{ "quit",              quit,              ArgNone },
	{ "chvt",              chvt,              ArgUInt },
	{ "reload",            reloadconfig,      ArgNone },
	/* mouse / bar actions */
	{ "togglewin",         togglewin,         ArgNone },
	{ "moveresize",        moveresize,        ArgCursor },
	{ "trayactivate",      trayactivate,      ArgNone },
	{ "traymenu",          traymenu,          ArgNone },
};

static const EnumName en_dir[] = {
	{"left", WLR_DIRECTION_LEFT}, {"right", WLR_DIRECTION_RIGHT},
	{"up", WLR_DIRECTION_UP}, {"down", WLR_DIRECTION_DOWN}, {NULL, 0}};
static const EnumName en_cursor[] = {{"move", CurMove}, {"resize", CurResize}, {NULL, 0}};
static const EnumName en_layout[] = {{"tile", 0}, {"floating", 1}, {"monocle", 2}, {NULL, 0}};

static const ActionDef *
findaction(const char *name)
{
	for (size_t i = 0; i < LENGTH(actions); i++)
		if (!strcmp(actions[i].name, name))
			return &actions[i];
	return NULL;
}

static int
enumlookup(const EnumName *e, const char *s, int *out)
{
	for (; e->name; e++)
		if (!strcasecmp(s, e->name)) { *out = e->val; return 0; }
	return -1;
}

static int
parseargument(const ActionDef *a, char *s, Arg *arg, const ConfPos *pos)
{
	long l; double d; int v; char **argv;

	memset(arg, 0, sizeof *arg);
	switch (a->argtype) {
	case ArgNone:
		return 0;
	case ArgInt:
		if (conf_parse_long(s, -1000000, 1000000, &l)) { conf_error(pos, "%s: expected an integer, got '%s'", a->name, s); return -1; }
		arg->i = (int)l;
		return 0;
	case ArgUInt:
		if (conf_parse_long(s, 0, 1000000, &l)) { conf_error(pos, "%s: expected a positive integer, got '%s'", a->name, s); return -1; }
		arg->ui = (uint32_t)l;
		return 0;
	case ArgFloat:
		if (conf_parse_double(s, -1000, 1000, &d)) { conf_error(pos, "%s: expected a number, got '%s'", a->name, s); return -1; }
		arg->f = (float)d;
		return 0;
	case ArgTag:
		if (!*s)
			return 0; /* no argument: previous tagset / the clicked tag */
		if (!strcasecmp(s, "all")) { arg->ui = ~0u; return 0; }
		if (conf_parse_long(s, 1, (long)LENGTH(tags), &l)) { conf_error(pos, "%s: expected a tag 1..%zu or 'all', got '%s'", a->name, LENGTH(tags), s); return -1; }
		arg->ui = 1u << (l - 1);
		return 0;
	case ArgShell:
		if (!*s) { conf_error(pos, "%s: missing command", a->name); return -1; }
		argv = cfgalloc(4 * sizeof(*argv));
		argv[0] = "/bin/sh"; argv[1] = "-c"; argv[2] = cfgstrdup(s); argv[3] = NULL;
		arg->v = argv;
		return 0;
	case ArgLayout:
		if (!*s)
			return 0; /* toggle between the two last layouts */
		if (enumlookup(en_layout, s, &v)) { conf_error(pos, "%s: unknown layout '%s' (tile, floating, monocle)", a->name, s); return -1; }
		arg->v = &layouts[v];
		return 0;
	case ArgDir:
		if (enumlookup(en_dir, s, &v)) { conf_error(pos, "%s: expected left/right/up/down, got '%s'", a->name, s); return -1; }
		arg->i = v;
		return 0;
	case ArgCursor:
		if (enumlookup(en_cursor, s, &v)) { conf_error(pos, "%s: expected move/resize, got '%s'", a->name, s); return -1; }
		arg->ui = (uint32_t)v;
		return 0;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* bindings, rules, monitors                                           */
/* ------------------------------------------------------------------ */
static int
parsemods(char *s, uint32_t *mods, const ConfPos *pos)
{
	char *tok, *save;

	*mods = 0;
	for (tok = strtok_r(s, "+ ", &save); tok; tok = strtok_r(NULL, "+ ", &save)) {
		if (!strcasecmp(tok, "SUPER") || !strcasecmp(tok, "LOGO") || !strcasecmp(tok, "MOD4") || !strcasecmp(tok, "WIN"))
			*mods |= WLR_MODIFIER_LOGO;
		else if (!strcasecmp(tok, "ALT") || !strcasecmp(tok, "MOD1"))
			*mods |= WLR_MODIFIER_ALT;
		else if (!strcasecmp(tok, "CTRL") || !strcasecmp(tok, "CONTROL"))
			*mods |= WLR_MODIFIER_CTRL;
		else if (!strcasecmp(tok, "SHIFT"))
			*mods |= WLR_MODIFIER_SHIFT;
		else if (strcasecmp(tok, "NONE")) {
			conf_error(pos, "unknown modifier '%s' (SUPER, ALT, CTRL, SHIFT, NONE)", tok);
			return -1;
		}
	}
	return 0;
}

static Key *cfgkeys;      static size_t ncfgkeys, capcfgkeys;
static Button *cfgbuttons; static size_t ncfgbuttons, capcfgbuttons;
static Rule *cfgrules;    static size_t ncfgrules, capcfgrules;
static MonitorRule *cfgmonrules; static size_t ncfgmonrules, capcfgmonrules;
static const char **cfgautostart; static size_t ncfgautostart, capcfgautostart;

static void
addkey(Key k)
{
	for (size_t i = 0; i < ncfgkeys; i++)
		if (CLEANMASK(cfgkeys[i].mod) == CLEANMASK(k.mod)
				&& xkb_keysym_to_lower(cfgkeys[i].keysym) == xkb_keysym_to_lower(k.keysym)) {
			cfgkeys[i] = k; /* later bind replaces an earlier one */
			return;
		}
	CFG_APPEND(cfgkeys, ncfgkeys, capcfgkeys, k);
}

/* bind = MODS, key, action[, argument] */
static int
parsebind(char *val, const ConfPos *pos)
{
	char *f[4]; size_t n = conf_split(val, ',', f, 4);
	const ActionDef *a; Key k; uint32_t mods; xkb_keysym_t sym;

	if (n < 3) { conf_error(pos, "bind: expected 'MODS, key, action[, argument]'"); return -1; }
	if (parsemods(f[0], &mods, pos))
		return -1;
	if ((sym = xkb_keysym_from_name(f[1], XKB_KEYSYM_CASE_INSENSITIVE)) == XKB_KEY_NoSymbol) {
		conf_error(pos, "bind: unknown key '%s' (use xkb keysym names, e.g. Return, space, h, XF86AudioMute)", f[1]);
		return -1;
	}
	if (!(a = findaction(f[2]))) { conf_error(pos, "bind: unknown action '%s'", f[2]); return -1; }
	memset(&k, 0, sizeof k);
	k.mod = mods; k.keysym = sym; k.func = a->func;
	if (parseargument(a, n > 3 ? f[3] : (char *)"", &k.arg, pos))
		return -1;
	addkey(k);
	return 0;
}

/* unbind = MODS, key   |   unbind = all */
static int
parseunbind(char *val, const ConfPos *pos)
{
	char *f[2]; size_t n = conf_split(val, ',', f, 2);
	uint32_t mods; xkb_keysym_t sym;

	if (n == 1 && !strcasecmp(f[0], "all")) { ncfgkeys = 0; return 0; }
	if (n != 2) { conf_error(pos, "unbind: expected 'MODS, key' or 'all'"); return -1; }
	if (parsemods(f[0], &mods, pos))
		return -1;
	if ((sym = xkb_keysym_from_name(f[1], XKB_KEYSYM_CASE_INSENSITIVE)) == XKB_KEY_NoSymbol) {
		conf_error(pos, "unbind: unknown key '%s'", f[1]);
		return -1;
	}
	for (size_t i = 0; i < ncfgkeys; i++)
		if (CLEANMASK(cfgkeys[i].mod) == CLEANMASK(mods)
				&& xkb_keysym_to_lower(cfgkeys[i].keysym) == xkb_keysym_to_lower(sym)) {
			cfgkeys[i] = cfgkeys[--ncfgkeys];
			return 0;
		}
	conf_error(pos, "unbind: no such binding");
	return -1;
}

/* bindtags = MODS: MODS+n view, +CTRL toggleview, +SHIFT tag, +CTRL+SHIFT toggletag, for n = 1..9 */
static int
parsebindtags(char *val, const ConfPos *pos)
{
	uint32_t mods;
	void (*funcs[4])(const Arg *) = { view, toggleview, tag, toggletag };
	uint32_t extra[4] = { 0, WLR_MODIFIER_CTRL, WLR_MODIFIER_SHIFT, WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT };
	char name[2] = { 0, 0 };

	if (parsemods(val, &mods, pos))
		return -1;
	for (size_t t = 0; t < LENGTH(tags) && t < 9; t++) {
		name[0] = '1' + t;
		for (int j = 0; j < 4; j++) {
			Key k = { .mod = mods | extra[j], .keysym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE),
			          .func = funcs[j] };
			k.arg.ui = 1u << t;
			addkey(k);
		}
	}
	return 0;
}

static const EnumName en_click_ctx[] = {
	{"tag", ClkTagBar}, {"ltsymbol", ClkLtSymbol}, {"status", ClkStatus}, {"title", ClkTitle},
	{"client", ClkClient}, {"root", ClkRoot}, {"tray", ClkTray}, {NULL, 0}};
static const EnumName en_button[] = {
	{"btn_left", BTN_LEFT}, {"btn_right", BTN_RIGHT}, {"btn_middle", BTN_MIDDLE},
	{"btn_side", BTN_SIDE}, {"btn_extra", BTN_EXTRA}, {NULL, 0}};

/* mousebind = context, MODS, button, action[, argument] */
static int
parsemousebind(char *val, const ConfPos *pos)
{
	char *f[5]; size_t n = conf_split(val, ',', f, 5);
	const ActionDef *a; Button b; int click, btn;

	if (n < 4) { conf_error(pos, "mousebind: expected 'context, MODS, button, action[, argument]'"); return -1; }
	if (enumlookup(en_click_ctx, f[0], &click)) { conf_error(pos, "mousebind: unknown context '%s' (tag, ltsymbol, status, title, client, root, tray)", f[0]); return -1; }
	memset(&b, 0, sizeof b);
	if (parsemods(f[1], &b.mod, pos))
		return -1;
	if (enumlookup(en_button, f[2], &btn)) { conf_error(pos, "mousebind: unknown button '%s' (btn_left, btn_right, btn_middle, btn_side, btn_extra)", f[2]); return -1; }
	if (!(a = findaction(f[3]))) { conf_error(pos, "mousebind: unknown action '%s'", f[3]); return -1; }
	b.click = click; b.button = btn; b.func = a->func;
	if (parseargument(a, n > 4 ? f[4] : (char *)"", &b.arg, pos))
		return -1;
	for (size_t i = 0; i < ncfgbuttons; i++)
		if (cfgbuttons[i].click == b.click && cfgbuttons[i].mod == b.mod && cfgbuttons[i].button == b.button) {
			cfgbuttons[i] = b;
			return 0;
		}
	CFG_APPEND(cfgbuttons, ncfgbuttons, capcfgbuttons, b);
	return 0;
}

/* rule = appid:foot, title:x, tags:3, floating:1, terminal:1, noswallow:1, monitor:0 */
static int
parserule(char *val, const ConfPos *pos)
{
	char *f[16]; size_t n = conf_split(val, ',', f, 16), i;
	Rule r = { NULL, NULL, 0, 0, 0, 0, -1 };
	int b; long l;

	for (i = 0; i < n; i++) {
		char *k = f[i], *v = strchr(k, ':');
		if (!v) { conf_error(pos, "rule: expected key:value, got '%s'", k); return -1; }
		*v++ = '\0'; k = conf_trim(k); v = conf_trim(v);
		if (!strcmp(k, "appid"))        r.id = cfgstrdup(v);
		else if (!strcmp(k, "title"))   r.title = cfgstrdup(v);
		else if (!strcmp(k, "tags")) {
			if (!strcasecmp(v, "all")) r.tags = ~0u;
			else if (conf_parse_long(v, 1, (long)LENGTH(tags), &l)) { conf_error(pos, "rule: tags must be 1..%zu, got '%s'", LENGTH(tags), v); return -1; }
			else r.tags = 1u << (l - 1);
		}
		else if (!strcmp(k, "floating") || !strcmp(k, "terminal") || !strcmp(k, "noswallow")) {
			if (conf_parse_bool(v, &b)) { conf_error(pos, "rule: %s expects true/false, got '%s'", k, v); return -1; }
			if (k[0] == 'f') r.isfloating = b; else if (k[0] == 't') r.isterm = b; else r.noswallow = b;
		}
		else if (!strcmp(k, "monitor")) {
			if (conf_parse_long(v, -1, 64, &l)) { conf_error(pos, "rule: monitor must be an index or -1, got '%s'", v); return -1; }
			r.monitor = (int)l;
		}
		else { conf_error(pos, "rule: unknown key '%s' (appid, title, tags, floating, terminal, noswallow, monitor)", k); return -1; }
	}
	if (!r.id && !r.title) { conf_error(pos, "rule: needs appid: or title:"); return -1; }
	CFG_APPEND(cfgrules, ncfgrules, capcfgrules, r);
	return 0;
}

static const EnumName en_transform[] = {
	{"normal", WL_OUTPUT_TRANSFORM_NORMAL}, {"90", WL_OUTPUT_TRANSFORM_90}, {"180", WL_OUTPUT_TRANSFORM_180},
	{"270", WL_OUTPUT_TRANSFORM_270}, {"flipped", WL_OUTPUT_TRANSFORM_FLIPPED},
	{"flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90}, {"flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180},
	{"flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270}, {NULL, 0}};

/* monitor = name, mfact, nmaster, scale, layout, transform, x, y   (trailing fields optional; name '*' matches all) */
static int
parsemonitor(char *val, const ConfPos *pos)
{
	char *f[8]; size_t n = conf_split(val, ',', f, 8);
	MonitorRule r = { NULL, 0.55f, 1, 1, &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL, -1, -1 };
	double d; long l; int v;

	if (n < 1 || !*f[0]) { conf_error(pos, "monitor: expected 'name[, mfact, nmaster, scale, layout, transform, x, y]'"); return -1; }
	if (strcmp(f[0], "*"))
		r.name = cfgstrdup(f[0]);
	if (n > 1 && *f[1]) { if (conf_parse_double(f[1], 0.05, 0.95, &d)) { conf_error(pos, "monitor: mfact must be 0.05..0.95, got '%s'", f[1]); return -1; } r.mfact = (float)d; }
	if (n > 2 && *f[2]) { if (conf_parse_long(f[2], 0, 100, &l)) { conf_error(pos, "monitor: nmaster must be 0..100, got '%s'", f[2]); return -1; } r.nmaster = (int)l; }
	if (n > 3 && *f[3]) { if (conf_parse_double(f[3], 0.1, 10, &d)) { conf_error(pos, "monitor: scale must be 0.1..10, got '%s'", f[3]); return -1; } r.scale = (float)d; }
	if (n > 4 && *f[4]) { if (enumlookup(en_layout, f[4], &v)) { conf_error(pos, "monitor: unknown layout '%s'", f[4]); return -1; } r.lt = &layouts[v]; }
	if (n > 5 && *f[5]) { if (enumlookup(en_transform, f[5], &v)) { conf_error(pos, "monitor: unknown transform '%s'", f[5]); return -1; } r.rr = v; }
	if (n > 6 && *f[6]) { if (conf_parse_long(f[6], -1, 100000, &l)) { conf_error(pos, "monitor: bad x '%s'", f[6]); return -1; } r.x = (int)l; }
	if (n > 7 && *f[7]) { if (conf_parse_long(f[7], -1, 100000, &l)) { conf_error(pos, "monitor: bad y '%s'", f[7]); return -1; } r.y = (int)l; }
	CFG_APPEND(cfgmonrules, ncfgmonrules, capcfgmonrules, r);
	return 0;
}

/* exec-once = command (run through sh once at startup, never on reload) */
static int
parseexec(char *val, const ConfPos *pos)
{
	const char *s;

	if (!*val) { conf_error(pos, "exec-once: missing command"); return -1; }
	s = "/bin/sh"; CFG_APPEND(cfgautostart, ncfgautostart, capcfgautostart, s);
	s = "-c";      CFG_APPEND(cfgautostart, ncfgautostart, capcfgautostart, s);
	s = cfgstrdup(val); CFG_APPEND(cfgautostart, ncfgautostart, capcfgautostart, s);
	s = NULL;      CFG_APPEND(cfgautostart, ncfgautostart, capcfgautostart, s);
	return 0;
}

/* tags = name1 name2 ...   (at most as many as there are tags) */
static int
parsetags(char *val, const ConfPos *pos)
{
	char *tok, *save; size_t i = 0;

	for (tok = strtok_r(val, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		if (i >= LENGTH(tags)) { conf_error(pos, "tags: only %zu tags exist, ignoring '%s'", LENGTH(tags), tok); return -1; }
		tags[i++] = cfgstrdup(tok);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */
static char *cfgpath;       /* from -c, or the default location */
static int cfgforcedebug;   /* -d beats the file */
static int cfgreloading;

static int
confhandler(const char *key, char *val, const ConfPos *pos, void *ud)
{
	(void)ud;
	if (!strcmp(key, "bind"))            return parsebind(val, pos);
	if (!strcmp(key, "unbind"))          return parseunbind(val, pos);
	if (!strcmp(key, "bindtags"))        return parsebindtags(val, pos);
	if (!strcmp(key, "mousebind"))       return parsemousebind(val, pos);
	if (!strcmp(key, "rule"))            return parserule(val, pos);
	if (!strcmp(key, "monitor"))         return parsemonitor(val, pos);
	if (!strcmp(key, "exec-once"))       return parseexec(val, pos);
	if (!strcmp(key, "tags"))            return parsetags(val, pos);
	for (size_t i = 0; i < LENGTH(options); i++)
		if (!strcmp(key, options[i].name))
			return setoption(&options[i], val, pos, cfgreloading);
	conf_error(pos, "unknown option '%s'", key);
	return -1;
}

static char *
defaultconfigpath(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char *p;
	size_t n;

	if (xdg && *xdg) {
		n = strlen(xdg) + sizeof("/nullwl/nullwl.conf");
		p = ecalloc(1, n);
		snprintf(p, n, "%s/nullwl/nullwl.conf", xdg);
	} else if (home && *home) {
		n = strlen(home) + sizeof("/.config/nullwl/nullwl.conf");
		p = ecalloc(1, n);
		snprintf(p, n, "%s/.config/nullwl/nullwl.conf", home);
	} else {
		return NULL;
	}
	return p;
}

/* Returns the number of rejected lines, or -1 if there is no config file. */
static int
loadconfig(int reload)
{
	size_t i;
	int errors;

	conf_errors_clear();
	if (!optsnapshot) {
		for (i = 0; i < LENGTH(options); i++)
			memcpy(optdefaults[i], options[i].ptr, optsize(options[i].type));
		for (i = 0; i < LENGTH(tags); i++)
			tagdefaults[i] = tags[i];
		optsnapshot = 1;
	} else {
		for (i = 0; i < LENGTH(options); i++)
			if (!(options[i].fx & FxRestart))
				memcpy(options[i].ptr, optdefaults[i], optsize(options[i].type));
		for (i = 0; i < LENGTH(tags); i++)
			tags[i] = tagdefaults[i];
	}

	/* start from the built-in lists */
	cfgfreeall();
	cfgkeys = NULL; ncfgkeys = capcfgkeys = 0;
	cfgbuttons = NULL; ncfgbuttons = capcfgbuttons = 0;
	cfgrules = NULL; ncfgrules = capcfgrules = 0;
	cfgmonrules = NULL; ncfgmonrules = capcfgmonrules = 0;
	cfgautostart = NULL; ncfgautostart = capcfgautostart = 0;
	for (i = 0; i < LENGTH(defkeys); i++)    CFG_APPEND(cfgkeys, ncfgkeys, capcfgkeys, defkeys[i]);
	for (i = 0; i < LENGTH(defbuttons); i++) CFG_APPEND(cfgbuttons, ncfgbuttons, capcfgbuttons, defbuttons[i]);
	for (i = 0; i < LENGTH(defrules); i++)   CFG_APPEND(cfgrules, ncfgrules, capcfgrules, defrules[i]);

	cfgreloading = reload;
	if (!cfgpath)
		cfgpath = defaultconfigpath();
	errors = cfgpath ? conf_load(cfgpath, confhandler, NULL) : -1;
	if (errors < 0)
		fprintf(stderr, "nullwl: config: no config file%s%s, using built-in defaults\n",
				cfgpath ? " at " : "", cfgpath ? cfgpath : "");
	else if (errors)
		fprintf(stderr, "nullwl: config: %s: %d line(s) ignored\n", cfgpath, errors);
	else
		fprintf(stderr, "nullwl: config: loaded %s (%zu binds, %zu mouse binds, %zu rules)\n",
				cfgpath, ncfgkeys, ncfgbuttons, ncfgrules);

	/* monitor rules from the file come first, the built-in catch-all last */
	for (i = 0; i < LENGTH(defmonrules); i++)
		CFG_APPEND(cfgmonrules, ncfgmonrules, capcfgmonrules, defmonrules[i]);
	CFG_APPEND(cfgautostart, ncfgautostart, capcfgautostart, (const char *)NULL);

	/* publish */
	keys = cfgkeys; nkeys = ncfgkeys;
	buttons = cfgbuttons; nbuttons = ncfgbuttons;
	rules = cfgrules; nrules = ncfgrules;
	monrules = cfgmonrules; nmonrules = ncfgmonrules;
	autostart = cfgautostart;
	free(drules);
	drules = ecalloc(nrules + RULES_MAX, sizeof(Rule));
	memcpy(drules, rules, nrules * sizeof(Rule));
	druleslen = nrules;

	if (cfgforcedebug)
		log_level = WLR_DEBUG;
	return errors;
}

/* Make the new settings take effect in the running session. */
static void
applyconfig(void)
{
	Monitor *m;
	Client *c, *sel;
	PointerDev *pd;

	wlr_log_init(log_level, NULL);
	wlr_scene_rect_set_color(root_bg, rootcolor);
	if (kb_group)
		wlr_keyboard_set_repeat_info(&kb_group->wlr_group->keyboard, repeat_rate, repeat_delay);
	wl_list_for_each(pd, &pointerdevs, link)
		configurepointer(pd->pointer);

	wl_list_for_each(m, &mons, link) {
		m->gappih = gappih; m->gappiv = gappiv;
		m->gappoh = gappoh; m->gappov = gappov;
	}
	sel = selmon ? focustop(selmon) : NULL;
	wl_list_for_each(c, &clients, link) {
		client_set_border_color(c, (float[])COLOR(colors[c->isurgent ? SchemeUrg : c == sel ? SchemeSel : SchemeNorm][ColBorder]));
		if (c->mon)
			resize(c, c->geom, 0); /* border width may have changed */
	}
	wl_list_for_each(m, &mons, link)
		arrange(m);
	drawbars();
}

static void
reloadconfig(const Arg *arg)
{
	int errors = loadconfig(1);

	applyconfig();
	ipc_config_reloaded(errors);
	fprintf(stderr, "nullwl: config reloaded%s\n", errors > 0 ? " with errors" : "");
}
