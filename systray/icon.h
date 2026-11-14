#ifndef ICON_H
#define ICON_H

#include <fcft/fcft.h>
#include <pixman.h>

#include <stddef.h>
#include <stdint.h>

typedef const struct fcft_glyph FallbackIcon;

typedef struct {
	pixman_image_t *img;
	uint32_t *buf_pixman;
	uint8_t *buf_orig;
	size_t size_orig;
	size_t size_pixman;
	int bbox_done;           /* bx.. below are valid */
	int bx, by, bw, bh;      /* box around the visible (non-transparent) pixels */
} Icon;

Icon *createicon (const uint8_t *buf, int width, int height, int size);
Icon *createicon_premul (uint32_t *pix, int width, int height);
FallbackIcon *createfallbackicon (const char *appname, int fgcolor,
                                  struct fcft_font *font);
void destroyicon (Icon *icon);
/* Composite icon into dst, scaled to fit a size x size box at (x, y) and centred in it.
 * The visible content is what is fitted, not the whole image, so glyphs with built-in
 * padding and full-bleed icons come out the same size. */
void icon_draw_fitted (Icon *icon, pixman_image_t *dst, int x, int y, int size);
int resize_image (pixman_image_t *orig, int new_width, int new_height);

#endif /* ICON_H */
