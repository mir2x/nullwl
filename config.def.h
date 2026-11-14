/*
 * Built-in defaults. These are what nullwl uses when there is no config file,
 * so keep them small and safe. Your real setup lives in
 * ~/.config/nullwl/nullwl.conf (see nullwl.conf.example), which overrides
 * everything below and is re-read on reload (Mod+Shift+C, SIGHUP).
 *
 * Only things that need C code (layout functions, the action table) are fixed
 * at compile time.
 */

/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }

/* appearance */
static int sloppyfocus                  = 1;  /* focus follows mouse */
static unsigned int cursor_size         = 32; /* xcursor size in pixels */
static int bypass_surface_visibility    = 0;  /* 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible  */
static int smartgaps                    = 0;  /* 1 means no outer gap when there is only one window */
static int monoclegaps                  = 0;  /* 1 means outer gaps in monocle layout */
static unsigned int borderpx            = 1;  /* border pixel of windows */
static unsigned int systrayspacing      = 2;  /* systray spacing */
static int showsystray                  = 1;  /* 0 means no systray */
static char *icon_theme                 = NULL; /* tray icon theme; NULL = the GTK setting */
static char *status_cmd                 = NULL; /* program whose output lines are shown as the bar status; NULL = read stdin */
static unsigned int gappih              = 10; /* horiz inner gap between windows */
static unsigned int gappiv              = 10; /* vert inner gap between windows */
static unsigned int gappoh              = 10; /* horiz outer gap between windows and screen edge */
static unsigned int gappov              = 10; /* vert outer gap between windows and screen edge */
static int showbar                      = 1;  /* 0 means no bar */
static int topbar                       = 1;  /* 0 means bottom bar */
static const char *fonts[]              = {"monospace:size=12"};
static float rootcolor[]                = COLOR(0x242424ff);
/* This conforms to the xdg-protocol. Set the alpha to zero to restore the old behavior */
static float fullscreen_bg[]            = {0.0f, 0.0f, 0.0f, 1.0f}; /* You can also use glsl colors */
static uint32_t colors[][3]             = {
	/*               fg          bg          border    */
	[SchemeNorm] = { 0xdededeff, 0x242424ff, 0x4d4d4dff },
	[SchemeSel]  = { 0xffffffff, 0x2e7cf7ff, 0x2e7cf7ff },
	[SchemeHid]  = { 0x808080ff, 0x242424ff, 0x2e7cf7ff },
	[SchemeUrg]  = { 0xffffffff, 0xff453aff, 0xff453aff },
};
static int enableautoswallow = 1;    /* enables autoswallowing newly spawned clients */
static float swallowborder = 1.0f;   /* add this multiplied by borderpx to border when a client is swallowed */

#define SCRATCHPAD_COUNT 3
/* tagging: the number of tags is fixed at compile time, their names are not */
static char *tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

/* logging */
static int log_level = WLR_ERROR;

/* Max amount of dynamically added rules */
#define RULES_MAX 100

/* layout(s). The order is fixed: the config file refers to them by name
 * (tile, floating, monocle). */
static const Layout layouts[] = {
	/* symbol     arrange function */
	{ "[T]",      tile },
	{ "[FL]",     NULL },    /* no layout function means floating behavior */
	{ "[M]",      monocle },
};

/* default window rules: none. The empty rule is only a placeholder. */
static const Rule defrules[] = {
	/* app_id   title   tags mask   isfloating   isterm   noswallow   monitor */
	{ NULL,     NULL,   0,          0,           0,       0,          -1 },
};

/* monitors */
/* (x=-1, y=-1) is reserved as an "autoconfigure" monitor position indicator
 * WARNING: negative values other than (-1, -1) cause problems with Xwayland clients due to
 * https://gitlab.freedesktop.org/xorg/xserver/-/issues/899 */
