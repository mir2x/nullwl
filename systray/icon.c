#include "icon.h"

#include <fcft/fcft.h>
#include <pixman.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PREMUL_ALPHA(chan, alpha) (chan * alpha + 127) / 255

/*
 * Converts pixels from uint8_t[4] to uint32_t and
 * straight alpha to premultiplied alpha.
 */
static uint32_t *
to_pixman(const uint8_t *src, int n_pixels, size_t *pix_size)
{
	uint32_t *dest = NULL;

	*pix_size = n_pixels * sizeof(uint32_t);
	dest = malloc(*pix_size);
	if (!dest)
		return NULL;

	for (int i = 0; i < n_pixels; i++) {
		uint8_t a = src[i * 4 + 0];
		uint8_t r = src[i * 4 + 1];
		uint8_t g = src[i * 4 + 2];
		uint8_t b = src[i * 4 + 3];

		/*
		 * Skip premultiplying fully opaque and fully transparent
		 * pixels.
		 */
		if (a == 0) {
			dest[i] = 0;

		} else if (a == 255) {
			dest[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
			          ((uint32_t)g << 8) | ((uint32_t)b);

		} else {
			dest[i] = ((uint32_t)a << 24) |
			          ((uint32_t)PREMUL_ALPHA(r, a) << 16) |
			          ((uint32_t)PREMUL_ALPHA(g, a) << 8) |
			          ((uint32_t)PREMUL_ALPHA(b, a));
		}
	}

	return dest;
}

Icon *
createicon(const uint8_t *buf, int width, int height, int size)
{
	Icon *icon = NULL;

	int n_pixels;
	pixman_image_t *img = NULL;
	size_t pixbuf_size;
	uint32_t *buf_pixman = NULL;
	uint8_t *buf_orig = NULL;

	/* the data comes from another process: trust nothing about it */
	if (width < 1 || height < 1 || width > 2048 || height > 2048 ||
	    (long)width * height * 4 != size)
		return NULL;
	n_pixels = size / 4;

	icon = calloc(1, sizeof(Icon));
	buf_orig = malloc(size);
	buf_pixman = to_pixman(buf, n_pixels, &pixbuf_size);
	if (!icon || !buf_orig || !buf_pixman)
		goto fail;

	img = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height,
	                               buf_pixman, width * 4);
	if (!img)
		goto fail;

	memcpy(buf_orig, buf, size);

	icon->buf_orig = buf_orig;
	icon->buf_pixman = buf_pixman;
	icon->img = img;
	icon->size_orig = size;
	icon->size_pixman = pixbuf_size;

	return icon;

fail:
	free(buf_orig);
	if (img)
		pixman_image_unref(img);
	free(buf_pixman);
	free(icon);
	return NULL;
}

/* Takes ownership of pix (premultiplied ARGB32, w*h pixels, malloc'd). */
Icon *
createicon_premul(uint32_t *pix, int width, int height)
{
	Icon *icon = calloc(1, sizeof(Icon));
	pixman_image_t *img;

	if (!icon)
		return NULL;
	img = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, pix, width * 4);
	if (!img) {
		free(icon);
		return NULL;
	}
	icon->img = img;
	icon->buf_pixman = pix;
	icon->size_pixman = (size_t)width * height * 4;
	return icon;
}

void
destroyicon(Icon *icon)
{
	if (icon->img)
		pixman_image_unref(icon->img);
	free(icon->buf_orig);
	free(icon->buf_pixman);
	free(icon);
}

FallbackIcon *
createfallbackicon(const char *appname, int fgcolor, struct fcft_font *font)
{
	const struct fcft_glyph *glyph;
	char initial;

	if ((unsigned char)appname[0] > 127) {
		/* first character is not ascii */
		initial = '?';
	} else {
		initial = toupper(*appname);
	}

	glyph = fcft_rasterize_char_utf32(font, initial, FCFT_SUBPIXEL_DEFAULT);
	if (!glyph)
		return NULL;

	return glyph;
}

/* box around pixels whose alpha is noticeable; the whole image when there are none */
static void
icon_measure(Icon *icon)
{
	int w = pixman_image_get_width(icon->img), h = pixman_image_get_height(icon->img);
	int stride = pixman_image_get_stride(icon->img) / 4;
	const uint32_t *p = pixman_image_get_data(icon->img);
	int x, y, x0 = w, y0 = h, x1 = -1, y1 = -1;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			if ((p[(size_t)y * stride + x] >> 24) > 24) {
				if (x < x0) x0 = x;
				if (x > x1) x1 = x;
				if (y < y0) y0 = y;
				if (y > y1) y1 = y;
			}
	if (x1 < 0) {
		x0 = y0 = 0;
		x1 = w - 1;
		y1 = h - 1;
	}
	icon->bx = x0;
	icon->by = y0;
	icon->bw = x1 - x0 + 1;
	icon->bh = y1 - y0 + 1;
	icon->bbox_done = 1;
}

void
icon_draw_fitted(Icon *icon, pixman_image_t *dst, int x, int y, int size)
{
	int w = pixman_image_get_width(icon->img), h = pixman_image_get_height(icon->img);
	double s, s_whole, cap;
	int dw, dh, dx, dy;
	pixman_transform_t tr;

	if (!icon->bbox_done)
		icon_measure(icon);
	s = fmin((double)size / icon->bw, (double)size / icon->bh);
	/* a tiny speck must not be blown up to fill the slot */
	s_whole = (double)size / (w > h ? w : h);
	cap = s_whole * 2.0;
	if (s > cap)
		s = cap;
	dw = (int)lround(icon->bw * s);
	dh = (int)lround(icon->bh * s);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	dx = x + (size - dw) / 2;
	dy = y + (size - dh) / 2;

	/* pixman samples the source at T * (dst pixel - dest origin + src origin) */
	pixman_transform_init_scale(&tr, pixman_double_to_fixed(1.0 / s), pixman_double_to_fixed(1.0 / s));
	pixman_image_set_transform(icon->img, &tr);
	pixman_image_set_filter(icon->img, PIXMAN_FILTER_BEST, NULL, 0);
	pixman_image_composite32(PIXMAN_OP_OVER, icon->img, NULL, dst,
	                         (int)lround(icon->bx * s), (int)lround(icon->by * s), 0, 0,
	                         dx, dy, dw, dh);
}

int
resize_image(pixman_image_t *image, int new_width, int new_height)
{
	int src_width = pixman_image_get_width(image);
	int src_height = pixman_image_get_height(image);
	pixman_transform_t transform;
	pixman_fixed_t scale_x, scale_y;

	if (src_width == new_width && src_height == new_height)
		return 0;

	scale_x = pixman_double_to_fixed((double)src_width / new_width);
	scale_y = pixman_double_to_fixed((double)src_height / new_height);

	pixman_transform_init_scale(&transform, scale_x, scale_y);
	if (!pixman_image_set_filter(image, PIXMAN_FILTER_BEST, NULL, 0) ||
	    !pixman_image_set_transform(image, &transform)) {
		return -1;
	}

	return 0;
}
