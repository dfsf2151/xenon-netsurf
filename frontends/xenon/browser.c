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

/* Browser-related callbacks */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/utils.h"
#include "utils/ring.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "netsurf/mouse.h"
#include "netsurf/window.h"
#include "netsurf/browser_window.h"
#include "netsurf/plotters.h"

#include "xenon/output.h"
#include "xenon/browser.h"
#include "xenon/plot.h"
#include "xenon/ui.h"
#include "xenon/xenon.h"

struct browser_window *xenon_bw;
volatile bool xenon_needs_redraw;

static uint32_t win_ctr = 0;

static struct gui_window *gw_ring = NULL;

#define MAX_TABS 16
static struct gui_window *tabs[MAX_TABS];
static int tab_count;

static void tab_add(struct gui_window *g)
{
	if (tab_count < MAX_TABS)
		tabs[tab_count++] = g;
}

static void tab_remove(struct gui_window *g)
{
	for (int i = 0; i < tab_count; i++) {
		if (tabs[i] != g)
			continue;
		for (int j = i; j + 1 < tab_count; j++)
			tabs[j] = tabs[j + 1];
		tab_count--;
		return;
	}
}

int xenon_tab_count(void)
{
	return tab_count;
}

struct gui_window *xenon_tab_at(int index)
{
	if (index < 0 || index >= tab_count)
		return NULL;
	return tabs[index];
}

struct gui_window *xenon_tab_active(void)
{
	for (int i = 0; i < tab_count; i++)
		if (tabs[i]->bw == xenon_bw)
			return tabs[i];
	return NULL;
}

const char *xenon_tab_title(int index)
{
	struct gui_window *g = xenon_tab_at(index);
	if (g == NULL || g->title[0] == '\0')
		return "New Tab";
	return g->title;
}

const char *xenon_tab_url(int index)
{
	struct gui_window *g = xenon_tab_at(index);
	return g != NULL ? g->url : "";
}

bool xenon_tab_is_home(int index)
{
	struct gui_window *g = xenon_tab_at(index);
	if (g == NULL || g->url[0] == '\0')
		return true;
	return strstr(g->url, "/home.html") != NULL;
}

void xenon_tab_activate(int index)
{
	struct gui_window *g = xenon_tab_at(index);
	if (g == NULL || g->bw == xenon_bw)
		return;
	xenon_bw = g->bw;
	xenon_damage_all();
	xenon_ui_chrome_changed();
}

void xenon_tab_close(int index)
{
	struct gui_window *g = xenon_tab_at(index);
	if (g != NULL)
		browser_window_destroy(g->bw);
}

static void go_home_tab(void)
{
	nsurl *url = NULL;

	if (nsurl_create("file:///game:/home.html", &url) != NSERROR_OK)
		return;
	if (browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, NULL) !=
	    NSERROR_OK)
		NSLOG(netsurf, ERROR, "tab: cannot create home tab");
	nsurl_unref(url);
}

void xenon_tab_new(void)
{
	go_home_tab();
}

void xenon_get_active_scroll(int *sx, int *sy)
{
	struct gui_window *g = xenon_tab_active();
	*sx = g != NULL ? g->scrollx : 0;
	*sy = g != NULL ? g->scrolly : 0;
}

bool xenon_is_loading(void)
{
	struct gui_window *g = xenon_tab_active();
	return g != NULL && g->loading;
}

/* exported function documented in xenon/browser.h */
nserror xenon_warn_user(const char *warning, const char *detail)
{
	moutf(MOUT_WARNING, "%s %s", warning, detail);
	return NSERROR_OK;
}

struct gui_window *
xenon_find_window_by_num(uint32_t win_num)
{
	struct gui_window *ret = NULL;

	RING_ITERATE_START(struct gui_window, gw_ring, c_ring) {
		if (c_ring->win_num == win_num) {
			ret = c_ring;
			RING_ITERATE_STOP(gw_ring, c_ring);
		}
	} RING_ITERATE_END(gw_ring, c_ring);

	return ret;
}

void
xenon_kill_browser_windows(void)
{
	while (gw_ring != NULL) {
		browser_window_destroy(gw_ring->bw);
	}
}