static const MonitorRule defmonrules[] = {
	/* name       mfact  nmaster scale layout       rotate/reflect                x    y */
	{ NULL,       0.55f, 1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
	/* default monitor rule: can be changed but cannot be eliminated; at least one monitor rule must exist */
};

/* keyboard */
static const struct xkb_rule_names xkb_rules = {
	/* can specify fields: rules, model, layout, variant, options */
	.options = NULL,
};

static int repeat_rate = 25;
static int repeat_delay = 600;

/* Trackpad */
static int tap_to_click = 1;
static int tap_and_drag = 1;
static int drag_lock = 1;
static int natural_scrolling = 0;
static int disable_while_typing = 1;
static int left_handed = 0;
static int middle_button_emulation = 0;
static enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;
static enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
static uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
static enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
static double accel_speed = 0.0;
static enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

/* Built-in key bindings: just enough to start a terminal, reload and quit.
 * Everything else belongs in the config file. */
#define MODKEY WLR_MODIFIER_LOGO

#define TAGKEYS(KEY,SKEY,TAG) \
	{ MODKEY,                    KEY,            view,            {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL,  KEY,            toggleview,      {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_SHIFT, SKEY,           tag,             {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,SKEY,toggletag, {.ui = 1 << TAG} }

static const char *termcmd[] = { "foot", NULL };

static const Key defkeys[] = {
	/* modifier                  key                  function          argument */
	{ MODKEY,                    XKB_KEY_Return,      spawn,            {.v = termcmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_C,           reloadconfig,     {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Q,           quit,             {0} },
	{ MODKEY,                    XKB_KEY_q,           killclient,       {0} },
	{ MODKEY,                    XKB_KEY_Tab,         view,             {0} },
	TAGKEYS(          XKB_KEY_1, XKB_KEY_exclam,                        0),
	TAGKEYS(          XKB_KEY_2, XKB_KEY_at,                            1),
	TAGKEYS(          XKB_KEY_3, XKB_KEY_numbersign,                    2),
	TAGKEYS(          XKB_KEY_4, XKB_KEY_dollar,                        3),
	TAGKEYS(          XKB_KEY_5, XKB_KEY_percent,                       4),
	TAGKEYS(          XKB_KEY_6, XKB_KEY_asciicircum,                   5),
	TAGKEYS(          XKB_KEY_7, XKB_KEY_ampersand,                     6),
	TAGKEYS(          XKB_KEY_8, XKB_KEY_asterisk,                      7),
	TAGKEYS(          XKB_KEY_9, XKB_KEY_parenleft,                     8),

	/* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
	{ WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_Terminate_Server, quit, {0} },
	/* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
	 * do not remove them.
	 */
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_XF86Switch_VT_##n, chvt, {.ui = (n)} }
	CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
	CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

static const Button defbuttons[] = {
	{ ClkLtSymbol, 0,      BTN_LEFT,   setlayout,      {.v = &layouts[0]} },
	{ ClkLtSymbol, 0,      BTN_RIGHT,  setlayout,      {.v = &layouts[2]} },
	{ ClkTitle,    0,      BTN_LEFT,   togglewin,      {0} },
	{ ClkStatus,   0,      BTN_MIDDLE, spawn,          {.v = termcmd} },
	{ ClkClient,   MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ ClkClient,   MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ ClkClient,   MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
	{ ClkTagBar,   0,      BTN_LEFT,   view,           {0} },
	{ ClkTagBar,   0,      BTN_RIGHT,  toggleview,     {0} },
	{ ClkTagBar,   MODKEY, BTN_LEFT,   tag,            {0} },
	{ ClkTagBar,   MODKEY, BTN_RIGHT,  toggletag,      {0} },
	{ ClkTray,     0,      BTN_LEFT,   trayactivate,   {0} },
	{ ClkTray,     0,      BTN_MIDDLE, traysecondary,  {0} },
	{ ClkTray,     0,      BTN_RIGHT,  traymenu,       {0} },
};
