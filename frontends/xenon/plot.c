/*
 * Xenon (Xbox 360) plotter: draws NetSurf plot operations into a linear
 * 1280x720 XRGB buffer and presents it with the xsdk display API.
 *
 * Copyright 2026
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "utils/utf8.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"

#include "xenon/bitmap.h"
#include "xenon/plot.h"
#include "xenon/ui.h"
#include "xenon/xenon.h"

#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/present.h"
#include "xsdk/log.h"

extern const unsigned char xenon_font8x16[4096];

uint32_t xenon_fb[XENON_WIDTH * XENON_HEIGHT];

static struct rect clip = { 0, 0, XENON_WIDTH, XENON_PAGE_HEIGHT };

static unsigned int stat_rects;
static unsigned int stat_lines;
static unsigned int stat_polygons;
static unsigned int stat_texts;
static unsigned int stat_bitmaps;

#define GLYPH_W 8
#define GLYPH_H 16
#define BASELINE 12
#define CURSOR_SCALE 2

/* libxenon:drivers/console/console.c:60-61, BSD 3-clause, Copyright 2009 Georg Lukas. */
static uint32_t tiled_index(uint32_t x, uint32_t y, uint32_t width)
{
	return ((y >> 5) * 32 * width + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) +
		(((x & 31) >> 2) << 3) + (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2);
}

/* A pixel's bytes in memory are B, G, R, A (xsdk tests/probe_display.c:29-33). */
static uint32_t pixel_bgra(uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 0xff;
	uint32_t g = (rgb >> 8) & 0xff;
	uint32_t b = rgb & 0xff;
	return (b << 24) | (g << 16) | (r << 8) | 0xff;
}

/* NetSurf colours are 0xAABBGGRR (plot_style.h component masks); our
 * framebuffer stores 0x00RRGGBB. */
static inline uint32_t to_rgb(uint32_t colour)
{
	return ((colour & 0xff) << 16) | (colour & 0xff00) |
	       ((colour >> 16) & 0xff);
}

static inline void plot_pixel(int x, int y, uint32_t colour)
{
	if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1)
		return;
	xenon_fb[(y + XENON_CHROME_TOP) * XENON_WIDTH + x] = colour;
}

static void fill_span(int y, int x0, int x1, uint32_t colour)
{
	if (x0 < clip.x0)
		x0 = clip.x0;
	if (x1 > clip.x1)
		x1 = clip.x1;
	if (x0 >= x1 || y < clip.y0 || y >= clip.y1)
		return;

	uint32_t *row = &xenon_fb[(y + XENON_CHROME_TOP) * XENON_WIDTH];
	for (int x = x0; x < x1; x++)
		row[x] = colour;
}

static void fill_rect(int x0, int y0, int x1, int y1, uint32_t colour)
{
	if (y0 < clip.y0)
		y0 = clip.y0;
	if (y1 > clip.y1)
		y1 = clip.y1;
	for (int y = y0; y < y1; y++)
		fill_span(y, x0, x1, colour);
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t colour, int width)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int dy = y1 > y0 ? y1 - y0 : y0 - y1;

	if (dx == 0 && dy == 0) {
		plot_pixel(x0, y0, colour);
		return;
	}

	if (dx >= dy) {
		int step = x1 >= x0 ? 1 : -1;
		int y = y0;
		int err = dx / 2;
		for (int x = x0; x != x1 + step; x += step) {
			for (int w = 0; w < width; w++)
				plot_pixel(x, y + w, colour);
			err -= dy;
			if (err < 0) {
				err += dx;
				y += y1 >= y0 ? 1 : -1;
			}
		}
	} else {
		int step = y1 >= y0 ? 1 : -1;
		int x = x0;
		int err = dy / 2;
		for (int y = y0; y != y1 + step; y += step) {
			for (int w = 0; w < width; w++)
				plot_pixel(x + w, y, colour);
			err -= dx;
			if (err < 0) {
				err += dy;
				x += x1 >= x0 ? 1 : -1;
			}
		}
	}
}