static struct gui_window *
gui_window_create(struct browser_window *bw,
		  struct gui_window *existing,
		  gui_window_create_flags flags)
{
	struct gui_window *ret = calloc(1, sizeof(*ret));
	if (ret == NULL)
		return NULL;

	ret->win_num = win_ctr++;
	ret->bw = bw;
	xenon_bw = bw;

	ret->width = XENON_WIDTH;
	ret->height = XENON_PAGE_HEIGHT;
	snprintf(ret->title, sizeof(ret->title), "New Tab");
	tab_add(ret);
	xenon_ui_chrome_changed();

	moutf(MOUT_WINDOW,
	      "NEW WIN %u FOR %p EXISTING %p NEWTAB %s CLONE %s",
	      ret->win_num, bw, existing,
	      flags & GW_CREATE_TAB ? "TRUE" : "FALSE",
	      flags & GW_CREATE_CLONE ? "TRUE" : "FALSE");
	moutf(MOUT_WINDOW,
	      "SIZE WIN %u WIDTH %d HEIGHT %d",
	      ret->win_num, ret->width, ret->height);

	RING_INSERT(gw_ring, ret);

	return ret;
}

static void
gui_window_destroy(struct gui_window *g)
{
	bool was_active = (g->bw == xenon_bw);

	moutf(MOUT_WINDOW, "DESTROY WIN %u", g->win_num);
	tab_remove(g);
	RING_REMOVE(gw_ring, g);

	if (was_active) {
		xenon_bw = tab_count > 0 ? tabs[tab_count - 1]->bw : NULL;
		xenon_damage_all();
	}
	xenon_ui_chrome_changed();
	free(g);
}

static void
gui_window_set_title(struct gui_window *g, const char *title)
{
	snprintf(g->title, sizeof(g->title), "%s", title != NULL ? title : "");
	moutf(MOUT_WINDOW, "TITLE WIN %u STR %s", g->win_num, g->title);
	xenon_ui_chrome_changed();
}

/**
 * Find the current dimensions of a xenon browser window content area.
 *
 * \param g The gui window to measure content area of.
 * \param width receives width of window
 * \param height receives height of window
 * \return NSERROR_OK on sucess and width and height updated.
 */
static nserror
gui_window_get_dimensions(struct gui_window *g, int *width, int *height)
{
	*width = g->width;
	*height = g->height;

	moutf(MOUT_WINDOW,
	      "GET_DIMENSIONS WIN %u WIDTH %d HEIGHT %d",
	      g->win_num, *width, *height);

	return NSERROR_OK;
}

static void
gui_window_new_content(struct gui_window *g)
{
	moutf(MOUT_WINDOW, "NEW_CONTENT WIN %u", g->win_num);
}

static void
gui_window_set_icon(struct gui_window *g, struct hlcache_handle *icon)
{
	moutf(MOUT_WINDOW, "NEW_ICON WIN %u", g->win_num);
}

static void
gui_window_start_throbber(struct gui_window *g)
{
	g->loading = true;
	moutf(MOUT_WINDOW, "START_THROBBER WIN %u", g->win_num);
	xenon_ui_chrome_changed();
}

static void
gui_window_stop_throbber(struct gui_window *g)
{
	g->loading = false;
	moutf(MOUT_WINDOW, "STOP_THROBBER WIN %u", g->win_num);
	xenon_ui_chrome_changed();
}


/**
 * Set the scroll position of a xenon browser window.
 *
 * Scrolls the viewport to ensure the specified rectangle of the
 *   content is shown.
 *
 * \param gw gui window to scroll
 * \param rect The rectangle to ensure is shown.
 * \return NSERROR_OK on success or apropriate error code.
 */
static nserror
gui_window_set_scroll(struct gui_window *gw, const struct rect *rect)
{
	gw->scrollx = rect->x0;
	gw->scrolly = rect->y0;
	if (gw->bw == xenon_bw)
		xenon_needs_redraw = true;

	moutf(MOUT_WINDOW, "SET_SCROLL WIN %u X %d Y %d",
		gw->win_num, rect->x0, rect->y0);
	return NSERROR_OK;
}


