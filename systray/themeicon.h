#ifndef THEMEICON_H
#define THEMEICON_H

#include <stdint.h>

/* Premultiplied ARGB32 pixels in native byte order (what pixman a8r8g8b8 and
 * cairo ARGB32 both use). Free with themeicon_free(). */
typedef struct {
	uint32_t *pix;
	int w, h;
} ThemeBitmap;

/* The icon theme to search first. NULL (the default) means: the GTK setting
 * gtk-icon-theme-name, else hicolor. Inherited themes and hicolor are always
 * searched after it. */
void themeicon_set_theme(const char *name);

/*
 * Find and load an icon.
 *   name      a theme icon name ("network-wireless"), or an absolute path
 *   extradir  searched before the system theme directories (the item's
 *             IconThemePath); may be NULL
 *   size      the size the icon will be shown at, in pixels
 * Returns NULL if nothing was found or the file could not be decoded.
 */
/* Just the lookup: the path of the file themeicon_load() would decode (malloc'd), or NULL. */
char *themeicon_find(const char *name, const char *extradir, int size);
ThemeBitmap *themeicon_load(const char *name, const char *extradir, int size);
void themeicon_free(ThemeBitmap *bm);

#endif /* THEMEICON_H */
