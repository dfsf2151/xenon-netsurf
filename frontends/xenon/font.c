/*
 * Xenon (Xbox 360) fonts: Noto TrueType faces rasterised by stb_truetype.
 *
 * Faces load from res/fonts/ the first time a size of them is asked for.
 * Each glyph is rasterised once per face and pixel size into an 8-bit
 * coverage bitmap kept in a fixed arena; when the arena or its index fills,
 * the whole cache is dropped and refilled from what the next frames draw.
 * Advances of the first 256 code points are kept per face and size, so
 * layout, which measures far more text than it draws, rarely rasterises.
 *
 * Without any face the old 8x16 cell font stands in, so the browser still
 * works from a package that lost its fonts.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/browser.h"
#include "netsurf/plot_style.h"

#include "xenon/font.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "xenon/stb/stb_truetype.h"
#pragma GCC diagnostic pop

extern const unsigned char xenon_font8x16[4096];

enum {
	PX_MIN = 6,
	PX_MAX = 96,
	CACHE_SLOTS = 8192,       /* power of two */
	CACHE_LIMIT = CACHE_SLOTS * 3 / 4,
	ARENA_BYTES = 3 * 1024 * 1024,
	FALLBACK_W = 8,
	FALLBACK_H = 16,
	FALLBACK_ASCENT = 12,
};

static const char *const face_files[XENON_FACE_COUNT] = {
	"res/fonts/NotoSans-Regular.ttf",
	"res/fonts/NotoSans-Bold.ttf",
	"res/fonts/NotoSans-Italic.ttf",
	"res/fonts/NotoSans-BoldItalic.ttf",
	"res/fonts/NotoSerif-Regular.ttf",
	"res/fonts/NotoSerif-Bold.ttf",
	"res/fonts/NotoSansMono-Regular.ttf",
	"res/fonts/NotoSansMono-Bold.ttf",
};

/* What to use when a face's file is missing. */
static const xenon_face face_fallback[XENON_FACE_COUNT] = {
	XENON_FACE_SANS,
	XENON_FACE_SANS,
	XENON_FACE_SANS,
	XENON_FACE_SANS_BOLD,
	XENON_FACE_SANS,
	XENON_FACE_SANS_BOLD,
	XENON_FACE_SANS,
	XENON_FACE_MONO,
};

struct face {
	bool tried;
	bool ok;
	unsigned char *data;
	stbtt_fontinfo info;
	int ascent;  /* font units */
	int descent; /* font units, negative */
	float *advance[PX_MAX + 1]; /* 256 advances per size, made on demand */
};

struct glyph {
	uint64_t key; /* 0 = empty */
	int16_t x0;   /* bitmap offset from the pen and the baseline */
	int16_t y0;
	uint16_t w;
	uint16_t h;
	float advance;
	uint32_t offset; /* into the arena */
};

static struct face faces[XENON_FACE_COUNT];
static struct glyph *cache;
static unsigned cache_used;
static unsigned char *arena;
static uint32_t arena_used;
static unsigned cache_flushes;
static unsigned glyphs_made;
static int any_face = -1; /* -1 unknown, 0 none, 1 some */

/* Coverage to blend weight: a mild boost of thin stems, which reads better
 * on a TV than linear coverage. */
static uint8_t coverage_curve[256];

static void make_curve(void)
{
	for (int i = 0; i < 256; i++)
		coverage_curve[i] = (uint8_t)(255.0f * powf(i / 255.0f, 0.78f) + 0.5f);
}

static bool load_face(xenon_face index)
{
	struct face *face = &faces[index];
	FILE *fp;
	long size;

	if (face->tried)
		return face->ok;
	face->tried = true;

	fp = fopen(face_files[index], "rb");
	if (fp == NULL) {
		fprintf(stderr, "xenon: font %s missing\n", face_files[index]);
		return false;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0 || (face->data = malloc((size_t)size)) == NULL) {
		fclose(fp);
		return false;
	}
	if (fread(face->data, 1, (size_t)size, fp) != (size_t)size ||
	    !stbtt_InitFont(&face->info, face->data,
			    stbtt_GetFontOffsetForIndex(face->data, 0))) {
		fclose(fp);
		free(face->data);
		face->data = NULL;
		fprintf(stderr, "xenon: font %s unreadable\n", face_files[index]);
		return false;
	}
	fclose(fp);

	stbtt_GetFontVMetrics(&face->info, &face->ascent, &face->descent, NULL);
	face->ok = true;
	fprintf(stderr, "xenon: font %s loaded, %ld bytes\n", face_files[index], size);
	return true;
}