/**
 * Invalidates an area of a xenon browser window
 *
 * \param gw gui_window
 * \param rect area to redraw or NULL for the entire window area
 * \return NSERROR_OK on success or appropriate error code
 */
static nserror
xenon_window_invalidate_area(struct gui_window *gw, const struct rect *rect)
{
	if (rect != NULL) {
		moutf(MOUT_WINDOW,
		      "INVALIDATE_AREA WIN %u X %d Y %d WIDTH %d HEIGHT %d",
		      gw->win_num,
		      rect->x0, rect->y0,
		      (rect->x1 - rect->x0), (rect->y1 - rect->y0));
	} else {
		moutf(MOUT_WINDOW, "INVALIDATE_AREA WIN %u ALL", gw->win_num);
	}

	if (gw->bw == xenon_bw) {
		if (rect != NULL)
			xenon_damage_rect(rect);
		else
			xenon_damage_all();
	}

	return NSERROR_OK;
}

static void
gui_window_update_extent(struct gui_window *g)
{
	int width, height;

	if (browser_window_get_extents(g->bw, false, &width, &height) != NSERROR_OK)
		return;

	moutf(MOUT_WINDOW, "UPDATE_EXTENT WIN %u WIDTH %d HEIGHT %d",
	      g->win_num, width, height);
}

static void
gui_window_set_status(struct gui_window *g, const char *text)
{
	if (g != NULL && g->bw == xenon_bw)
		xenon_ui_status(text);
}

static void
gui_window_set_pointer(struct gui_window *g, gui_pointer_shape shape)
{
	const char *ptr_name = "UNKNOWN";

	switch (shape) {
	case GUI_POINTER_POINT:
		ptr_name = "POINT";
		break;
	case GUI_POINTER_CARET:
		ptr_name = "CARET";
		break;
	case GUI_POINTER_UP:
		ptr_name = "UP";
		break;
	case GUI_POINTER_DOWN:
		ptr_name = "DOWN";
		break;
	case GUI_POINTER_LEFT:
		ptr_name = "LEFT";
		break;
	case GUI_POINTER_RIGHT:
		ptr_name = "RIGHT";
		break;
	case GUI_POINTER_LD:
		ptr_name = "LD";
		break;
	case GUI_POINTER_RD:
		ptr_name = "RD";
		break;
	case GUI_POINTER_LU:
		ptr_name = "LU";
		break;
	case GUI_POINTER_RU:
		ptr_name = "RU";
		break;
	case GUI_POINTER_CROSS:
		ptr_name = "CROSS";
		break;
	case GUI_POINTER_MOVE:
		ptr_name = "MOVE";
		break;
	case GUI_POINTER_WAIT:
		ptr_name = "WAIT";
		break;
	case GUI_POINTER_HELP:
		ptr_name = "HELP";
		break;
	case GUI_POINTER_MENU:
		ptr_name = "MENU";
		break;
	case GUI_POINTER_PROGRESS:
		ptr_name = "PROGRESS";
		break;
	case GUI_POINTER_NO_DROP:
		ptr_name = "NO_DROP";
		break;
	case GUI_POINTER_NOT_ALLOWED:
		ptr_name = "NOT_ALLOWED";
		break;
	case GUI_POINTER_DEFAULT:
		ptr_name = "DEFAULT";
		break;
	default:
		break;
	}

	moutf(MOUT_WINDOW, "SET_POINTER WIN %u POINTER %s",
	      g->win_num, ptr_name);
}

static nserror
gui_window_set_url(struct gui_window *g, nsurl *url)
{
	snprintf(g->url, sizeof(g->url), "%s", nsurl_access(url));
	moutf(MOUT_WINDOW, "SET_URL WIN %u URL %s",
	      g->win_num, g->url);
	xenon_ui_chrome_changed();
	return NSERROR_OK;
}

static bool
gui_window_get_scroll(struct gui_window *g, int *sx, int *sy)
{
	moutf(MOUT_WINDOW, "GET_SCROLL WIN %u X %d Y %d",
	      g->win_num, g->scrollx, g->scrolly);
	*sx = g->scrollx;
	*sy = g->scrolly;
	return true;
}

