/*
 * Xenon (Xbox 360) font layout on the proportional faces in font.c.
 *
 * Measuring and drawing share the same advances and rounding, so text
 * lands where layout put it.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"
#include "netsurf/plot_style.h"
#include "netsurf/layout.h"

#include "xenon/font.h"
#include "xenon/layout.h"

static nserror xenon_font_width_cb(const plot_font_style_t *fstyle,
				   const char *string, size_t length, int *width)
{
	*width = xenon_font_width(xenon_font_face_for(fstyle),
				  xenon_font_px_for(fstyle), string, length);
	return NSERROR_OK;
}

static nserror xenon_font_position_in_string(const plot_font_style_t *fstyle,
					     const char *string, size_t length,
					     int x, size_t *char_offset, int *actual_x)
{
	*char_offset = xenon_font_offset_at(xenon_font_face_for(fstyle),
					    xenon_font_px_for(fstyle), string,
					    length, x, true, actual_x);
	return NSERROR_OK;
}

/* Where to break a line at width x: at the last space that fits, else the
 * first space after it, else nowhere. */
static nserror xenon_font_split(const plot_font_style_t *fstyle,
				const char *string, size_t length,
				int x, size_t *char_offset, int *actual_x)
{
	xenon_face face = xenon_font_face_for(fstyle);
	int px = xenon_font_px_for(fstyle);
	int edge;
	size_t fit = xenon_font_offset_at(face, px, string, length, x, false, &edge);
	size_t split;

	if (fit >= length) {
		*char_offset = length;
		*actual_x = edge;
		return NSERROR_OK;
	}

	split = fit;
	while (split > 0 && string[split] != ' ')
		split--;
	if (split == 0) {
		split = fit;
		while (split < length && string[split] != ' ')
			split++;
	}

	*char_offset = split;
	*actual_x = xenon_font_width(face, px, string, split);
	return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
	.width = xenon_font_width_cb,
	.position = xenon_font_position_in_string,
	.split = xenon_font_split,
};

struct gui_layout_table *xenon_layout_table = &layout_table;