/* The face to draw with, after fallbacks; NULL means the cell font. */
static struct face *resolve(xenon_face *index)
{
	if (any_face < 0) {
		make_curve();
		any_face = load_face(XENON_FACE_SANS) ? 1 : 0;
	}
	if (any_face == 0)
		return NULL;
	if ((unsigned)*index >= XENON_FACE_COUNT)
		*index = XENON_FACE_SANS;
	if (!load_face(*index)) {
		*index = face_fallback[*index];
		if (!load_face(*index))
			*index = XENON_FACE_SANS;
	}
	return &faces[*index];
}

bool xenon_font_available(void)
{
	xenon_face sans = XENON_FACE_SANS;
	return resolve(&sans) != NULL;
}

static int clamp_px(int px)
{
	if (px < PX_MIN)
		return PX_MIN;
	if (px > PX_MAX)
		return PX_MAX;
	return px;
}

xenon_face xenon_font_face_for(const struct plot_font_style *fstyle)
{
	bool bold = fstyle->weight >= 600;
	bool italic = (fstyle->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0;

	switch (fstyle->family) {
	case PLOT_FONT_FAMILY_SERIF:
		return bold ? XENON_FACE_SERIF_BOLD : XENON_FACE_SERIF;
	case PLOT_FONT_FAMILY_MONOSPACE:
		return bold ? XENON_FACE_MONO_BOLD : XENON_FACE_MONO;
	default:
		if (bold)
			return italic ? XENON_FACE_SANS_BOLD_ITALIC : XENON_FACE_SANS_BOLD;
		return italic ? XENON_FACE_SANS_ITALIC : XENON_FACE_SANS;
	}
}

int xenon_font_px_for(const struct plot_font_style *fstyle)
{
	/* size is points in fixed point; a CSS pixel is 1/96 inch and NetSurf
	 * scales points by its screen dpi. */
	int dpi = browser_get_dpi();
	int px = (int)(((int64_t)fstyle->size * dpi + (72 * PLOT_STYLE_SCALE) / 2) /
		       (72 * PLOT_STYLE_SCALE));
	return clamp_px(px);
}

static size_t utf8_next(const char *s, size_t len, uint32_t *cp)
{
	const unsigned char c = (unsigned char)s[0];

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xf0 && len >= 4) {
		*cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3f) << 12) |
		      ((uint32_t)(s[2] & 0x3f) << 6) | (uint32_t)(s[3] & 0x3f);
		return 4;
	}
	if (c >= 0xe0 && len >= 3) {
		*cp = ((uint32_t)(c & 0x0f) << 12) | ((uint32_t)(s[1] & 0x3f) << 6) |
		      (uint32_t)(s[2] & 0x3f);
		return 3;
	}
	if (c >= 0xc0 && len >= 2) {
		*cp = ((uint32_t)(c & 0x1f) << 6) | (uint32_t)(s[1] & 0x3f);
		return 2;
	}
	*cp = 0xfffd;
	return 1;
}

/* Non-breaking space draws as a space; soft hyphens and zero-width marks
 * take no room. */
static bool invisible(uint32_t cp)
{
	return cp == 0x00ad || cp == 0x200b || cp == 0x200c || cp == 0x200d ||
	       cp == 0xfeff;
}

static float scale_of(struct face *face, int px)
{
	return stbtt_ScaleForMappingEmToPixels(&face->info, (float)px);
}

static int glyph_index(struct face *face, uint32_t cp)
{
	int index;

	if (cp == 0x00a0)
		cp = ' ';
	index = stbtt_FindGlyphIndex(&face->info, (int)cp);
	if (index == 0 && cp >= 0x2010 && cp <= 0x2015)
		index = stbtt_FindGlyphIndex(&face->info, '-');
	return index;
}