static bool
gui_window_scroll_start(struct gui_window *g)
{
	moutf(MOUT_WINDOW, "SCROLL_START WIN %u", g->win_num);
	g->scrollx = g->scrolly = 0;
	return true;
}


static void
gui_window_place_caret(struct gui_window *g, int x, int y, int height,
		       const struct rect *clip)
{
	moutf(MOUT_WINDOW, "PLACE_CARET WIN %u X %d Y %d HEIGHT %d",
	      g->win_num, x, y, height);
	if (g->bw == xenon_bw)
		xenon_ui_caret(true);
}

static void
gui_window_remove_caret(struct gui_window *g)
{
	moutf(MOUT_WINDOW, "REMOVE_CARET WIN %u", g->win_num);
	if (g->bw == xenon_bw)
		xenon_ui_caret(false);
}

static bool
gui_window_drag_start(struct gui_window *g, gui_drag_type type,
		      const struct rect *rect)
{
	moutf(MOUT_WINDOW, "SCROLL_START WIN %u TYPE %i", g->win_num, type);
	return false;
}

static nserror
gui_window_save_link(struct gui_window *g, nsurl *url, const char *title)
{
	moutf(MOUT_WINDOW, "SAVE_LINK WIN %u URL %s TITLE %s",
		g->win_num, nsurl_access(url), title);
	return NSERROR_OK;
}

static void
gui_window_console_log(struct gui_window *g,
		       browser_window_console_source src,
		       const char *msg,
		       size_t msglen,
		       browser_window_console_flags flags)
{
	bool foldable = !!(flags & BW_CS_FLAG_FOLDABLE);
	const char *src_text;
	const char *level_text;

	switch (src) {
	case BW_CS_INPUT:
		src_text = "client-input";
		break;
	case BW_CS_SCRIPT_ERROR:
		src_text = "scripting-error";
		break;
	case BW_CS_SCRIPT_CONSOLE:
		src_text = "scripting-console";
		break;
	default:
		assert(0 && "Unknown scripting source");
		src_text = "unknown";
		break;
	}

	switch (flags & BW_CS_FLAG_LEVEL_MASK) {
	case BW_CS_FLAG_LEVEL_DEBUG:
		level_text = "DEBUG";
		break;
	case BW_CS_FLAG_LEVEL_LOG:
		level_text = "LOG";
		break;
	case BW_CS_FLAG_LEVEL_INFO:
		level_text = "INFO";
		break;
	case BW_CS_FLAG_LEVEL_WARN:
		level_text = "WARN";
		break;
	case BW_CS_FLAG_LEVEL_ERROR:
		level_text = "ERROR";
		break;
	default:
		assert(0 && "Unknown console logging level");
		level_text = "unknown";
		break;
	}

	moutf(MOUT_WINDOW, "CONSOLE_LOG WIN %u SOURCE %s %sFOLDABLE %s %.*s",
	      g->win_num, src_text, foldable ? "" : "NOT-", level_text,
	      (int)msglen, msg);
}

static void
gui_window_report_page_info(struct gui_window *g)
{
	const char *state = "***WAH***";

	switch (browser_window_get_page_info_state(g->bw)) {
	case PAGE_STATE_UNKNOWN:
		state = "UNKNOWN";
		break;

	case PAGE_STATE_INTERNAL:
		state = "INTERNAL";
		break;

	case PAGE_STATE_LOCAL:
		state = "LOCAL";
		break;

	case PAGE_STATE_INSECURE:
		state = "INSECURE";
		break;

	case PAGE_STATE_SECURE_OVERRIDE:
		state = "SECURE_OVERRIDE";
		break;

	case PAGE_STATE_SECURE_ISSUES:
		state = "SECURE_ISSUES";
		break;

	case PAGE_STATE_SECURE:
		state = "SECURE";
		break;

	default:
		assert(0 && "Xenon needs some lovin' here");
		break;
	}
	moutf(MOUT_WINDOW, "PAGE_STATUS WIN %u STATUS %s",
	      g->win_num, state);
}

