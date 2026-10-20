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
#include <time.h>

#include "utils/log.h"
#include "utils/utf8.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"

#include "xenon/bitmap.h"
#include "xenon/font.h"
#include "xenon/plot.h"
#include "xenon/ui.h"
#include "xenon/xenon.h"

#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/present.h"
#include "xsdk/log.h"

uint32_t xenon_fb[XENON_WIDTH * XENON_HEIGHT];

static struct rect clip = { 0, 0, XENON_WIDTH, XENON_PAGE_HEIGHT };

static unsigned int stat_rects;
static unsigned int stat_lines;
static unsigned int stat_polygons;
static unsigned int stat_texts;
static unsigned int stat_bitmaps;

/* libxenon:drivers/console/console.c:60-61, BSD 3-clause, Copyright 2009 Georg Lukas. */
static uint32_t tiled_index(uint32_t x, uint32_t y, uint32_t width)
{
	return ((y >> 5) * 32 * width + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) +
		(((x & 31) >> 2) << 3) + (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2);
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
	struct xenon_surface surface = {
		.pixels = xenon_fb + XENON_CHROME_TOP * XENON_WIDTH,
		.stride = XENON_WIDTH,
		.clip_x0 = clip.x0,
		.clip_y0 = clip.y0,
		.clip_x1 = clip.x1,
		.clip_y1 = clip.y1,
	};

	(void)ctx;
	stat_texts++;
	xenon_font_draw(&surface, xenon_font_face_for(fstyle),
			xenon_font_px_for(fstyle), x, y, text, length,
			to_rgb(fstyle->foreground));
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

void xenon_plot_begin(void)
{
	stat_rects = 0;
	stat_lines = 0;
	stat_polygons = 0;
	stat_texts = 0;
	stat_bitmaps = 0;
}

void xenon_plot_clear(const struct rect *area)
{
	for (int y = area->y0; y < area->y1; y++) {
		uint32_t *row = &xenon_fb[(y + XENON_CHROME_TOP) * XENON_WIDTH];
		for (int x = area->x0; x < area->x1; x++)
			row[x] = 0xffffff;
	}
	clip = *area;
}

void xenon_plot_shift_page(int dy)
{
	uint32_t *page = &xenon_fb[XENON_CHROME_TOP * XENON_WIDTH];
	size_t keep = (size_t)(XENON_PAGE_HEIGHT - (dy > 0 ? dy : -dy)) * XENON_WIDTH;

	if (dy > 0)
		memmove(page, page + (size_t)dy * XENON_WIDTH, keep * 4);
	else if (dy < 0)
		memmove(page + (size_t)(-dy) * XENON_WIDTH, page, keep * 4);
}

void xenon_plot_stats(void)
{
	fprintf(stderr, "xenon: plot rects=%u lines=%u polygons=%u texts=%u "
		"bitmaps=%u\n", stat_rects, stat_lines, stat_polygons,
		stat_texts, stat_bitmaps);
}

/* ------------------------------------------------------------------ */
/* Presenting: page + chrome composed linearly, then tiled             */
/* ------------------------------------------------------------------ */

/* The page buffer stays as NetSurf drew it; each present copies it here,
 * draws the chrome over the copy, and tiles the copy to the screen. So
 * moving the pointer or opening a menu never lays the page out again. */
static uint32_t compose[XENON_WIDTH * XENON_HEIGHT];

/* For the 1024 pixels of one 32x32 display tile, in the order they sit in
 * memory, each pixel's offset in a linear 1280-wide buffer from the tile's
 * corner: the inverse of tiled_index. Writing a tile in memory order keeps
 * the stores to the write-combined screen sequential. */
static uint32_t tile_order[1024];
static int tile_order_y[1024];
static bool tile_order_ready;

static void make_tile_order(void)
{
	for (uint32_t y = 0; y < 32; y++) {
		for (uint32_t x = 0; x < 32; x++) {
			uint32_t o = tiled_index(x, y, XENON_WIDTH);
			tile_order[o] = y * XENON_WIDTH + x;
			tile_order_y[o] = (int)y;
		}
	}
	tile_order_ready = true;
}

static inline uint32_t to_screen(uint32_t rgb)
{
	return __builtin_bswap32(rgb) | 0xff;
}

enum { TILES_X = XENON_WIDTH / 32, TILES_Y = (XENON_HEIGHT + 31) / 32 };

static void tile_one(volatile uint32_t *out, const uint32_t *src, int tx, int ty)
{
	const uint32_t *corner = src + (size_t)ty * 32 * XENON_WIDTH + tx * 32;
	volatile uint32_t *tile = out + ((size_t)ty * TILES_X + tx) * 1024;
	int rows_left = XENON_HEIGHT - ty * 32;

	if (rows_left >= 32) {
		for (int o = 0; o < 1024; o++)
			tile[o] = to_screen(corner[tile_order[o]]);
	} else {
		for (int o = 0; o < 1024; o++)
			if (tile_order_y[o] < rows_left)
				tile[o] = to_screen(corner[tile_order[o]]);
	}
}

static void tile_out(volatile uint32_t *out, const uint32_t *src)
{
	if (!tile_order_ready)
		make_tile_order();
	for (int ty = 0; ty < TILES_Y; ty++)
		for (int tx = 0; tx < TILES_X; tx++)
			tile_one(out, src, tx, ty);
}

/* Writes the tiles that a rectangle touches. */
static void tile_rect(volatile uint32_t *out, const uint32_t *src,
		      int x0, int y0, int x1, int y1)
{
	if (!tile_order_ready)
		make_tile_order();
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > XENON_WIDTH)
		x1 = XENON_WIDTH;
	if (y1 > XENON_HEIGHT)
		y1 = XENON_HEIGHT;
	for (int ty = y0 / 32; ty * 32 < y1; ty++)
		for (int tx = x0 / 32; tx * 32 < x1; tx++)
			tile_one(out, src, tx, ty);
}