static void fill_polygon(const int *p, unsigned int n, uint32_t colour)
{
	if (n < 3)
		return;

	unsigned int maxn = n;
	if (maxn > 256)
		maxn = 256;

	int ymin = p[1];
	int ymax = p[1];
	for (unsigned int i = 1; i < maxn; i++) {
		if (p[2 * i + 1] < ymin)
			ymin = p[2 * i + 1];
		if (p[2 * i + 1] > ymax)
			ymax = p[2 * i + 1];
	}
	if (ymin < clip.y0)
		ymin = clip.y0;
	if (ymax > clip.y1 - 1)
		ymax = clip.y1 - 1;

	for (int y = ymin; y <= ymax; y++) {
		float xs[256];
		unsigned int nx = 0;

		for (unsigned int i = 0; i < maxn && nx < 256; i++) {
			unsigned int j = (i + 1) % maxn;
			int y0 = p[2 * i + 1];
			int y1 = p[2 * j + 1];
			if (y0 == y1)
				continue;
			if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) {
				int x0 = p[2 * i];
				int x1 = p[2 * j];
				xs[nx++] = (float)x0 + (float)(y - y0) * (float)(x1 - x0) / (float)(y1 - y0);
			}
		}

		for (unsigned int i = 1; i < nx; i++) {
			float v = xs[i];
			unsigned int j = i;
			while (j > 0 && xs[j - 1] > v) {
				xs[j] = xs[j - 1];
				j--;
			}
			xs[j] = v;
		}

		for (unsigned int i = 0; i + 1 < nx; i += 2) {
			int a = (int)xs[i];
			int b = (int)xs[i + 1];
			fill_span(y, a, b + 1, colour);
		}
	}
}

static void draw_circle(int cx, int cy, int radius, uint32_t colour, bool filled)
{
	int x = radius;
	int y = 0;
	int err = 0;

	while (x >= y) {
		if (filled) {
			fill_span(cy + y, cx - x, cx + x + 1, colour);
			fill_span(cy - y, cx - x, cx + x + 1, colour);
			fill_span(cy + x, cx - y, cx + y + 1, colour);
			fill_span(cy - x, cx - y, cx + y + 1, colour);
		} else {
			plot_pixel(cx + x, cy + y, colour);
			plot_pixel(cx + y, cy + x, colour);
			plot_pixel(cx - y, cy + x, colour);
			plot_pixel(cx - x, cy + y, colour);
			plot_pixel(cx - x, cy - y, colour);
			plot_pixel(cx - y, cy - x, colour);
			plot_pixel(cx + y, cy - x, colour);
			plot_pixel(cx + x, cy - y, colour);
		}
		y++;
		err += 1 + 2 * y;
		if (2 * (err - x) + 1 > 0) {
			x--;
			err += 1 - 2 * x;
		}
	}
}