/**** Handlers ****/

static void
xenon_window_handle_new(int argc, char **argv)
{
	nsurl *url = NULL;
	nserror error = NSERROR_OK;

	if (argc > 3)
		return;

	if (argc == 3) {
		error = nsurl_create(argv[2], &url);
	}
	if (error == NSERROR_OK) {
		error = browser_window_create(BW_CREATE_HISTORY,
					      url,
					      NULL,
					      NULL,
					      NULL);
		if (url != NULL) {
			nsurl_unref(url);
		}
	}
	if (error != NSERROR_OK) {
		xenon_warn_user(messages_get_errorcode(error), 0);
	}
}

static void
xenon_window_handle_destroy(int argc, char **argv)
{
	struct gui_window *gw;
	uint32_t nr = atoi((argc > 2) ? argv[2] : "-1");

	gw = xenon_find_window_by_num(nr);

	if (gw == NULL) {
		moutf(MOUT_ERROR, "WINDOW NUM BAD");
	} else {
		browser_window_destroy(gw->bw);
	}
}

static void
xenon_window_handle_go(int argc, char **argv)
{
	struct gui_window *gw;
	nsurl *url;
	nsurl *ref_url = NULL;
	nserror error;

	if (argc < 4 || argc > 5) {
		moutf(MOUT_ERROR, "WINDOW GO ARGS BAD");
		return;
	}

	gw = xenon_find_window_by_num(atoi(argv[2]));

	if (gw == NULL) {
		moutf(MOUT_ERROR, "WINDOW NUM BAD");
		return;
	}

	error = nsurl_create(argv[3], &url);
	if (error == NSERROR_OK) {
		if (argc == 5) {
			error = nsurl_create(argv[4], &ref_url);
		}

		if (error == NSERROR_OK) {
			error = browser_window_navigate(gw->bw,
							url,
							ref_url,
							BW_NAVIGATE_HISTORY,
							NULL,
							NULL,
							NULL);
			if (ref_url != NULL) {
				nsurl_unref(ref_url);
			}
		}
		nsurl_unref(url);
	}

	if (error != NSERROR_OK) {
		xenon_warn_user(messages_get_errorcode(error), 0);
	}
}

/**
 * handle WINDOW STOP command
 */
static void
xenon_window_handle_stop(int argc, char **argv)
{
	struct gui_window *gw;
	if (argc != 3) {
		moutf(MOUT_ERROR, "WINDOW STOP ARGS BAD\n");
		return;
	}

	gw = xenon_find_window_by_num(atoi(argv[2]));

	if (gw == NULL) {
		moutf(MOUT_ERROR, "WINDOW NUM BAD");
	} else {
		browser_window_stop(gw->bw);
	}
}


static void
xenon_window_handle_redraw(int argc, char **argv)
{
	struct gui_window *gw;
	struct rect clip;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = xenon_plotters
	};

	if (argc != 3 && argc != 7) {
		moutf(MOUT_ERROR, "WINDOW REDRAW ARGS BAD");
		return;
	}

	gw = xenon_find_window_by_num(atoi(argv[2]));

	if (gw == NULL) {
		moutf(MOUT_ERROR, "WINDOW NUM BAD");
		return;
	}

	clip.x0 = 0;
	clip.y0 = 0;
	clip.x1 = gw->width;
	clip.y1 = gw->height;

	if (argc == 7) {
		clip.x0 = atoi(argv[3]);
		clip.y0 = atoi(argv[4]);
		clip.x1 = atoi(argv[5]);
		clip.y1 = atoi(argv[6]);
	}

	NSLOG(netsurf, INFO, "Issue redraw");
	moutf(MOUT_WINDOW, "REDRAW WIN %d START", atoi(argv[2]));
	browser_window_redraw(gw->bw, gw->scrollx, gw->scrolly, &clip, &ctx);
	moutf(MOUT_WINDOW, "REDRAW WIN %d STOP", atoi(argv[2]));
}

