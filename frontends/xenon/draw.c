/*
 * Xenon (Xbox 360) chrome drawing: anti-aliased shapes and text into a
 * linear 0x00RRGGBB buffer. Shapes take coverage from their distance to
 * the edge, so circles and rounded corners blend half a pixel wide.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "xenon/font.h"
#include "xenon/xenon.h"

#define CURSOR_SCALE 2

static uint32_t *dst;
static int dst_w;
static int dst_h;
static int cx0, cy0, cx1, cy1; /* clip */

void xenon_overlay_begin_linear(uint32_t *buffer, uint32_t width, uint32_t height)
{
	dst = buffer;
	dst_w = (int)width;
	dst_h = (int)height;
	xenon_overlay_unclip();
}

void xenon_overlay_clip(int x0, int y0, int x1, int y1)
{
	cx0 = x0 < 0 ? 0 : x0;
	cy0 = y0 < 0 ? 0 : y0;
	cx1 = x1 > dst_w ? dst_w : x1;
	cy1 = y1 > dst_h ? dst_h : y1;
}

void xenon_overlay_unclip(void)
{
	cx0 = 0;
	cy0 = 0;
	cx1 = dst_w;
	cy1 = dst_h;
}

static inline uint32_t blend(uint32_t d, uint32_t s, unsigned a)
{
	uint32_t rb = (s & 0xff00ff) * a + (d & 0xff00ff) * (255 - a) + 0x800080;
	uint32_t g = (s & 0x00ff00) * a + (d & 0x00ff00) * (255 - a) + 0x008000;

	rb = ((rb + ((rb >> 8) & 0xff00ff)) >> 8) & 0xff00ff;
	g = ((g + ((g >> 8) & 0x00ff00)) >> 8) & 0x00ff00;
	return rb | g;
}

static inline void put(int x, int y, uint32_t rgb, unsigned a)
{
	uint32_t *p;

	if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1 || a == 0)
		return;
	p = &dst[y * dst_w + x];
	*p = a >= 255 ? rgb : blend(*p, rgb, a);
}

static void span(int y, int x0, int x1, uint32_t rgb, unsigned a)
{
	uint32_t *row;

	if (dst == NULL || y < cy0 || y >= cy1)
		return;
	if (x0 < cx0)
		x0 = cx0;
	if (x1 > cx1)
		x1 = cx1;
	if (x0 >= x1)
		return;
	row = &dst[y * dst_w];
	if (a >= 255) {
		for (int x = x0; x < x1; x++)
			row[x] = rgb;
	} else {
		for (int x = x0; x < x1; x++)
			row[x] = blend(row[x], rgb, a);
	}
}

void xenon_overlay_rect_a(int x0, int y0, int x1, int y1, uint32_t rgb, unsigned alpha)
{
	if (y0 < cy0)
		y0 = cy0;
	if (y1 > cy1)
		y1 = cy1;
	for (int y = y0; y < y1; y++)
		span(y, x0, x1, rgb, alpha);
}

void xenon_overlay_rect(int x0, int y0, int x1, int y1, uint32_t rgb)
{
	xenon_overlay_rect_a(x0, y0, x1, y1, rgb, 255);
}

void xenon_overlay_gradient(int x0, int y0, int x1, int y1, uint32_t top, uint32_t bottom)
{
	int h = y1 - y0;

	for (int y = y0; y < y1; y++) {
		unsigned t = h > 1 ? (unsigned)((y - y0) * 255 / (h - 1)) : 0;
		span(y, x0, x1, blend(top, bottom, t), 255);
	}
}

static inline unsigned coverage(float distance_inside)
{
	/* distance_inside: positive inside the edge, in pixels. */
	float c = distance_inside + 0.5f;
	if (c <= 0.0f)
		return 0;
	if (c >= 1.0f)
		return 255;
	return (unsigned)(c * 255.0f + 0.5f);
}

/* Rows of a span that leave out a hole: the part of a shadow a card will
 * cover anyway. */
static int hole_x0, hole_y0, hole_x1, hole_y1;