static size_t xenon_utf8_decode(const char *s, size_t len, uint32_t *cp)
{
	const unsigned char c = (unsigned char)s[0];

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xf0 && len >= 4) {
		*cp = ((uint32_t)(c & 0x07) << 18) | ((s[1] & 0x3f) << 12) |
		      ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
		return 4;
	}
	if (c >= 0xe0 && len >= 3) {
		*cp = ((uint32_t)(c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
		return 3;
	}
	if (c >= 0xc0 && len >= 2) {
		*cp = ((uint32_t)(c & 0x1f) << 6) | (s[1] & 0x3f);
		return 2;
	}
	*cp = '?';
	return 1;
}

static void draw_glyph(int x, int ytop, uint32_t cp, uint32_t colour, bool bold)
{
	/* The font is ASCII; map the punctuation a page really uses and show
	 * a centred dot for list bullets instead of a question mark. */
	switch (cp) {
	case 0x00a0: cp = ' '; break;
	case 0x00a9: cp = 'c'; break;
	case 0x00ae: cp = 'R'; break;
	case 0x2013:
	case 0x2014: cp = '-'; break;
	case 0x2018:
	case 0x2019: cp = '\''; break;
	case 0x201c:
	case 0x201d: cp = '"'; break;
	case 0x2026: cp = '.'; break;
	case 0x20ac: cp = 'E'; break;
	case 0x2122: cp = 'T'; break;
	case 0x2022:
		fill_rect(x + 2, ytop + 7, x + 6, ytop + 10, colour);
		return;
	default:
		if (cp > 0x7e || cp < 0x20)
			cp = '?';
		break;
	}

	const unsigned char *glyph = &xenon_font8x16[cp * GLYPH_H];
	for (int row = 0; row < GLYPH_H; row++) {
		unsigned char bits = glyph[row];
		if (bits == 0)
			continue;
		for (int col = 0; col < GLYPH_W; col++) {
			if (bits & (0x80 >> col)) {
				plot_pixel(x + col, ytop + row, colour);
				if (bold)
					plot_pixel(x + col + 1, ytop + row, colour);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* NetSurf plotter callbacks                                          */
/* ------------------------------------------------------------------ */

static nserror xenon_plot_clip(const struct redraw_context *ctx, const struct rect *newclip)
{
	(void)ctx;
	clip = *newclip;
	if (clip.x0 < 0)
		clip.x0 = 0;
	if (clip.y0 < 0)
		clip.y0 = 0;
	if (clip.x1 > XENON_WIDTH)
		clip.x1 = XENON_WIDTH;
	if (clip.y1 > XENON_PAGE_HEIGHT)
		clip.y1 = XENON_PAGE_HEIGHT;
	return NSERROR_OK;
}

static nserror xenon_plot_arc(const struct redraw_context *ctx, const plot_style_t *style,
			      int x, int y, int radius, int angle1, int angle2)
{
	(void)ctx;
	(void)angle1;
	(void)angle2;
	if (style->stroke_type != PLOT_OP_TYPE_NONE)
		draw_circle(x, y, radius, to_rgb(style->stroke_colour), false);
	return NSERROR_OK;
}

static nserror xenon_plot_disc(const struct redraw_context *ctx, const plot_style_t *style,
			       int x, int y, int radius)
{
	(void)ctx;
	if (style->fill_type != PLOT_OP_TYPE_NONE)
		draw_circle(x, y, radius, to_rgb(style->fill_colour), true);
	else if (style->stroke_type != PLOT_OP_TYPE_NONE)
		draw_circle(x, y, radius, to_rgb(style->stroke_colour), false);
	return NSERROR_OK;
}

static nserror xenon_plot_line(const struct redraw_context *ctx, const plot_style_t *style,
			       const struct rect *line)
{
	(void)ctx;
	stat_lines++;
	if (style->stroke_type != PLOT_OP_TYPE_NONE) {
		int width = plot_style_fixed_to_int(style->stroke_width);
		if (width < 1)
			width = 1;
		draw_line(line->x0, line->y0, line->x1, line->y1, to_rgb(style->stroke_colour), width);
	}
	return NSERROR_OK;
}

static nserror xenon_plot_rectangle(const struct redraw_context *ctx, const plot_style_t *style,
				    const struct rect *rect)
{
	(void)ctx;
	stat_rects++;

	if (style->fill_type != PLOT_OP_TYPE_NONE)
		fill_rect(rect->x0, rect->y0, rect->x1, rect->y1, to_rgb(style->fill_colour));

	if (style->stroke_type != PLOT_OP_TYPE_NONE) {
		int width = plot_style_fixed_to_int(style->stroke_width);
		uint32_t colour = to_rgb(style->stroke_colour);
		if (width < 1)
			width = 1;
		fill_rect(rect->x0, rect->y0, rect->x1, rect->y0 + width, colour);
		fill_rect(rect->x0, rect->y1 - width, rect->x1, rect->y1, colour);
		fill_rect(rect->x0, rect->y0, rect->x0 + width, rect->y1, colour);
		fill_rect(rect->x1 - width, rect->y0, rect->x1, rect->y1, colour);
	}
	return NSERROR_OK;
}

static nserror xenon_plot_polygon(const struct redraw_context *ctx, const plot_style_t *style,
				  const int *p, unsigned int n)
{
	(void)ctx;
	stat_polygons++;
	if (style->fill_type != PLOT_OP_TYPE_NONE)
		fill_polygon(p, n, to_rgb(style->fill_colour));
	else if (style->stroke_type != PLOT_OP_TYPE_NONE) {
		for (unsigned int i = 0; i < n; i++) {
			unsigned int j = (i + 1) % n;
			draw_line(p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1],
				  to_rgb(style->stroke_colour), 1);
		}
	}
	return NSERROR_OK;
}

static nserror xenon_plot_path(const struct redraw_context *ctx, const plot_style_t *pstyle,
			       const float *p, unsigned int n, const float transform[6])
{
	(void)ctx;
	(void)pstyle;
	(void)p;
	(void)n;
	(void)transform;
	/* Paths are used for some CSS decorations; the stranded netbook browser
	 * skips them for now. */
	return NSERROR_OK;
}

/* 8-bit alpha blend of one channel, exact for 0..255 inputs. */
static inline unsigned blend_channel(unsigned dst, unsigned src, unsigned alpha)
{
	unsigned value = src * alpha + dst * (255 - alpha);

	return (value + 1 + (value >> 8)) >> 8;
}

/* One scaled copy of the bitmap with its top left at (x, y). Nearest
 * neighbour: the console cannot afford filtering for a whole page, and
 * browsers of this era did not either. */
static void draw_bitmap_scaled(struct bitmap *bmap, int x, int y,
			       int width, int height)
{
	const unsigned char *base = bmap->ptr;
	int sw = bmap->width;
	int sh = bmap->height;
	int x0 = x;
	int y0 = y;
	int x1 = x + width;
	int y1 = y + height;

	if (x0 < clip.x0)
		x0 = clip.x0;
	if (y0 < clip.y0)
		y0 = clip.y0;
	if (x1 > clip.x1)
		x1 = clip.x1;
	if (y1 > clip.y1)
		y1 = clip.y1;
	if (x0 >= x1 || y0 >= y1)
		return;

	for (int dy = y0; dy < y1; dy++) {
		int sy = (int)((long long)(dy - y) * sh / height);
		const unsigned char *row;
		uint32_t *dstrow = &xenon_fb[(dy + XENON_CHROME_TOP) * XENON_WIDTH];

		if (sy < 0)
			sy = 0;
		else if (sy >= sh)
			sy = sh - 1;
		row = base + (size_t)sy * bmap->rowstride;

		for (int dx = x0; dx < x1; dx++) {
			int sx = (int)((long long)(dx - x) * sw / width);
			const unsigned char *pixel;
			unsigned alpha;

			if (sx < 0)
				sx = 0;
			else if (sx >= sw)
				sx = sw - 1;
			pixel = row + (size_t)sx * 4;
			alpha = pixel[3];
			if (alpha == 0)
				continue;
			if (alpha == 255) {
				dstrow[dx] = ((uint32_t)pixel[0] << 16) |
					     ((uint32_t)pixel[1] << 8) |
					     pixel[2];
			} else {
				uint32_t dst = dstrow[dx];

				dstrow[dx] =
					(blend_channel((dst >> 16) & 0xff,
						       pixel[0], alpha) << 16) |
					(blend_channel((dst >> 8) & 0xff,
						       pixel[1], alpha) << 8) |
					blend_channel(dst & 0xff, pixel[2],
						      alpha);
			}
		}
	}
}

static nserror xenon_plot_bitmap(const struct redraw_context *ctx, struct bitmap *bitmap,
				 int x, int y, int width, int height, colour bg,
				 bitmap_flags_t flags)
{
	struct bitmap *bmap = bitmap;
	bool repeat_x;
	bool repeat_y;

	(void)ctx;
	(void)bg;

	if (bmap == NULL || bmap->ptr == NULL || width <= 0 || height <= 0)
		return NSERROR_OK;

	stat_bitmaps++;
	repeat_x = (flags & BITMAPF_REPEAT_X) != 0;
	repeat_y = (flags & BITMAPF_REPEAT_Y) != 0;

	if (!repeat_x && !repeat_y) {
		draw_bitmap_scaled(bmap, x, y, width, height);
		return NSERROR_OK;
	}

	/* Tile from the first copy at or above/left of the clip. */
	if (repeat_x)
		while (x > clip.x0)
			x -= width;
	if (repeat_y)
		while (y > clip.y0)
			y -= height;

	for (int tile_y = y; tile_y < clip.y1; tile_y += height)
		for (int tile_x = x; tile_x < clip.x1; tile_x += width)
			draw_bitmap_scaled(bmap, tile_x, tile_y, width, height);

	return NSERROR_OK;
}

static nserror xenon_plot_text(const struct redraw_context *ctx,
			       const struct plot_font_style *fstyle, int x, int y,
			       const char *text, size_t length)
{
	(void)ctx;
	stat_texts++;

	const bool bold = fstyle->weight >= 600;
	const int top = y - BASELINE;
	size_t offset = 0;

	while (offset < length) {
		uint32_t cp;
		offset += xenon_utf8_decode(text + offset, length - offset, &cp);
		draw_glyph(x, top, cp, to_rgb(fstyle->foreground), bold);
		x += GLYPH_W;
	}
	return NSERROR_OK;
}

static const struct plotter_table plotters = {
	.clip = xenon_plot_clip,
	.arc = xenon_plot_arc,
	.disc = xenon_plot_disc,
	.line = xenon_plot_line,
	.rectangle = xenon_plot_rectangle,
	.polygon = xenon_plot_polygon,
	.path = xenon_plot_path,
	.bitmap = xenon_plot_bitmap,
	.text = xenon_plot_text,
	.option_knockout = false,
};

const struct plotter_table *xenon_plotters = &plotters;

void xenon_plot_reset(void)
{
	memset(xenon_fb, 0xff, sizeof(xenon_fb));
	clip.x0 = 0;
	clip.y0 = 0;
	clip.x1 = XENON_WIDTH;
	clip.y1 = XENON_PAGE_HEIGHT;
	stat_rects = 0;
	stat_lines = 0;
	stat_polygons = 0;
	stat_texts = 0;
	stat_bitmaps = 0;
}

void xenon_plot_stats(void)
{
	fprintf(stderr, "xenon: plot rects=%u lines=%u polygons=%u texts=%u "
		"bitmaps=%u\n", stat_rects, stat_lines, stat_polygons,
		stat_texts, stat_bitmaps);
}

static void dump_framebuffer(const char *name)
{
	char path[512];
	size_t used = 0;
	const char *logpath = xsdk_log_path();
	const char *slash = logpath != NULL ? strrchr(logpath, '\\') : NULL;
	FILE *fp;

	if (name == NULL || name[0] == '\0')
		name = "fb";

	if (slash != NULL) {
		used = (size_t)(slash - logpath) + 1;
		if (used >= sizeof(path))
			used = 0;
		else
			memcpy(path, logpath, used);
	}
	if (used == 0) {
		const char *fallback = "game:\\";
		used = strlen(fallback);
		memcpy(path, fallback, used);
	}
	snprintf(path + used, sizeof(path) - used, "%s.bin", name);

	fp = fopen(path, "wb");
	if (fp == NULL) {
		fprintf(stderr, "xenon: cannot write %s\n", path);
		return;
	}

	/* Screenshots include the chrome, which normally lives only in the
	 * tiled present buffer. Draw it into a linear copy for the file. */
	{
		uint32_t *shot = malloc(sizeof(xenon_fb));
		if (shot != NULL) {
			memcpy(shot, xenon_fb, sizeof(xenon_fb));
			xenon_overlay_begin_linear(shot, XENON_WIDTH, XENON_HEIGHT);
			xenon_ui_draw();
			fwrite(shot, sizeof(xenon_fb), 1, fp);
			free(shot);
		} else {
			fwrite(xenon_fb, sizeof(xenon_fb), 1, fp);
		}
	}
	fclose(fp);
	fprintf(stderr, "xenon: wrote %s (%u bytes)\n", path, (unsigned)sizeof(xenon_fb));
}

void xenon_screen_dump(const char *name)
{
	dump_framebuffer(name);
}

/* ------------------------------------------------------------------ */
/* Chrome overlay: drawn into the presented tile buffer, not the page  */
/* ------------------------------------------------------------------ */

static uint32_t *overlay_dst;
static uint32_t overlay_width;
static uint32_t overlay_height;
static bool overlay_linear;

void xenon_overlay_begin(uint32_t *dst, uint32_t width, uint32_t height)
{
	overlay_dst = dst;
	overlay_width = width;
	overlay_height = height;
	overlay_linear = false;
}

void xenon_overlay_begin_linear(uint32_t *dst, uint32_t width, uint32_t height)
{
	overlay_dst = dst;
	overlay_width = width;
	overlay_height = height;
	overlay_linear = true;
}

static inline void overlay_pixel(int x, int y, uint32_t rgb)
{
	if (overlay_dst == NULL || x < 0 || y < 0 ||
	    x >= (int)overlay_width || y >= (int)overlay_height)
		return;
	if (overlay_linear)
		overlay_dst[(uint32_t)y * overlay_width + (uint32_t)x] = rgb;
	else
		overlay_dst[tiled_index((uint32_t)x, (uint32_t)y,
					overlay_width)] = pixel_bgra(rgb);
}

void xenon_overlay_rect(int x0, int y0, int x1, int y1, uint32_t rgb)
{
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int)overlay_width)
		x1 = (int)overlay_width;
	if (y1 > (int)overlay_height)
		y1 = (int)overlay_height;

	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			overlay_pixel(x, y, rgb);
}

void xenon_overlay_text(int x, int ytop, const char *text, uint32_t rgb)
{
	for (; *text != '\0'; text++, x += GLYPH_W) {
		unsigned char c = (unsigned char)*text;
		if (c < 0x20 || c > 0x7e)
			c = '?';
		const unsigned char *glyph = &xenon_font8x16[c * GLYPH_H];
		for (int row = 0; row < GLYPH_H; row++) {
			unsigned char bits = glyph[row];
			if (bits == 0)
				continue;
			for (int col = 0; col < GLYPH_W; col++) {
				if (bits & (0x80 >> col))
					overlay_pixel(x + col, ytop + row, rgb);
			}
		}
	}
}

void xenon_overlay_text_scaled(int x, int ytop, const char *text, uint32_t rgb,
			       int scale)
{
	if (scale < 1)
		scale = 1;
	for (; *text != '\0'; text++, x += GLYPH_W * scale) {
		unsigned char c = (unsigned char)*text;
		if (c < 0x20 || c > 0x7e)
			c = '?';
		const unsigned char *glyph = &xenon_font8x16[c * GLYPH_H];
		for (int row = 0; row < GLYPH_H; row++) {
			unsigned char bits = glyph[row];
			if (bits == 0)
				continue;
			for (int col = 0; col < GLYPH_W; col++) {
				if ((bits & (0x80 >> col)) == 0)
					continue;
				for (int sy = 0; sy < scale; sy++)
					for (int sx = 0; sx < scale; sx++)
						overlay_pixel(x + col * scale + sx,
							      ytop + row * scale + sy, rgb);
			}
		}
	}
}

void xenon_overlay_line(int x0, int y0, int x1, int y1, int width, uint32_t rgb)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;
	int half = width / 2;

	if (width < 1)
		width = 1;

	for (;;) {
		for (int oy = -half; oy < width - half; oy++)
			for (int ox = -half; ox < width - half; ox++)
				overlay_pixel(x0 + ox, y0 + oy, rgb);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x0 += sx;
		}
		if (e2 < dx) {
			err += dx;
			y0 += sy;
		}
	}
}

