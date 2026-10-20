/*
 * Copyright 2011 Daniel Silverstone <dsilvers@digital-scurf.org>
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

#ifndef NETSURF_XENON_BROWSER_H
#define NETSURF_XENON_BROWSER_H

#include <stdbool.h>

struct hlcache_handle;

extern struct gui_window_table *xenon_window_table;
extern struct gui_download_table *xenon_download_table;

struct gui_window {
	struct gui_window *r_next;
	struct gui_window *r_prev;
  
	uint32_t win_num;
	struct browser_window *bw;
  
	int width, height;
	int scrollx, scrolly;

	char title[160];
	char url[1024];
	bool loading;

	char *host;  /* Ignore this, it's in case RING*() gets debugging for fetchers */
  
};

/* Tab strip, in creation order. */
int xenon_tab_count(void);
struct gui_window *xenon_tab_at(int index);
struct gui_window *xenon_tab_active(void);
void xenon_tab_activate(int index);
void xenon_tab_close(int index);
void xenon_tab_new(void);
bool xenon_tab_is_home(int index);
const char *xenon_tab_title(int index);
const char *xenon_tab_url(int index);

/* Scroll offset of the active page, for the redraw call. */
void xenon_get_active_scroll(int *sx, int *sy);
bool xenon_is_loading(void);

struct gui_window *xenon_find_window_by_num(uint32_t win_num);
struct gui_window *xenon_find_window_by_content(struct hlcache_handle *content);
void xenon_window_process_reformats(void);

void xenon_window_handle_command(int argc, char **argv);
void xenon_kill_browser_windows(void);

nserror xenon_warn_user(const char *warning, const char *detail);

#endif /* NETSURF_XENON_BROWSER_H */