static void span_holed(int y, int x0, int x1, uint32_t rgb, unsigned a)
{
	if (y >= hole_y0 && y < hole_y1 && hole_x0 < x1 && hole_x1 > x0) {
		span(y, x0, hole_x0, rgb, a);
		span(y, hole_x1, x1, rgb, a);
	} else {
		span(y, x0, x1, rgb, a);
	}
}

/* A rounded rectangle with anti-aliased corners, filled with alpha. */
void xenon_overlay_round_rect_a(int x0, int y0, int x1, int y1, int radius,
				uint32_t rgb, unsigned alpha)
{
	int w = x1 - x0;
	int h = y1 - y0;

	if (w <= 0 || h <= 0 || dst == NULL)
		return;
	if (radius > w / 2)
		radius = w / 2;
	if (radius > h / 2)
		radius = h / 2;
	if (radius <= 0) {
		xenon_overlay_rect_a(x0, y0, x1, y1, rgb, alpha);
		return;
	}

	for (int y = y0; y < y1; y++) {
		int dy = 0;
		float fy;

		if (y < cy0 || y >= cy1)
			continue;
		if (y < y0 + radius)
			dy = y0 + radius - y;
		else if (y >= y1 - radius)
			dy = y - (y1 - radius - 1);
		if (dy == 0) {
			span_holed(y, x0, x1, rgb, alpha);
			continue;
		}

		fy = (float)dy - 0.5f;
		for (int i = 0; i < radius; i++) {
			float fx = (float)(radius - i) - 0.5f;
			float d = (float)radius - sqrtf(fx * fx + fy * fy);
			unsigned a = coverage(d) * alpha / 255;
			put(x0 + i, y, rgb, a);
			put(x1 - 1 - i, y, rgb, a);
		}
		span_holed(y, x0 + radius, x1 - radius, rgb, alpha);
	}
}

void xenon_overlay_round_rect(int x0, int y0, int x1, int y1, int radius, uint32_t rgb)
{
	xenon_overlay_round_rect_a(x0, y0, x1, y1, radius, rgb, 255);
}

/* A frame: the outer rounded rectangle minus the inner one, by distance. */
void xenon_overlay_round_frame(int x0, int y0, int x1, int y1, int radius,
			       int thickness, uint32_t rgb)
{
	float half_w = (float)(x1 - x0) / 2.0f;
	float half_h = (float)(y1 - y0) / 2.0f;
	float mid_x = (float)(x0 + x1) / 2.0f;
	float mid_y = (float)(y0 + y1) / 2.0f;
	float r = (float)radius;

	if (x1 <= x0 || y1 <= y0 || dst == NULL)
		return;
	if (r > half_w)
		r = half_w;
	if (r > half_h)
		r = half_h;

	for (int y = y0; y < y1; y++) {
		if (y < cy0 || y >= cy1)
			continue;
		bool band_y = y < y0 + thickness + (int)r + 1 || y >= y1 - thickness - (int)r - 1;
		for (int x = x0; x < x1; x++) {
			bool band_x = x < x0 + thickness + (int)r + 1 || x >= x1 - thickness - (int)r - 1;
			float px, py, qx, qy, outside, d;

			if (!band_x && !band_y)
				continue;
			/* Signed distance to a rounded box (Inigo Quilez's form). */
			px = fabsf((float)x + 0.5f - mid_x) - (half_w - r);
			py = fabsf((float)y + 0.5f - mid_y) - (half_h - r);
			qx = px > 0.0f ? px : 0.0f;
			qy = py > 0.0f ? py : 0.0f;
			outside = sqrtf(qx * qx + qy * qy) +
				  (px > py ? (px < 0.0f ? px : 0.0f) : (py < 0.0f ? py : 0.0f)) - r;
			d = -outside; /* inside distance from the outer edge */
			unsigned a_out = coverage(d);
			unsigned a_in = coverage((float)thickness - d);
			unsigned a = a_out < a_in ? a_out : a_in;
			put(x, y, rgb, a);
		}
	}
}