static void compose_frame(uint32_t *target)
{
	memcpy(target, xenon_fb, sizeof(xenon_fb));
	xenon_overlay_begin_linear(target, XENON_WIDTH, XENON_HEIGHT);
	xenon_ui_draw();
}

/* The pointer is drawn into the composed frame only while its tiles are
 * written, then the pixels under it are put back, so the composed frame
 * stays pointer-free and a pointer move needs no new compose. */
enum { CURSOR_W = 30, CURSOR_H = 44, PRESENT_TIMEOUT_MS = 40 };

struct cursor_box {
	bool shown;
	int x0, y0, x1, y1;
};

/* What each front buffer last received. */
struct buffer_state {
	uint32_t address;
	unsigned generation; /* the compose it holds; 0 = unknown */
	struct cursor_box cursor;
};

static uint32_t cursor_under[CURSOR_W * CURSOR_H];
static struct buffer_state buffers[4];
static unsigned next_buffer_slot;
static unsigned compose_generation;
static unsigned present_count;
static unsigned present_partial;
static uint64_t present_us_total;

static struct buffer_state *buffer_for(uint32_t address)
{
	struct buffer_state *b;

	for (unsigned i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
		if (buffers[i].address == address)
			return &buffers[i];
	b = &buffers[next_buffer_slot++ % (sizeof(buffers) / sizeof(buffers[0]))];
	b->address = address;
	b->generation = 0;
	b->cursor.shown = false;
	return b;
}

static void cursor_box_at(struct cursor_box *box, int x, int y)
{
	box->shown = true;
	box->x0 = x;
	box->y0 = y;
	box->x1 = x + CURSOR_W > XENON_WIDTH ? XENON_WIDTH : x + CURSOR_W;
	box->y1 = y + CURSOR_H > XENON_HEIGHT ? XENON_HEIGHT : y + CURSOR_H;
}

static void cursor_put(const struct cursor_box *box, int x, int y)
{
	int w = box->x1 - box->x0;

	for (int y_ = box->y0; y_ < box->y1; y_++)
		memcpy(&cursor_under[(y_ - box->y0) * CURSOR_W],
		       &compose[y_ * XENON_WIDTH + box->x0], (size_t)w * 4);
	xenon_overlay_begin_linear(compose, XENON_WIDTH, XENON_HEIGHT);
	xenon_overlay_arrow(x, y);
}

static void cursor_take(const struct cursor_box *box)
{
	int w = box->x1 - box->x0;

	for (int y_ = box->y0; y_ < box->y1; y_++)
		memcpy(&compose[y_ * XENON_WIDTH + box->x0],
		       &cursor_under[(y_ - box->y0) * CURSOR_W], (size_t)w * 4);
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

	/* Screenshots include the chrome and the pointer. */
	{
		struct cursor_box box = { false, 0, 0, 0, 0 };
		int cx, cy;

		compose_frame(compose);
		compose_generation++;
		if (xenon_ui_cursor(&cx, &cy)) {
			cursor_box_at(&box, cx, cy);
			cursor_put(&box, cx, cy);
		}
		fwrite(compose, sizeof(compose), 1, fp);
		if (box.shown)
			cursor_take(&box);
	}
	fclose(fp);
	fprintf(stderr, "xenon: wrote %s (%u bytes)\n", path, (unsigned)sizeof(compose));
}

void xenon_screen_dump(const char *name)
{
	dump_framebuffer(name);
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

	fprintf(stderr, "xenon: display %ux%u\n", (unsigned)screen_width,
		(unsigned)screen_height);
	screen_open = true;
	return true;
}

bool xenon_screen_present(bool full)
{
	uint32_t address = 0;
	NTSTATUS status;
	struct timespec t0, t1;
	struct buffer_state *buffer;
	struct cursor_box box = { false, 0, 0, 0, 0 };
	int cx, cy;
	bool partial;

	if (!screen_ensure_open())
		return true; /* nothing to show on; don't retry forever */

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (full || compose_generation == 0) {
		compose_frame(compose);
		compose_generation++;
		if (compose_generation == 0)
			compose_generation = 1;
	}

	status = xsdk_present_begin(&address, PRESENT_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		return false;

	volatile uint32_t *out = xsdk_present_pixels(address);
	buffer = buffer_for(address);

	if (xenon_ui_cursor(&cx, &cy)) {
		cursor_box_at(&box, cx, cy);
		cursor_put(&box, cx, cy);
	}

	partial = buffer->generation == compose_generation &&
		  screen_width == XENON_WIDTH && screen_height == XENON_HEIGHT;
	if (partial) {
		if (buffer->cursor.shown)
			tile_rect(out, compose, buffer->cursor.x0, buffer->cursor.y0,
				  buffer->cursor.x1, buffer->cursor.y1);
		if (box.shown)
			tile_rect(out, compose, box.x0, box.y0, box.x1, box.y1);
		present_partial++;
	} else if (screen_width == XENON_WIDTH && screen_height == XENON_HEIGHT) {
		tile_out(out, compose);
	} else {
		for (uint32_t y = 0; y < XENON_HEIGHT && y < screen_height; y++)
			for (uint32_t x = 0; x < XENON_WIDTH && x < screen_width; x++)
				out[tiled_index(x, y, screen_width)] =
					to_screen(compose[y * XENON_WIDTH + x]);
	}

	if (box.shown)
		cursor_take(&box);
	buffer->generation = compose_generation;
	buffer->cursor = box;

	__asm__ __volatile__("sync" ::: "memory");
	xsdk_present_end();

	clock_gettime(CLOCK_MONOTONIC, &t1);
	present_us_total += (uint64_t)((int64_t)(t1.tv_sec - t0.tv_sec) * 1000000 +
				       ((int64_t)t1.tv_nsec - (int64_t)t0.tv_nsec) / 1000);
	present_count++;
	return true;
}

void xenon_screen_stats(void)
{
	if (present_count > 0)
		fprintf(stderr, "xenon: %u presents (%u pointer-only), %u us each "
			"on average\n", present_count, present_partial,
			(unsigned)(present_us_total / present_count));
	xenon_font_stats();
}

void xenon_screen_close(void)
{
	if (!screen_open)
		return;

	xsdk_present_close();
	screen_open = false;
}
