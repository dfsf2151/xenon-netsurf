/*
 * Xenon (Xbox 360) fonts: Noto TrueType faces rasterised by stb_truetype
 * into a cache of anti-aliased glyphs.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#ifndef NETSURF_XENON_FONT_H
#define NETSURF_XENON_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct plot_font_style;

typedef enum {
	XENON_FACE_SANS,
	XENON_FACE_SANS_BOLD,
	XENON_FACE_SANS_ITALIC,
	XENON_FACE_SANS_BOLD_ITALIC,
	XENON_FACE_SERIF,
	XENON_FACE_SERIF_BOLD,
	XENON_FACE_MONO,
	XENON_FACE_MONO_BOLD,
	XENON_FACE_COUNT,
} xenon_face;

/* A destination for text: 0x00RRGGBB pixels, a row stride in pixels and a
 * clip rectangle in the same coordinates. */
struct xenon_surface {
	uint32_t *pixels;
	int stride;
	int clip_x0;
	int clip_y0;
	int clip_x1;
	int clip_y1;
};

/* False when no face could be read; the 8x16 font then stands in. */
bool xenon_font_available(void);

xenon_face xenon_font_face_for(const struct plot_font_style *fstyle);
int xenon_font_px_for(const struct plot_font_style *fstyle);

/* Width in pixels of the first length bytes of a UTF-8 string. */
int xenon_font_width(xenon_face face, int px, const char *text, size_t length);

/* The byte offset of the character whose left edge is nearest x (round) or
 * the last one starting at or before x (floor), and that edge's x. */
size_t xenon_font_offset_at(xenon_face face, int px, const char *text,
			    size_t length, int x, bool round, int *edge_x);

/* Ascent and descent above and below the baseline, in pixels. */
void xenon_font_metrics(xenon_face face, int px, int *ascent, int *descent);

/* Draws text with its baseline at y; returns the pen position after it. */
int xenon_font_draw(const struct xenon_surface *surface, xenon_face face,
		    int px, int x, int y, const char *text, size_t length,
		    uint32_t rgb);

/* Releases every cached glyph (for low memory). */
void xenon_font_flush(void);

void xenon_font_stats(void);

#endif /* NETSURF_XENON_FONT_H */