/* A soft shadow under a card: rings of falling alpha. */
void xenon_overlay_shadow(int x0, int y0, int x1, int y1, int radius, int spread,
			  unsigned alpha)
{
	/* The card covers its own middle rows, so leave them out. */
	hole_x0 = x0;
	hole_x1 = x1;
	hole_y0 = y0 + radius;
	hole_y1 = y1 - radius;
	for (int i = spread; i >= 1; i--) {
		unsigned a = alpha * (unsigned)(spread - i + 1) / (unsigned)(spread * (spread + 1) / 2);
		xenon_overlay_round_rect_a(x0 - i, y0 - i + spread / 2, x1 + i,
					   y1 + i + spread / 2, radius + i, 0x000000, a);
	}
	hole_y0 = hole_y1 = 0;
}

void xenon_overlay_disc_a(int cx, int cy, int radius, uint32_t rgb, unsigned alpha)
{
	float r = (float)radius;

	for (int y = cy - radius - 1; y <= cy + radius + 1; y++) {
		float fy = (float)y + 0.5f - ((float)cy + 0.5f);
		if (y < cy0 || y >= cy1)
			continue;
		for (int x = cx - radius - 1; x <= cx + radius + 1; x++) {
			float fx = (float)x + 0.5f - ((float)cx + 0.5f);
			float d = r - sqrtf(fx * fx + fy * fy);
			put(x, y, rgb, coverage(d) * alpha / 255);
		}
	}
}

void xenon_overlay_disc(int cx, int cy, int radius, uint32_t rgb)
{
	xenon_overlay_disc_a(cx, cy, radius, rgb, 255);
}

void xenon_overlay_ring(int cx, int cy, int radius, int thickness, uint32_t rgb)
{
	float r = (float)radius;

	for (int y = cy - radius - 1; y <= cy + radius + 1; y++) {
		float fy = (float)y + 0.5f - ((float)cy + 0.5f);
		if (y < cy0 || y >= cy1)
			continue;
		for (int x = cx - radius - 1; x <= cx + radius + 1; x++) {
			float fx = (float)x + 0.5f - ((float)cx + 0.5f);
			float d = r - sqrtf(fx * fx + fy * fy);
			unsigned a_out = coverage(d);
			unsigned a_in = coverage((float)thickness - d);
			put(x, y, rgb, a_out < a_in ? a_out : a_in);
		}
	}
}

/* A thick line with round ends, by distance to the segment. */
void xenon_overlay_line(int x0, int y0, int x1, int y1, int width, uint32_t rgb)
{
	float ax = (float)x0 + 0.5f, ay = (float)y0 + 0.5f;
	float bx = (float)x1 + 0.5f, by = (float)y1 + 0.5f;
	float dx = bx - ax, dy = by - ay;
	float len2 = dx * dx + dy * dy;
	float half = (float)(width < 1 ? 1 : width) / 2.0f;
	int minx = (x0 < x1 ? x0 : x1) - width - 1;
	int maxx = (x0 > x1 ? x0 : x1) + width + 1;
	int miny = (y0 < y1 ? y0 : y1) - width - 1;
	int maxy = (y0 > y1 ? y0 : y1) + width + 1;

	for (int y = miny; y <= maxy; y++) {
		if (y < cy0 || y >= cy1)
			continue;
		for (int x = minx; x <= maxx; x++) {
			float px = (float)x + 0.5f - ax, py = (float)y + 0.5f - ay;
			float t = len2 > 0.0f ? (px * dx + py * dy) / len2 : 0.0f;
			float qx, qy;
			if (t < 0.0f)
				t = 0.0f;
			if (t > 1.0f)
				t = 1.0f;
			qx = px - t * dx;
			qy = py - t * dy;
			put(x, y, rgb, coverage(half - sqrtf(qx * qx + qy * qy)));
		}
	}
}