void xenon_overlay_disc(int cx, int cy, int radius, uint32_t rgb)
{
	if (radius < 0)
		return;
	for (int y = -radius; y <= radius; y++) {
		int x = 0;
		while ((x + 1) * (x + 1) + y * y <= radius * radius)
			x++;
		xenon_overlay_rect(cx - x, cy + y, cx + x + 1, cy + y + 1, rgb);
	}
}

void xenon_overlay_ring(int cx, int cy, int radius, int thickness, uint32_t rgb)
{
	int inner = radius - thickness;
	if (inner < 0)
		inner = 0;
	for (int y = -radius; y <= radius; y++) {
		for (int x = -radius; x <= radius; x++) {
			int d = x * x + y * y;
			if (d <= radius * radius && d >= inner * inner)
				overlay_pixel(cx + x, cy + y, rgb);
		}
	}
}

static int round_inset(int dy, int radius)
{
	int dx = 0;
	if (dy < 0)
		dy = -dy;
	while (dx < radius && (radius - dx) * (radius - dx) + dy * dy >
	       radius * radius)
		dx++;
	return dx;
}

static void round_span(int x0, int y0, int x1, int y1, int radius, int y,
		       int *sx0, int *sx1)
{
	int dy = 0;

	if (radius > (x1 - x0) / 2)
		radius = (x1 - x0) / 2;
	if (radius > (y1 - y0) / 2)
		radius = (y1 - y0) / 2;
	if (radius < 0)
		radius = 0;

	if (y < y0 + radius)
		dy = y0 + radius - y;
	else if (y >= y1 - radius)
		dy = y - (y1 - radius) + 1;

	*sx0 = x0 + (dy > 0 ? round_inset(dy, radius) : 0);
	*sx1 = x1 - (dy > 0 ? round_inset(dy, radius) : 0);
	if (*sx1 < *sx0)
		*sx1 = *sx0;
}