static float advance_uncached(struct face *face, int px, uint32_t cp)
{
	int adv;
	int lsb;

	if (invisible(cp))
		return 0.0f;
	stbtt_GetGlyphHMetrics(&face->info, glyph_index(face, cp), &adv, &lsb);
	return (float)adv * scale_of(face, px);
}

static float advance_of(struct face *face, int px, uint32_t cp)
{
	float *table;

	if (cp >= 256)
		return advance_uncached(face, px, cp);
	table = face->advance[px];
	if (table == NULL) {
		table = malloc(256 * sizeof(float));
		if (table == NULL)
			return advance_uncached(face, px, cp);
		for (int i = 0; i < 256; i++)
			table[i] = advance_uncached(face, px, (uint32_t)i);
		face->advance[px] = table;
	}
	return table[cp];
}

void xenon_font_flush(void)
{
	if (cache != NULL)
		memset(cache, 0, CACHE_SLOTS * sizeof(*cache));
	cache_used = 0;
	arena_used = 0;
	cache_flushes++;
}

static bool cache_ready(void)
{
	if (cache != NULL)
		return true;
	cache = calloc(CACHE_SLOTS, sizeof(*cache));
	arena = malloc(ARENA_BYTES);
	if (cache == NULL || arena == NULL) {
		free(cache);
		free(arena);
		cache = NULL;
		arena = NULL;
		return false;
	}
	return true;
}

static const struct glyph *glyph_get(xenon_face index, struct face *face, int px,
				     uint32_t cp)
{
	uint64_t key = ((uint64_t)(index + 1) << 40) | ((uint64_t)px << 24) | cp;
	uint32_t hash = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 40);
	struct glyph *slot;
	float scale;
	int gi, x0, y0, x1, y1, w, h;

	if (!cache_ready())
		return NULL;

	for (;;) {
		slot = &cache[hash & (CACHE_SLOTS - 1)];
		if (slot->key == key)
			return slot;
		if (slot->key == 0)
			break;
		hash++;
	}

	scale = scale_of(face, px);
	gi = glyph_index(face, cp);
	stbtt_GetGlyphBitmapBox(&face->info, gi, scale, scale, &x0, &y0, &x1, &y1);
	w = x1 - x0;
	h = y1 - y0;
	if (w < 0 || h < 0 || w > 4 * PX_MAX || h > 4 * PX_MAX)
		w = h = 0;
	if (invisible(cp))
		w = h = 0;

	if (cache_used >= CACHE_LIMIT || arena_used + (uint32_t)(w * h) > ARENA_BYTES) {
		xenon_font_flush();
		return glyph_get(index, face, px, cp);
	}

	slot->key = key;
	slot->x0 = (int16_t)x0;
	slot->y0 = (int16_t)y0;
	slot->w = (uint16_t)w;
	slot->h = (uint16_t)h;
	slot->advance = advance_of(face, px, cp);
	slot->offset = arena_used;
	if (w > 0 && h > 0) {
		stbtt_MakeGlyphBitmap(&face->info, arena + arena_used, w, h, w,
				      scale, scale, gi);
		arena_used += (uint32_t)(w * h);
	}
	cache_used++;
	glyphs_made++;
	return slot;
}

int xenon_font_width(xenon_face index, int px, const char *text, size_t length)
{
	struct face *face = resolve(&index);
	float pen = 0.0f;
	size_t offset = 0;

	px = clamp_px(px);
	if (face == NULL)
		return FALLBACK_W * (int)length;

	while (offset < length) {
		uint32_t cp;
		offset += utf8_next(text + offset, length - offset, &cp);
		pen += advance_of(face, px, cp);
	}
	return (int)(pen + 0.5f);
}

size_t xenon_font_offset_at(xenon_face index, int px, const char *text,
			    size_t length, int x, bool round, int *edge_x)
{
	struct face *face = resolve(&index);
	float pen = 0.0f;
	size_t offset = 0;

	px = clamp_px(px);
	while (offset < length) {
		uint32_t cp;
		size_t step = utf8_next(text + offset, length - offset, &cp);
		float advance = face != NULL ? advance_of(face, px, cp) : FALLBACK_W;
		float limit = round ? pen + advance / 2.0f : pen + advance;

		if ((float)x < limit)
			break;
		pen += advance;
		offset += step;
	}
	*edge_x = (int)(pen + 0.5f);
	return offset;
}

