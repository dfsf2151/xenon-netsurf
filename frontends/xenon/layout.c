/*
 * Xenon (Xbox 360) font layout: fixed 8x16 cell font.
 *
 * The plotter draws every glyph in an 8x16 cell, so the metrics here must
 * agree exactly. UTF-8 aware: character offsets are byte offsets.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <stddef.h>
#include <stdint.h>

#include "utils/utf8.h"
#include "netsurf/plot_style.h"
#include "netsurf/layout.h"

#include "xenon/layout.h"

#define CELL_WIDTH 8

static size_t byte_offset_of_char(const char *string, size_t length, size_t char_index)
{
	size_t offset = 0;
	size_t chars = 0;

	while (offset < length && chars < char_index) {
		const unsigned char c = (unsigned char)string[offset];
		size_t step = 1;
		if (c >= 0xf0)
			step = 4;
		else if (c >= 0xe0)
			step = 3;
		else if (c >= 0xc0)
			step = 2;
		if (offset + step > length)
			step = 1;
		offset += step;
		chars++;
	}
	return offset;
}

static nserror xenon_font_width(const plot_font_style_t *fstyle,
				const char *string, size_t length, int *width)
{
	(void)fstyle;
	*width = CELL_WIDTH * (int)utf8_bounded_length(string, length);
	return NSERROR_OK;
}

static nserror xenon_font_position_in_string(const plot_font_style_t *fstyle,
					     const char *string, size_t length,
					     int x, size_t *char_offset, int *actual_x)
{
	(void)fstyle;
	size_t index;

	if (x <= 0) {
		index = 0;
	} else {
		index = (size_t)(x / CELL_WIDTH);
		if (index > utf8_bounded_length(string, length))
			index = utf8_bounded_length(string, length);
	}

	*char_offset = byte_offset_of_char(string, length, index);
	*actual_x = (int)index * CELL_WIDTH;
	return NSERROR_OK;
}

static nserror xenon_font_split(const plot_font_style_t *fstyle,
				const char *string, size_t length,
				int x, size_t *char_offset, int *actual_x)
{
	(void)fstyle;
	size_t index = (size_t)(x / CELL_WIDTH);
	size_t chars = utf8_bounded_length(string, length);

	if (index > chars) {
		index = chars;
	} else {
		size_t offset = byte_offset_of_char(string, length, index);
		while (index > 0) {
			size_t prev = byte_offset_of_char(string, length, index - 1);
			if (string[prev] == ' ' && offset == prev + 1) {
				break;
			}
			/* Walk back to a space character. */
			if (string[prev] == ' ')
				break;
			index--;
			offset = prev;
		}
		if (index == 0) {
			index = (size_t)(x / CELL_WIDTH);
			if (index > chars)
				index = chars;
			while (index < chars) {
				size_t offset2 = byte_offset_of_char(string, length, index);
				if (string[offset2] == ' ')
					break;
				index++;
			}
		}
	}

	*char_offset = byte_offset_of_char(string, length, index);
	*actual_x = (int)index * CELL_WIDTH;
	return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
	.width = xenon_font_width,
	.position = xenon_font_position_in_string,
	.split = xenon_font_split,
};

struct gui_layout_table *xenon_layout_table = &layout_table;