void xenon_overlay_round_rect(int x0, int y0, int x1, int y1, int radius,
			      uint32_t rgb)
{
	int sx0, sx1;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int)overlay_width)
		x1 = (int)overlay_width;
	if (y1 > (int)overlay_height)
		y1 = (int)overlay_height;
	if (x0 >= x1 || y0 >= y1)
		return;

	for (int y = y0; y < y1; y++) {
		round_span(x0, y0, x1, y1, radius, y, &sx0, &sx1);
		for (int x = sx0; x < sx1; x++)
			overlay_pixel(x, y, rgb);
	}
}

void xenon_overlay_round_frame(int x0, int y0, int x1, int y1, int radius,
			       int thickness, uint32_t rgb)
{
	int ix0 = x0 + thickness;
	int iy0 = y0 + thickness;
	int ix1 = x1 - thickness;
	int iy1 = y1 - thickness;
	int ir = radius - thickness;

	if (ir < 0)
		ir = 0;

	for (int y = y0; y < y1; y++) {
		int ox0, ox1, nx0 = 1, nx1 = 0;
		round_span(x0, y0, x1, y1, radius, y, &ox0, &ox1);
		if (y >= iy0 && y < iy1)
			round_span(ix0, iy0, ix1, iy1, ir, y, &nx0, &nx1);

		for (int x = ox0; x < ox1; x++) {
			if (x >= nx0 && x < nx1)
				continue;
			overlay_pixel(x, y, rgb);
		}
	}
}