void xenon_overlay_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t rgb)
{
	const float vx[3] = { (float)x0, (float)x1, (float)x2 };
	const float vy[3] = { (float)y0, (float)y1, (float)y2 };
	int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
	int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
	int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
	float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vx[2] - vx[0]) * (vy[1] - vy[0]);
	float sign = area > 0.0f ? 1.0f : -1.0f;
	float len[3];

	if (area == 0.0f)
		return;
	for (int i = 0; i < 3; i++) {
		int j = (i + 1) % 3;
		float ex = vx[j] - vx[i], ey = vy[j] - vy[i];
		len[i] = sqrtf(ex * ex + ey * ey);
	}
	for (int y = miny - 1; y <= maxy + 1; y++) {
		if (y < cy0 || y >= cy1)
			continue;
		for (int x = minx - 1; x <= maxx + 1; x++) {
			float px = (float)x + 0.5f, py = (float)y + 0.5f;
			float d = 1e9f;
			for (int i = 0; i < 3; i++) {
				int j = (i + 1) % 3;
				/* Signed distance to edge i, positive on the inside. */
				float f = (vx[j] - vx[i]) * (py - vy[i]) -
					  (vy[j] - vy[i]) * (px - vx[i]);
				float v = sign * f / len[i];
				if (v < d)
					d = v;
			}
			put(x, y, rgb, coverage(d));
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
	/* Drop shadow first, then the arrow. */
	for (size_t row = 0; row < sizeof(cursor_shape) / sizeof(cursor_shape[0]); row++) {
		for (int col = 0; cursor_shape[row][col] != '\0'; col++) {
			if (cursor_shape[row][col] == '.')
				continue;
			for (int sy = 0; sy < CURSOR_SCALE; sy++)
				for (int sx = 0; sx < CURSOR_SCALE; sx++)
					put(x + 3 + col * CURSOR_SCALE + sx,
					    y + 3 + (int)row * CURSOR_SCALE + sy, 0x000000, 90);
		}
	}
	for (size_t row = 0; row < sizeof(cursor_shape) / sizeof(cursor_shape[0]); row++) {
		for (int col = 0; cursor_shape[row][col] != '\0'; col++) {
			if (cursor_shape[row][col] == '.')
				continue;
			uint32_t colour = cursor_shape[row][col] == 'X' ? 0x101418 : 0xffffff;
			for (int sy = 0; sy < CURSOR_SCALE; sy++)
				for (int sx = 0; sx < CURSOR_SCALE; sx++)
					put(x + col * CURSOR_SCALE + sx,
					    y + (int)row * CURSOR_SCALE + sy, colour, 255);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Text                                                               */
/* ------------------------------------------------------------------ */

static xenon_face ui_face(int weight)
{
	return weight == XENON_TEXT_BOLD ? XENON_FACE_SANS_BOLD : XENON_FACE_SANS;
}

int xenon_overlay_text_width(const char *text, int px, int weight)
{
	return xenon_font_width(ui_face(weight), px, text, strlen(text));
}

int xenon_overlay_text(int x, int y, const char *text, uint32_t rgb, int px, int weight)
{
	struct xenon_surface surface = {
		.pixels = dst,
		.stride = dst_w,
		.clip_x0 = cx0,
		.clip_y0 = cy0,
		.clip_x1 = cx1,
		.clip_y1 = cy1,
	};

	if (dst == NULL)
		return x;
	return xenon_font_draw(&surface, ui_face(weight), px, x, y, text, strlen(text), rgb);
}

int xenon_overlay_text_fit(int x, int y, const char *text, uint32_t rgb, int px,
			   int weight, int max_width)
{
	static const char ellipsis[] = "\xe2\x80\xa6";
	size_t length = strlen(text);
	int edge;
	size_t fit;
	char buffer[512];

	if (xenon_font_width(ui_face(weight), px, text, length) <= max_width)
		return xenon_overlay_text(x, y, text, rgb, px, weight);

	max_width -= xenon_font_width(ui_face(weight), px, ellipsis, 3);
	if (max_width < 0)
		max_width = 0;
	fit = xenon_font_offset_at(ui_face(weight), px, text, length, max_width, false, &edge);
	if (fit > sizeof(buffer) - 4)
		fit = sizeof(buffer) - 4;
	memcpy(buffer, text, fit);
	memcpy(buffer + fit, ellipsis, 4);
	return xenon_overlay_text(x, y, buffer, rgb, px, weight);
}