void xenon_font_metrics(xenon_face index, int px, int *ascent, int *descent)
{
	struct face *face = resolve(&index);

	px = clamp_px(px);
	if (face == NULL) {
		*ascent = FALLBACK_ASCENT;
		*descent = FALLBACK_H - FALLBACK_ASCENT;
		return;
	}
	*ascent = (int)ceilf(face->ascent * scale_of(face, px));
	*descent = (int)ceilf(-face->descent * scale_of(face, px));
}

static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
	uint32_t rb = ((src & 0xff00ff) * a + (dst & 0xff00ff) * (255 - a) + 0x800080);
	uint32_t g = ((src & 0x00ff00) * a + (dst & 0x00ff00) * (255 - a) + 0x008000);

	rb = ((rb + ((rb >> 8) & 0xff00ff)) >> 8) & 0xff00ff;
	g = ((g + ((g >> 8) & 0x00ff00)) >> 8) & 0x00ff00;
	return rb | g;
}

static void draw_fallback_glyph(const struct xenon_surface *s, int x, int ytop,
				uint32_t cp, uint32_t rgb)
{
	const unsigned char *glyph;

	if (cp > 0x7e || cp < 0x20)
		cp = cp == 0x00a0 ? ' ' : '?';
	glyph = &xenon_font8x16[cp * FALLBACK_H];
	for (int row = 0; row < FALLBACK_H; row++) {
		int y = ytop + row;
		if (glyph[row] == 0 || y < s->clip_y0 || y >= s->clip_y1)
			continue;
		for (int col = 0; col < FALLBACK_W; col++) {
			int px = x + col;
			if ((glyph[row] & (0x80 >> col)) && px >= s->clip_x0 &&
			    px < s->clip_x1)
				s->pixels[y * s->stride + px] = rgb;
		}
	}
}

int xenon_font_draw(const struct xenon_surface *s, xenon_face index, int px,
		    int x, int y, const char *text, size_t length, uint32_t rgb)
{
	struct face *face = resolve(&index);
	float pen = (float)x;
	size_t offset = 0;

	px = clamp_px(px);
	while (offset < length) {
		const struct glyph *g;
		uint32_t cp;
		int gx, gy, cx0, cy0, cx1, cy1;

		offset += utf8_next(text + offset, length - offset, &cp);

		if (face == NULL) {
			draw_fallback_glyph(s, (int)pen, y - FALLBACK_ASCENT, cp, rgb);
			pen += FALLBACK_W;
			continue;
		}

		g = glyph_get(index, face, px, cp);
		if (g == NULL) {
			pen += advance_of(face, px, cp);
			continue;
		}

		gx = (int)(pen + 0.5f) + g->x0;
		gy = y + g->y0;
		pen += g->advance;
		if (g->w == 0 || gx >= s->clip_x1 || gy >= s->clip_y1 ||
		    gx + g->w <= s->clip_x0 || gy + g->h <= s->clip_y0)
			continue;

		cx0 = gx < s->clip_x0 ? s->clip_x0 - gx : 0;
		cy0 = gy < s->clip_y0 ? s->clip_y0 - gy : 0;
		cx1 = gx + g->w > s->clip_x1 ? s->clip_x1 - gx : g->w;
		cy1 = gy + g->h > s->clip_y1 ? s->clip_y1 - gy : g->h;

		for (int row = cy0; row < cy1; row++) {
			const unsigned char *cov = arena + g->offset + (size_t)row * g->w;
			uint32_t *dst = s->pixels + (size_t)(gy + row) * s->stride + gx;
			for (int col = cx0; col < cx1; col++) {
				unsigned a = cov[col];
				if (a == 0)
					continue;
				a = coverage_curve[a];
				dst[col] = a == 255 ? rgb : blend(dst[col], rgb, a);
			}
		}
	}
	return (int)(pen + 0.5f);
}

void xenon_font_stats(void)
{
	fprintf(stderr, "xenon: font cache %u glyphs, %u kB of %u kB, %u made, "
		"%u flushes\n", cache_used, (unsigned)(arena_used / 1024),
		(unsigned)(ARENA_BYTES / 1024), glyphs_made, cache_flushes);
}