void xenon_overlay_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
			    uint32_t rgb)
{
	int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
	int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
	int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);

	for (int y = miny; y <= maxy; y++) {
		for (int x = minx; x <= maxx; x++) {
			int d1 = (x - x1) * (y0 - y1) - (x0 - x1) * (y - y1);
			int d2 = (x - x2) * (y1 - y2) - (x1 - x2) * (y - y2);
			int d3 = (x - x0) * (y2 - y0) - (x2 - x0) * (y - y0);
			bool neg = d1 < 0 || d2 < 0 || d3 < 0;
			bool pos = d1 > 0 || d2 > 0 || d3 > 0;
			if (!(neg && pos))
				overlay_pixel(x, y, rgb);
		}
	}
}

/* A 12x19 arrow, doubled for the TV. X is the outline, # the fill. */
static const char *const cursor_shape[] = {
	"X...........",
	"XX..........",
	"X#X.........",
	"X##X........",
	"X###X.......",
	"X####X......",
	"X#####X.....",
	"X######X....",
	"X#######X...",
	"X########X..",
	"X#####XXXXX.",
	"X##X##X.....",
	"X#X.X##X....",
	"XX..X##X....",
	"X....X##X...",
	".....X##X...",
	"......X##X..",
	"......X##X..",
	".......XX...",
};