static void
xenon_window_handle_reload(int argc, char **argv)
{
	struct gui_window *gw;
	if (argc != 3 && argc != 4) {
		moutf(MOUT_ERROR, "WINDOW RELOAD ARGS BAD\n");
		return;
	}

	gw = xenon_find_window_by_num(atoi(argv[2]));

	if (gw == NULL) {
		moutf(MOUT_ERROR, "WINDOW NUM BAD");
	} else {
		browser_window_reload(gw->bw, argc == 4);
	}
}

static void
xenon_window_handle_exec(int argc, char **argv)
{
	struct gui_window *gw;
	if (argc < 5) {
		moutf(MOUT_ERROR, "WINDOW EXEC ARGS BAD\n");
	}

	gw = xenon_find_window_by_num(atoi(argv[2]));

	if (gw == NULL) {
		moutf(MOUT_ERROR, "WINDOW NUM BAD");
	} else {
		/* Gather argv[4] onward into a string to pass to js_exec */
		int total = 1;
		for (int i = 4; i < argc; ++i) {
			total += strlen(argv[i]) + 1;
		}
		char *cmd = calloc(total, 1);
		if (cmd == NULL) {
			moutf(MOUT_ERROR, "JS WIN %d RET ENOMEM", atoi(argv[2]));
			return;
		}
		char *cmdcur = cmd; /* string cursor */

		for (int argi = 4; argi < argc; ++argi) {
			int argl;
			argl = strlen(argv[argi]);
			memcpy(cmdcur, argv[argi], argl);
			cmdcur+=argl;
			*cmdcur = ' ';
			cmdcur++;
		}
		cmdcur--;
		*cmdcur = 0;

		/* Now execute the JS */

		moutf(MOUT_WINDOW, "JS WIN %d RET %s", atoi(argv[2]),
		      browser_window_exec(gw->bw, cmd, total - 1) ? "TRUE" : "FALSE");

		free(cmd);
	}
}


static void
xenon_window_handle_click(int argc, char **argv)
{
	/* `WINDOW CLICK WIN` _%id%_ `X` _%num%_ `Y` _%num%_ `BUTTON` _%str%_ `KIND` _%str%_ */
	/*  0      1     2    3       4  5        6  7        8       9        10    11      */
	struct gui_window *gw;
	if (argc != 12) {
		moutf(MOUT_ERROR, "WINDOW CLICK ARGS BAD\n");
	}

	gw = xenon_find_window_by_num(atoi(argv[2]));

	if (gw == NULL) {
		moutf(MOUT_ERROR, "WINDOW NUM BAD");
	} else {
		int x = atoi(argv[5]);
		int y = atoi(argv[7]);
		browser_mouse_state mouse;
		const char *button = argv[9];
		const char *kind = argv[11];
		if (strcmp(button, "LEFT") == 0) {
			mouse = BROWSER_MOUSE_CLICK_1;
		} else if (strcmp(button, "RIGHT") == 0) {
			mouse = BROWSER_MOUSE_CLICK_2;
		} else {
			moutf(MOUT_ERROR, "WINDOW BUTTON BAD");
			return;
		}
		if (strcmp(kind, "SINGLE") == 0) {
			/* Nothing */
		} else if (strcmp(kind, "DOUBLE") == 0) {
			mouse |= BROWSER_MOUSE_DOUBLE_CLICK;
		} else if (strcmp(kind, "TRIPLE") == 0) {
			mouse |= BROWSER_MOUSE_TRIPLE_CLICK;
		} else {
			moutf(MOUT_ERROR, "WINDOW KIND BAD");
			return;
		}
		browser_window_mouse_click(gw->bw, mouse, x, y);
	}
}

