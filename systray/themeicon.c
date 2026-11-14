/*
 * Icon lookup following the freedesktop Icon Theme Specification, and
 * decoding of the files it finds (PNG through cairo, SVG through librsvg).
 */
#include "themeicon.h"

#include <cairo.h>
#include <librsvg/rsvg.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define SVG_RENDER_SIZE_MIN 16
#define MAX_DEPTH 8       /* theme inheritance */
#define MAX_ICON_PIXELS 2048

static char *configured_theme;

void
themeicon_set_theme(const char *name)
{
	free(configured_theme);
	configured_theme = name && *name ? strdup(name) : NULL;
}

/* ------------------------------------------------------------------ */
/* small string helpers                                                */
/* ------------------------------------------------------------------ */
static char *
xstrdup(const char *s)
{
	char *r = strdup(s);
	if (!r)
		abort();
	return r;
}

static char *
join(const char *a, const char *b, const char *c)
{
	size_t n = strlen(a) + strlen(b) + (c ? strlen(c) : 0) + 1;
	char *r = malloc(n);
	if (!r)
		abort();
	snprintf(r, n, "%s%s%s", a, b, c ? c : "");
	return r;
}

static int
isfile(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *
trim(char *s)
{
	char *e;
	while (isspace((unsigned char)*s))
		s++;
	for (e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); )
		*--e = '\0';
	return s;
}

/* ------------------------------------------------------------------ */
/* base directories                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
	char **v;
	size_t n;
} List;

static void
list_add(List *l, char *s) /* takes ownership */
{
	char **nv = realloc(l->v, (l->n + 1) * sizeof *nv);
	if (!nv)
		abort();
	l->v = nv;
	l->v[l->n++] = s;
}

static int
list_has(const List *l, const char *s)
{
	for (size_t i = 0; i < l->n; i++)
		if (!strcmp(l->v[i], s))
			return 1;
	return 0;
}

static void
list_free(List *l)
{
	for (size_t i = 0; i < l->n; i++)
		free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->n = 0;
}

/* ~/.icons, $XDG_DATA_HOME/icons, then $XDG_DATA_DIRS/icons */
static void
system_basedirs(List *out)
{
	const char *home = getenv("HOME"), *xh = getenv("XDG_DATA_HOME"), *xd = getenv("XDG_DATA_DIRS");
	char *dirs, *tok, *save;

	if (home && *home)
		list_add(out, join(home, "/.icons", NULL));
	if (xh && *xh)
		list_add(out, join(xh, "/icons", NULL));
	else if (home && *home)
		list_add(out, join(home, "/.local/share/icons", NULL));
	dirs = xstrdup(xd && *xd ? xd : "/usr/local/share:/usr/share");
	for (tok = strtok_r(dirs, ":", &save); tok; tok = strtok_r(NULL, ":", &save))
		list_add(out, join(tok, "/icons", NULL));
	free(dirs);
}

/* ------------------------------------------------------------------ */
/* index.theme                                                         */
/* ------------------------------------------------------------------ */
typedef enum { DirFixed, DirScalable, DirThreshold } DirType;

typedef struct {
	char *path; /* relative to the theme directory */
	int size, scale, min, max, threshold;
	DirType type;
} ThemeDir;

typedef struct {
	char *base;  /* e.g. /usr/share/icons */
	char *name;  /* theme directory name */
	ThemeDir *dirs;
	size_t ndirs;
	char *inherits; /* comma-separated, raw */
} ThemeIndex;

static ThemeIndex *index_cache;
static size_t nindex_cache;

static void
dirs_add(ThemeIndex *ti, const char *path)
{
	ThemeDir *nd = realloc(ti->dirs, (ti->ndirs + 1) * sizeof *nd);
	if (!nd)
		abort();
	ti->dirs = nd;
	memset(&nd[ti->ndirs], 0, sizeof *nd);
	nd[ti->ndirs].path = xstrdup(path);
	nd[ti->ndirs].scale = 1;
	nd[ti->ndirs].type = DirThreshold;
	nd[ti->ndirs].threshold = 2;
	ti->ndirs++;
}