void xenon_overlay_arrow(int x, int y)
{
	for (size_t row = 0; row < sizeof(cursor_shape) / sizeof(cursor_shape[0]); row++) {
		for (int col = 0; cursor_shape[row][col] != '\0'; col++) {
			if (cursor_shape[row][col] == '.')
				continue;
			uint32_t colour = cursor_shape[row][col] == 'X' ? 0x000000 : 0xffffff;
			for (int sy = 0; sy < CURSOR_SCALE; sy++)
				for (int sx = 0; sx < CURSOR_SCALE; sx++)
					overlay_pixel(x + col * CURSOR_SCALE + sx,
						      y + (int)row * CURSOR_SCALE + sy, colour);
		}
	}
}


/* The display stays open between presents: closing it gives XAM's surface
 * back to the screen, so the last presented frame would only last a vblank. */
static bool screen_open;
static uint32_t screen_width;
static uint32_t screen_height;

static bool screen_ensure_open(void)
{
	NTSTATUS status;

	if (screen_open)
		return true;

	status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		fprintf(stderr, "xenon: gpu open failed 0x%08X\n", (unsigned)status);
		return false;
	}

	status = xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &screen_width, &screen_height);
	if (!NT_SUCCESS(status)) {
		fprintf(stderr, "xenon: no display 0x%08X\n", (unsigned)status);
		return false;
	}

	screen_open = true;
	return true;
}

void xenon_screen_present(void)
{
	uint32_t address = 0;
	NTSTATUS status;

	if (!screen_ensure_open())
		return;

	status = xsdk_present_begin(&address, 1000);
	if (!NT_SUCCESS(status)) {
		fprintf(stderr, "xenon: present begin failed 0x%08X\n", (unsigned)status);
		return;
	}

	volatile uint32_t *dst = xsdk_present_pixels(address);
	for (uint32_t y = 0; y < XENON_HEIGHT && y < screen_height; y++) {
		for (uint32_t x = 0; x < XENON_WIDTH && x < screen_width; x++) {
			dst[tiled_index(x, y, screen_width)] =
				pixel_bgra(xenon_fb[y * XENON_WIDTH + x]);
		}
	}
	__asm__ __volatile__("sync" ::: "memory");

	xenon_overlay_begin(dst, screen_width, screen_height);
	xenon_ui_draw();
	__asm__ __volatile__("sync" ::: "memory");

	xsdk_present_end();

	fprintf(stderr, "xenon: presented %ux%u\n", (unsigned)screen_width, (unsigned)screen_height);
}

void xenon_screen_close(void)
{
	if (!screen_open)
		return;

	xsdk_present_close();
	screen_open = false;
}