void
xenon_window_handle_command(int argc, char **argv)
{
	if (argc == 1)
		return;

	if (strcmp(argv[1], "NEW") == 0) {
		xenon_window_handle_new(argc, argv);
	} else if (strcmp(argv[1], "DESTROY") == 0) {
		xenon_window_handle_destroy(argc, argv);
	} else if (strcmp(argv[1], "GO") == 0) {
		xenon_window_handle_go(argc, argv);
	} else if (strcmp(argv[1], "STOP") == 0) {
		xenon_window_handle_stop(argc, argv);
	} else if (strcmp(argv[1], "REDRAW") == 0) {
		xenon_window_handle_redraw(argc, argv);
	} else if (strcmp(argv[1], "RELOAD") == 0) {
		xenon_window_handle_reload(argc, argv);
	} else if (strcmp(argv[1], "EXEC") == 0) {
		xenon_window_handle_exec(argc, argv);
	} else if (strcmp(argv[1], "CLICK") == 0) {
		xenon_window_handle_click(argc, argv);
	} else {
		moutf(MOUT_ERROR, "WINDOW COMMAND UNKNOWN %s\n", argv[1]);
	}

}

/**
 * process miscellaneous window events
 *
 * \param gw The window receiving the event.
 * \param event The event code.
 * \return NSERROR_OK when processed ok
 */
static nserror
gui_window_event(struct gui_window *gw, enum gui_window_event event)
{
	switch (event) {
	case GW_EVENT_UPDATE_EXTENT:
		gui_window_update_extent(gw);
		break;

	case GW_EVENT_REMOVE_CARET:
		gui_window_remove_caret(gw);
		break;

	case GW_EVENT_SCROLL_START:
		gui_window_scroll_start(gw);
		break;

	case GW_EVENT_NEW_CONTENT:
		gui_window_new_content(gw);
		break;

	case GW_EVENT_START_THROBBER:
		gui_window_start_throbber(gw);
		break;

	case GW_EVENT_STOP_THROBBER:
		gui_window_stop_throbber(gw);
		break;

	case GW_EVENT_PAGE_INFO_CHANGE:
		gui_window_report_page_info(gw);
		break;

	default:
		break;
	}
	return NSERROR_OK;
}

static struct gui_window_table window_table = {
	.create = gui_window_create,
	.destroy = gui_window_destroy,
	.invalidate = xenon_window_invalidate_area,
	.get_scroll = gui_window_get_scroll,
	.set_scroll = gui_window_set_scroll,
	.get_dimensions = gui_window_get_dimensions,
	.event = gui_window_event,

	.set_title = gui_window_set_title,
	.set_url = gui_window_set_url,
	.set_icon = gui_window_set_icon,
	.set_status = gui_window_set_status,
	.set_pointer = gui_window_set_pointer,
	.place_caret = gui_window_place_caret,
	.drag_start = gui_window_drag_start,
	.save_link = gui_window_save_link,

	.console_log = gui_window_console_log,
};

struct gui_window_table *xenon_window_table = &window_table;

/* The page's scroll range: how far it can scroll down and right. */
static void scroll_limits(struct gui_window *g, int *max_x, int *max_y)
{
	int width = 0;
	int height = 0;

	if (browser_window_get_extents(g->bw, true, &width, &height) != NSERROR_OK) {
		width = 0;
		height = 0;
	}
	*max_x = width > XENON_WIDTH ? width - XENON_WIDTH : 0;
	*max_y = height > XENON_PAGE_HEIGHT ? height - XENON_PAGE_HEIGHT : 0;
}

bool xenon_scroll_by(int dx, int dy)
{
	struct gui_window *g = xenon_tab_active();
	int max_x, max_y, x, y;

	if (g == NULL || g->bw == NULL)
		return false;
	scroll_limits(g, &max_x, &max_y);
	x = g->scrollx + dx;
	y = g->scrolly + dy;
	if (x > max_x)
		x = max_x;
	if (y > max_y)
		y = max_y;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x == g->scrollx && y == g->scrolly)
		return false;
	g->scrollx = x;
	g->scrolly = y;
	xenon_needs_redraw = true;
	return true;
}

bool xenon_scroll_info(int *y, int *view, int *total)
{
	struct gui_window *g = xenon_tab_active();
	int max_x, max_y;

	if (g == NULL || g->bw == NULL)
		return false;
	scroll_limits(g, &max_x, &max_y);
	if (max_y <= 0)
		return false;
	*y = g->scrolly;
	*view = XENON_PAGE_HEIGHT;
	*total = max_y + XENON_PAGE_HEIGHT;
	return true;
}