static ThemeDir *
dirs_find(ThemeIndex *ti, const char *path)
{
	for (size_t i = 0; i < ti->ndirs; i++)
		if (!strcmp(ti->dirs[i].path, path))
			return &ti->dirs[i];
	return NULL;
}

static ThemeIndex *
parse_index(const char *base, const char *name)
{
	ThemeIndex *ti;
	char *path = join(base, "/", name), *file = join(path, "/index.theme", NULL);
	char *line = NULL, *dirlist = NULL, section[256] = "";
	size_t cap = 0;
	FILE *f = fopen(file, "r");

	free(path);
	free(file);
	index_cache = realloc(index_cache, (nindex_cache + 1) * sizeof *index_cache);
	if (!index_cache)
		abort();
	ti = &index_cache[nindex_cache++];
	memset(ti, 0, sizeof *ti);
	ti->base = xstrdup(base);
	ti->name = xstrdup(name);
	if (!f)
		return ti; /* cached as "no such theme here" */

	/* first pass collects sections; Directories= may come before the sections */
	while (getline(&line, &cap, f) >= 0) {
		char *l = trim(line), *eq;

		if (*l == '[') {
			char *e = strchr(l, ']');
			if (e) {
				*e = '\0';
				snprintf(section, sizeof section, "%s", l + 1);
			}
			continue;
		}
		if (!*l || *l == '#' || !(eq = strchr(l, '=')))
			continue;
		*eq = '\0';
		{
			char *k = trim(l), *v = trim(eq + 1);
			if (!strcmp(section, "Icon Theme")) {
				if (!strcmp(k, "Directories") || !strcmp(k, "ScaledDirectories")) {
					char *joined = dirlist ? join(dirlist, ",", v) : xstrdup(v);
					free(dirlist);
					dirlist = joined;
				} else if (!strcmp(k, "Inherits")) {
					free(ti->inherits);
					ti->inherits = xstrdup(v);
				}
			}
		}
	}
	if (dirlist) {
		char *tok, *save;
		for (tok = strtok_r(dirlist, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
			tok = trim(tok);
			if (*tok && !dirs_find(ti, tok))
				dirs_add(ti, tok);
		}
	}
	/* second pass: per-directory keys */
	rewind(f);
	section[0] = '\0';
	while (getline(&line, &cap, f) >= 0) {
		char *l = trim(line), *eq;
		ThemeDir *d;

		if (*l == '[') {
			char *e = strchr(l, ']');
			if (e) {
				*e = '\0';
				snprintf(section, sizeof section, "%s", l + 1);
			}
			continue;
		}
		if (!*l || *l == '#' || !(eq = strchr(l, '=')) || !(d = dirs_find(ti, section)))
			continue;
		*eq = '\0';
		{
			char *k = trim(l), *v = trim(eq + 1);
			int n = atoi(v);
			if (!strcmp(k, "Size")) { d->size = n; if (!d->min) d->min = n; if (!d->max) d->max = n; }
			else if (!strcmp(k, "Scale")) d->scale = n > 0 ? n : 1;
			else if (!strcmp(k, "MinSize")) d->min = n;
			else if (!strcmp(k, "MaxSize")) d->max = n;
			else if (!strcmp(k, "Threshold")) d->threshold = n;
			else if (!strcmp(k, "Type"))
				d->type = !strcasecmp(v, "Fixed") ? DirFixed : !strcasecmp(v, "Scalable") ? DirScalable : DirThreshold;
		}
	}
	free(line);
	free(dirlist);
	fclose(f);
	return ti;
}

static ThemeIndex *
get_index(const char *base, const char *name)
{
	for (size_t i = 0; i < nindex_cache; i++)
		if (!strcmp(index_cache[i].base, base) && !strcmp(index_cache[i].name, name))
			return &index_cache[i];
	return parse_index(base, name);
}

/* Directory size distance from the spec (scale is always 1 for us). */
static int
dir_distance(const ThemeDir *d, int size)
{
	switch (d->type) {
	case DirFixed:
		return abs(d->size - size);
	case DirScalable:
		if (size < d->min) return d->min - size;
		if (size > d->max) return size - d->max;
		return 0;
	case DirThreshold:
	default:
		if (size < d->size - d->threshold) return (d->size - d->threshold) - size;
		if (size > d->size + d->threshold) return size - (d->size + d->threshold);
		return 0;
	}
}

/* Best file for name inside one theme (all base dirs). Returns malloc'd path or NULL. */
static char *
find_in_theme(const List *bases, const char *theme, const char *name, int size)
{
	char *best = NULL;
	int bestdist = INT_MAX, bestsvg = 0;
	static const char *exts[] = { ".svg", ".png" };

	for (size_t b = 0; b < bases->n; b++) {
		ThemeIndex *ti = get_index(bases->v[b], theme);

		for (size_t i = 0; i < ti->ndirs; i++) {
			const ThemeDir *d = &ti->dirs[i];
			int dist = dir_distance(d, size);

			if (d->scale != 1 || dist > bestdist)
				continue;
			for (size_t e = 0; e < 2; e++) {
				char *p = join(bases->v[b], "/", NULL), *q, *r;
				q = join(p, theme, "/");
				free(p);
				p = join(q, d->path, "/");
				free(q);
				r = join(p, name, exts[e]);
				free(p);
				if (isfile(r) && (dist < bestdist || (dist == bestdist && e == 0 && !bestsvg))) {
					free(best);
					best = r;
					bestdist = dist;
					bestsvg = e == 0;
				} else {
					free(r);
				}
			}
		}
	}
	return best;
}

static void
collect_chain(const List *bases, const char *theme, List *chain, int depth)
{
	if (depth > MAX_DEPTH || list_has(chain, theme))
		return;
	list_add(chain, xstrdup(theme));
	for (size_t b = 0; b < bases->n; b++) {
		ThemeIndex *ti = get_index(bases->v[b], theme);
		if (ti->inherits) {
			char *copy = xstrdup(ti->inherits), *tok, *save;
			for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
				collect_chain(bases, trim(tok), chain, depth + 1);
			free(copy);
			return; /* the first base dir that knows the theme decides */
		}
	}
}

static char *
current_theme(void)
{
	const char *xc = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char *path, *line = NULL, *theme = NULL;
	size_t cap = 0;
	FILE *f;

	if (configured_theme)
		return xstrdup(configured_theme);
	if (xc && *xc)
		path = join(xc, "/gtk-3.0/settings.ini", NULL);
	else if (home && *home)
		path = join(home, "/.config/gtk-3.0/settings.ini", NULL);
	else
		return xstrdup("hicolor");
	if ((f = fopen(path, "r"))) {
		while (getline(&line, &cap, f) >= 0) {
			char *l = trim(line);
			if (!strncmp(l, "gtk-icon-theme-name", 19) && strchr(l, '=')) {
				theme = xstrdup(trim(strchr(l, '=') + 1));
				break;
			}
		}
		free(line);
		fclose(f);
	}
	free(path);
	return theme && *theme ? theme : (free(theme), xstrdup("hicolor"));
}

/* flat files: the item's own dir and the legacy pixmaps dir */
static char *
find_flat(const char *dir, const char *name)
{
	static const char *exts[] = { ".svg", ".png" };

	for (size_t e = 0; e < 2; e++) {
		char *p = join(dir, "/", name), *r = join(p, exts[e], NULL);
		free(p);
		if (isfile(r))
			return r;
		free(r);
	}
	return NULL;
}

static char *
find_icon(const char *name, const char *extradir, int size)
{
	List bases = {0}, chain = {0};
	char *theme, *found = NULL;

	if (extradir && *extradir) {
		/* an item may ship a complete theme tree, or just loose files */
		if ((found = find_flat(extradir, name)))
			return found;
		list_add(&bases, xstrdup(extradir));
	}
	system_basedirs(&bases);

	theme = current_theme();
	collect_chain(&bases, theme, &chain, 0);
	free(theme);
	collect_chain(&bases, "hicolor", &chain, 0);
	for (size_t i = 0; i < chain.n && !found; i++)
		found = find_in_theme(&bases, chain.v[i], name, size);
	if (!found)
		found = find_flat("/usr/share/pixmaps", name);
	list_free(&bases);
	list_free(&chain);
	return found;
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */
/* ------------------------------------------------------------------ */
static ThemeBitmap *
bitmap_from_surface(cairo_surface_t *s)
{
	ThemeBitmap *bm;
	int w, h, stride;
	unsigned char *data;

	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS)
		return NULL;
	cairo_surface_flush(s);
	w = cairo_image_surface_get_width(s);
	h = cairo_image_surface_get_height(s);
	stride = cairo_image_surface_get_stride(s);
	data = cairo_image_surface_get_data(s);
	if (w <= 0 || h <= 0 || w > MAX_ICON_PIXELS || h > MAX_ICON_PIXELS || !data
			|| cairo_image_surface_get_format(s) != CAIRO_FORMAT_ARGB32)
		return NULL;
	if (!(bm = calloc(1, sizeof *bm)) || !(bm->pix = malloc((size_t)w * h * 4))) {
		free(bm);
		return NULL;
	}
	for (int y = 0; y < h; y++)
		memcpy(bm->pix + (size_t)y * w, data + (size_t)y * stride, (size_t)w * 4);
	bm->w = w;
	bm->h = h;
	return bm;
}

static ThemeBitmap *
load_svg(const char *path, int size)
{
	GError *err = NULL;
	RsvgHandle *h = rsvg_handle_new_from_file(path, &err);
	cairo_surface_t *s;
	cairo_t *cr;
	RsvgRectangle viewport;
	ThemeBitmap *bm = NULL;

	if (!h) {
		if (err)
			g_error_free(err);
		return NULL;
	}
	if (size < SVG_RENDER_SIZE_MIN)
		size = SVG_RENDER_SIZE_MIN;
	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cr = cairo_create(s);
	viewport = (RsvgRectangle){ 0, 0, size, size };
	if (rsvg_handle_render_document(h, cr, &viewport, &err))
		bm = bitmap_from_surface(s);
	else if (err)
		g_error_free(err);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	g_object_unref(h);
	return bm;
}

static ThemeBitmap *
load_png(const char *path)
{
	cairo_surface_t *s = cairo_image_surface_create_from_png(path);
	ThemeBitmap *bm = bitmap_from_surface(s);

	cairo_surface_destroy(s);
	return bm;
}

char *
themeicon_find(const char *name, const char *extradir, int size)
{
	if (!name || !*name)
		return NULL;
	if (name[0] == '/')
		return isfile(name) ? xstrdup(name) : NULL;
	return find_icon(name, extradir, size);
}

ThemeBitmap *
themeicon_load(const char *name, const char *extradir, int size)
{
	char *path;
	ThemeBitmap *bm = NULL;
	size_t n;

	/* SVGs are rasterized at the size asked for, but never tiny: they are scaled down later */
	if (size < 32)
		size = 32;
	path = themeicon_find(name, extradir, size);
	if (!path)
		return NULL;
	n = strlen(path);
	if (n > 4 && !strcasecmp(path + n - 4, ".png"))
		bm = load_png(path);
	else if ((n > 4 && !strcasecmp(path + n - 4, ".svg")) || (n > 5 && !strcasecmp(path + n - 5, ".svgz")))
		bm = load_svg(path, size);
	free(path);
	return bm;
}

void
themeicon_free(ThemeBitmap *bm)
{
	if (bm) {
		free(bm->pix);
		free(bm);
	}
}
