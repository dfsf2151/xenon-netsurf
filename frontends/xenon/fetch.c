/*
 * Copyright 2014 Vincent Sanders <vince@netsurf-browser.org>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

#include "utils/errors.h"
#include "utils/file.h"
#include "utils/nsurl.h"
#include "utils/filepath.h"
#include "netsurf/fetch.h"

#include "xenon/filetype.h"
#include "xenon/fetch.h"

extern char **respaths;


#define XENON_RESOURCE_PREFIX "file:///game:/res/"

static nsurl *gui_get_resource_url(const char *path)
{
	char buf[PATH_MAX];
	nsurl *url = NULL;
	int written;

	written = snprintf(buf, sizeof(buf), "%s%s", XENON_RESOURCE_PREFIX, path);
	if (written < 0 || (size_t)written >= sizeof(buf))
		return NULL;

	if (nsurl_create(buf, &url) != NSERROR_OK)
		return NULL;

	return url;
}

static struct gui_fetch_table fetch_table = {
	.filetype = xenon_fetch_filetype,

	.get_resource_url = gui_get_resource_url,
};

struct gui_fetch_table *xenon_fetch_table = &fetch_table;
