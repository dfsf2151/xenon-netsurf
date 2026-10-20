/*
 * Copyright 2015 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef NS_XENON_BITMAP_H
#define NS_XENON_BITMAP_H

#include <stdbool.h>
#include <stddef.h>

extern struct gui_bitmap_table *xenon_bitmap_table;

/* Shared with the plotter, which reads the pixels back. Bytes are in the
 * core's default layout: R, G, B, A. */
struct bitmap {
	void *ptr;
	size_t rowstride;
	int width;
	int height;
	bool opaque;
};

#endif /* NS_XENON_BITMAP_H */
