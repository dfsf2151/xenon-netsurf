/*
 * Xenon browser chrome: dashboard-style top bar, tabs, speed dial,
 * address/search bar, options menu, on-screen keyboard and status bar.
 *
 * The page is drawn by NetSurf into the area between the fixed chrome
 * (xenon_plot_* adds XENON_CHROME_TOP), and this file draws the chrome
 * straight into the presented tile buffer. Moving the pointer therefore
 * never forces the page to be laid out again.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "netsurf/mouse.h"

#include "desktop/browser_history.h"

#include "xenon/audio.h"
#include "xenon/browser.h"
#include "xenon/input.h"
#include "xenon/time.h"
#include "xenon/ui.h"
#include "xenon/xenon.h"

volatile bool xenon_quit_requested;

enum {
	CURSOR_SCALE = 2,
	TOAST_MS = 5000,
	URL_MAX = 480,

	CHROME_TOP = XENON_CHROME_TOP,
	CHROME_BOTTOM = XENON_CHROME_BOTTOM,
	BOTTOM_Y = XENON_HEIGHT - CHROME_BOTTOM,

	ADDR_X0 = 158,
	ADDR_X1 = 860,
	ADDR_CY = 45,
	SEARCH_CX = 832,

	TAB_Y = 100,
	TAB_H = 44,

	OSK_X = 114,
	OSK_Y = 286,
	OSK_KEY_W = 84,
	OSK_KEY_H = 52,
	OSK_COLS = 12,
	OSK_ROWS = 4,
	OSK_KEYS = OSK_COLS * OSK_ROWS,
	OSK_ACTIONS = 6,
	OSK_ACTION_W = 172,

	DIAL_PER_PAGE = 6,
	DIAL_COUNT = 12,
	DIAL_X = 67,
	DIAL_Y = 212,
	DIAL_W = 176,
	DIAL_H = 214,
	DIAL_GAP = 18,
};

enum {
	C_CHROME = 0xdfeccd,
	C_CHROME_DARK = 0xc9dcb2,
	C_CHROME_LINE = 0xb9cda2,
	C_GREEN = 0x7cb342,
	C_GREEN_DARK = 0x558b2f,
	C_INK = 0x1e2a16,
	C_MUTED = 0x5f6f52,
	C_WHITE = 0xffffff,
	C_PAGE = 0xf4f8ee,
	C_CARD_LINE = 0xd3e0c2,
	C_TAB_IDLE = 0xe8f2dc,
	C_HOVER = 0xc8e6a0,
};

static const char osk_chars[OSK_KEYS] = {
	'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
	'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
	'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
	'.', '/', ':', '-', '_', '~', '?', '=', '&', '%', '@', '+',
};

static const char *const osk_action_names[OSK_ACTIONS] = {
	"Shift", "Space", "Del", "Clear", "Go", "Cancel"
};

static int cursor_x = 640;
static int cursor_y = 300;
static bool cursor_hidden;

static bool url_mode;
static bool search_mode;
static bool caps;
static int key_sel;
static char url_text[URL_MAX + 1];
static size_t url_len;

static bool menu_mode;
static int menu_sel;
static bool a_consumed;

static int dial_page;

static char toast[200];
static uint64_t toast_until;

static bool dirty;
static int last_clock_min = -1;
static uint64_t last_loading_tick;

typedef enum {
	ICON_GLOBE,
	ICON_SEARCH,
	ICON_TROPHY,
	ICON_PEOPLE,
	ICON_PICTURE,
	ICON_HELP,
	ICON_PLAY,
	ICON_ORB,
	ICON_N,
	ICON_CODE,
	ICON_E,
	ICON_INFO,
} dial_icon;

struct dial {
	const char *label;
	const char *url;
	dial_icon icon;
};

static const struct dial dials[DIAL_COUNT] = {
	{ "Wikipedia", "https://en.wikipedia.org/wiki/Xbox_360", ICON_GLOBE },
	{ "Search", "", ICON_SEARCH },
	{ "News", "https://news.ycombinator.com/", ICON_TROPHY },
	{ "Simple Wiki", "https://simple.wikipedia.org/wiki/Web_browser", ICON_PEOPLE },
	{ "Pictures", "https://commons.wikimedia.org/wiki/Main_Page", ICON_PICTURE },
	{ "Help", "file:///game:/help.html", ICON_HELP },
	{ "YouTube", "https://www.youtube.com/", ICON_PLAY },
	{ "Xbox 360", "https://en.wikipedia.org/wiki/Xbox_360", ICON_ORB },
	{ "NetSurf", "https://www.netsurf-browser.org/", ICON_N },
	{ "HTML", "https://en.wikipedia.org/wiki/HTML", ICON_CODE },
	{ "Example", "https://example.com/", ICON_E },
	{ "About", "file:///game:/about.html", ICON_INFO },
};

static const char *const menu_items[] = {
	"New Tab", "Reload Page", "Home", "Close Tab",
	"Play demo.wav", "Play demo.mp3", "Sound Test", "Help", "Quit"
};
#define MENU_ITEMS ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

static const int chrome_btn_x[5] = { 900, 954, 1008, 1062, 1116 };

static int tab_width(void)
{
	int n = xenon_tab_count();
	int width;

	if (n < 1)
		return 220;
	width = (1000 - (n - 1) * 6) / n;
	if (width > 220)
		width = 220;
	if (width < 120)
		width = 120;
	return width;
}

static int plus_center_x(void)
{
	int n = xenon_tab_count();
	return 16 + n * (tab_width() + 6) + 18;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void set_dirty(void)
{
	dirty = true;
}

void xenon_ui_chrome_changed(void)
{
	set_dirty();
}

bool xenon_ui_dirty_take(void)
{
	bool was = dirty;
	dirty = false;
	return was;
}

void xenon_ui_set_cursor(int x, int y)
{
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x > XENON_WIDTH - 1)
		x = XENON_WIDTH - 1;
	if (y > XENON_HEIGHT - 1)
		y = XENON_HEIGHT - 1;
	if (x != cursor_x || y != cursor_y) {
		cursor_x = x;
		cursor_y = y;
		set_dirty();
	}
}

void xenon_ui_toast(const char *text)
{
	if (text == NULL || text[0] == '\0')
		return;
	snprintf(toast, sizeof(toast), "%s", text);
	toast_until = now_ms() + TOAST_MS;
	set_dirty();
}

void xenon_ui_init(void)
{
	url_text[0] = '\0';
	url_len = 0;
	cursor_x = 640;
	cursor_y = 310;
	key_sel = 0;
	menu_sel = 0;
	xenon_ui_toast("A open   B back   X options   Y search");
}

/* ------------------------------------------------------------------ */
/* Tabs and navigation                                                */
/* ------------------------------------------------------------------ */

static int active_tab(void)
{
	for (int i = 0; i < xenon_tab_count(); i++) {
		struct gui_window *g = xenon_tab_at(i);
		if (g != NULL && g->bw == xenon_bw)
			return i;
	}
	return -1;
}

static bool on_home(void)
{
	int index = active_tab();
	return index < 0 || xenon_tab_is_home(index);
}

static bool looks_like_url(const char *text)
{
	const char *dot;

	if (strstr(text, "://") != NULL)
		return true;
	if (strncasecmp(text, "www.", 4) == 0)
		return true;
	if (strchr(text, ' ') != NULL || strchr(text, '\t') != NULL)
		return false;
	dot = strrchr(text, '.');
	return dot != NULL && strlen(dot + 1) >= 2;
}

static void url_encode(const char *text, char *out, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t used = 0;

	for (const unsigned char *p = (const unsigned char *)text;
	     *p != '\0' && used + 4 < size; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
		    *p == '.' || *p == '~') {
			out[used++] = (char)*p;
		} else if (*p == ' ') {
			out[used++] = '+';
		} else {
			out[used++] = '%';
			out[used++] = hex[*p >> 4];
			out[used++] = hex[*p & 15];
		}
	}
	out[used] = '\0';
}

static bool navigation_ok(nserror error)
{
	if (error == NSERROR_OK)
		return true;
	xenon_ui_toast("Navigation failed");
	return false;
}

static void go_url(const char *text)
{
	char trimmed[URL_MAX + 1];
	char target[URL_MAX * 3 + 200];
	char encoded[URL_MAX * 3 + 1];
	nsurl *url = NULL;
	nserror error;
	size_t len;
	bool search;

	while (*text == ' ')
		text++;
	snprintf(trimmed, sizeof(trimmed), "%s", text);
	len = strlen(trimmed);
	while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\n' ||
			   trimmed[len - 1] == '\r')) {
		trimmed[--len] = '\0';
	}
	if (len == 0) {
		xenon_ui_toast("Type an address or a search");
		return;
	}

	search = false;
	if (trimmed[0] == '?') {
		search = true;
		memmove(trimmed, trimmed + 1, strlen(trimmed));
	} else if (!looks_like_url(trimmed)) {
		search = true;
	}

	if (search) {
		url_encode(trimmed, encoded, sizeof(encoded));
		snprintf(target, sizeof(target),
			 "https://lite.duckduckgo.com/lite/?q=%s", encoded);
	} else if (strstr(trimmed, "://") != NULL) {
		snprintf(target, sizeof(target), "%s", trimmed);
	} else {
		snprintf(target, sizeof(target), "https://%s", trimmed);
	}

	if (strncmp(target, "file:///game:/", 14) == 0 &&
	    (strstr(target, ".wav") != NULL || strstr(target, ".mp3") != NULL)) {
		char path[URL_MAX + 16];
		snprintf(path, sizeof(path), "game:\\%s", target + 14);
		if (xenon_audio_play_file(path)) {
			url_mode = false;
			cursor_hidden = false;
			xenon_ui_toast(xenon_audio_status());
		} else {
			xenon_ui_toast(xenon_audio_status());
		}
		return;
	}

	error = nsurl_create(target, &url);
	if (error != NSERROR_OK) {
		xenon_ui_toast("Bad address");
		return;
	}

	if (xenon_bw == NULL)
		error = browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, NULL);
	else
		error = browser_window_navigate(xenon_bw, url, NULL,
						BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
	nsurl_unref(url);

	if (navigation_ok(error)) {
		url_mode = false;
		cursor_hidden = false;
		xenon_ui_toast(search ? "Searching..." : "Loading...");
	}
}

void xenon_ui_script_url(const char *url)
{
	go_url(url);
}

static void go_home(void)
{
	go_url("file:///game:/home.html");
}

static void open_url_bar(bool clear)
{
	toast[0] = '\0';
	url_text[0] = '\0';
	url_len = 0;
	search_mode = clear;

	if (!clear) {
		int index = active_tab();
		const char *current = index >= 0 ? xenon_tab_url(index) : "";
		if (current[0] != '\0' && strstr(current, "/home.html") == NULL) {
			snprintf(url_text, sizeof(url_text), "%s", current);
			url_len = strlen(url_text);
		}
	}

	url_mode = true;
	cursor_hidden = true;
	key_sel = 0;
	set_dirty();
}

static void url_insert(char c)
{
	if (url_len >= URL_MAX)
		return;
	url_text[url_len++] = c;
	url_text[url_len] = '\0';
	set_dirty();
}

static void url_backspace(void)
{
	if (url_len == 0)
		return;
	url_text[--url_len] = '\0';
	set_dirty();
}

static void close_url_bar(void)
{
	url_mode = false;
	cursor_hidden = false;
	set_dirty();
}

static void activate_key(void)
{
	if (key_sel < OSK_KEYS) {
		char c = osk_chars[key_sel];
		if (caps && c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		url_insert(c);
		return;
	}

	switch (key_sel - OSK_KEYS) {
	case 0:
		caps = !caps;
		set_dirty();
		break;
	case 1:
		url_insert(' ');
		break;
	case 2:
		url_backspace();
		break;
	case 3:
		url_len = 0;
		url_text[0] = '\0';
		set_dirty();
		break;
	case 4:
		go_url(url_text);
		break;
	case 5:
		close_url_bar();
		break;
	}
}

static void key_move(int dx, int dy)
{
	int col;
	int row;

	if (key_sel < OSK_KEYS) {
		col = key_sel % OSK_COLS;
		row = key_sel / OSK_COLS;
		if (dy != 0) {
			row += dy;
			if (row < 0)
				row = 0;
			if (row > OSK_ROWS - 1)
				row = OSK_ROWS - 1;
			key_sel = row * OSK_COLS + col;
		} else if (dx != 0) {
			col += dx;
			if (col < 0)
				col = OSK_COLS - 1;
			if (col > OSK_COLS - 1)
				col = 0;
			key_sel = row * OSK_COLS + col;
		}
	} else {
		int action = key_sel - OSK_KEYS;
		if (dy < 0) {
			key_sel = (OSK_ROWS - 1) * OSK_COLS +
				  (action * 2 < OSK_COLS ? action * 2 : OSK_COLS - 1);
		} else if (dy > 0) {
			/* stay on the action row */
		} else if (dx != 0) {
			action += dx;
			if (action < 0)
				action = OSK_ACTIONS - 1;
			if (action >= OSK_ACTIONS)
				action = 0;
			key_sel = OSK_KEYS + action;
		}
	}
	set_dirty();
}

/* ------------------------------------------------------------------ */
/* Hit testing                                                        */
/* ------------------------------------------------------------------ */

static bool point_in(int x, int y, int x0, int y0, int x1, int y1)
{
	return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static bool over_circle(int x, int y, int cx, int cy, int radius)
{
	int dx = x - cx;
	int dy = y - cy;
	return dx * dx + dy * dy <= radius * radius;
}

static void dial_rect(int index, int *x0, int *y0, int *x1, int *y1)
{
	int slot = index % DIAL_PER_PAGE;
	*x0 = DIAL_X + slot * (DIAL_W + DIAL_GAP);
	*y0 = DIAL_Y;
	*x1 = *x0 + DIAL_W;
	*y1 = *y0 + DIAL_H;
}

static void handle_tabs_click(int x, int y)
{
	int n = xenon_tab_count();
	int width = tab_width();

	for (int i = 0; i < n; i++) {
		int x0 = 16 + i * (width + 6);
		if (x < x0 || x >= x0 + width || y < TAB_Y || y >= TAB_Y + TAB_H)
			continue;
		if (x >= x0 + width - 26 && i == active_tab()) {
			xenon_tab_close(i);
			if (xenon_tab_count() == 0)
				xenon_tab_new();
		} else {
			xenon_tab_activate(i);
		}
		return;
	}

	if (over_circle(x, y, plus_center_x(), TAB_Y + 22, 22)) {
		if (n >= 6) {
			xenon_ui_toast("Six tabs is the limit");
		} else {
			xenon_tab_new();
		}
	}
}

static void handle_chrome_click(int x, int y)
{
	enum { BTN_BACK, BTN_FORWARD, BTN_RELOAD, BTN_HOME, BTN_MENU };

	if (point_in(x, y, ADDR_X0, 20, ADDR_X1, 70)) {
		open_url_bar(false);
		return;
	}
	if (over_circle(x, y, SEARCH_CX, ADDR_CY, 24)) {
		open_url_bar(true);
		return;
	}
	if (y >= TAB_Y && y < TAB_Y + TAB_H) {
		handle_tabs_click(x, y);
		return;
	}
	if (over_circle(x, y, 44, 46, 28)) {
		go_home();
		return;
	}
	if (over_circle(x, y, 118, 46, 30)) {
		go_home();
		return;
	}

	for (int i = 0; i < 5; i++) {
		if (!over_circle(x, y, chrome_btn_x[i], 46, 26))
			continue;
		switch (i) {
		case BTN_BACK:
			if (xenon_bw != NULL)
				browser_window_history_back(xenon_bw, false);
			break;
		case BTN_FORWARD:
			if (xenon_bw != NULL)
				browser_window_history_forward(xenon_bw, false);
			break;
		case BTN_RELOAD:
			if (xenon_bw != NULL)
				browser_window_reload(xenon_bw, false);
			break;
		case BTN_HOME:
			go_home();
			break;
		case BTN_MENU:
			toast[0] = '\0';
			menu_mode = true;
			menu_sel = 0;
			set_dirty();
			break;
		}
		return;
	}
}

static void handle_dial_click(int x, int y)
{
	if (over_circle(x, y, 30, 319, 26)) {
		if (dial_page > 0) {
			dial_page--;
			set_dirty();
		}
		return;
	}
	if (over_circle(x, y, 1250, 319, 26)) {
		if (dial_page < (DIAL_COUNT / DIAL_PER_PAGE) - 1) {
			dial_page++;
			set_dirty();
		}
		return;
	}

	for (int i = 0; i < DIAL_PER_PAGE; i++) {
		int index = dial_page * DIAL_PER_PAGE + i;
		int x0, y0, x1, y1;
		dial_rect(index, &x0, &y0, &x1, &y1);
		if (!point_in(x, y, x0, y0, x1, y1))
			continue;
		if (dials[index].url[0] == '\0') {
			open_url_bar(true);
		} else {
			go_url(dials[index].url);
		}
		return;
	}
}

static void handle_bottom_click(int x, int y)
{
	if (over_circle(x, y, 1104, 698, 22)) {
		go_url("file:///game:/help.html");
		return;
	}
}

static void menu_activate(void)
{
	int index = active_tab();

	switch (menu_sel) {
	case 0:
		if (xenon_tab_count() >= 6)
			xenon_ui_toast("Six tabs is the limit");
		else
			xenon_tab_new();
		break;
	case 1:
		if (xenon_bw != NULL)
			browser_window_reload(xenon_bw, false);
		break;
	case 2:
		go_home();
		break;
	case 3:
		if (index >= 0) {
			xenon_tab_close(index);
			if (xenon_tab_count() == 0)
				xenon_tab_new();
		}
		break;
	case 4:
		if (xenon_audio_play_file("game:\\demo.wav"))
			xenon_ui_toast("Playing demo.wav");
		else
			xenon_ui_toast(xenon_audio_status());
		break;
	case 5:
		if (xenon_audio_play_file("game:\\demo.mp3"))
			xenon_ui_toast("Playing demo.mp3");
		else
			xenon_ui_toast(xenon_audio_status());
		break;
	case 6:
		if (xenon_audio_play_tone(660, 1200))
			xenon_ui_toast("Playing 660 Hz test tone");
		else
			xenon_ui_toast("Audio not available");
		break;
	case 7:
		go_url("file:///game:/help.html");
		break;
	case 8:
		xenon_quit_requested = true;
		break;
	}
	menu_mode = false;
	set_dirty();
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

void xenon_ui_update(void)
{
	uint64_t now = now_ms();
	time_t wall = xenon_time(NULL);
	struct tm tm;
	int dx = 0;
	int dy = 0;

	if (toast[0] != '\0' && now > toast_until) {
		toast[0] = '\0';
		set_dirty();
	}

	if (gmtime_r(&wall, &tm) != NULL && tm.tm_min != last_clock_min) {
		last_clock_min = tm.tm_min;
		set_dirty();
	}

	if (xenon_is_loading() && now - last_loading_tick > 250) {
		last_loading_tick = now;
		set_dirty();
	}

	if (xenon_input_pressed(XB_START)) {
		xenon_quit_requested = true;
		return;
	}

	if (url_mode) {
		static uint64_t next_repeat;
		int step_x = 0;
		int step_y = 0;

		if (xenon_input_pressed(XB_LEFT)) {
			step_x = -1;
			next_repeat = now + 380;
		} else if (xenon_input_pressed(XB_RIGHT)) {
			step_x = 1;
			next_repeat = now + 380;
		} else if (xenon_input_pressed(XB_UP)) {
			step_y = -1;
			next_repeat = now + 380;
		} else if (xenon_input_pressed(XB_DOWN)) {
			step_y = 1;
			next_repeat = now + 380;
		} else if (now >= next_repeat) {
			if (xenon_input_down(XB_LEFT))
				step_x = -1;
			else if (xenon_input_down(XB_RIGHT))
				step_x = 1;
			else if (xenon_input_down(XB_UP))
				step_y = -1;
			else if (xenon_input_down(XB_DOWN))
				step_y = 1;
			if (step_x != 0 || step_y != 0)
				next_repeat = now + 95;
		}
		if (step_x != 0 || step_y != 0)
			key_move(step_x, step_y);

		if (xenon_input_pressed(XB_A))
			activate_key();
		if (xenon_input_pressed(XB_X))
			url_backspace();
		if (xenon_input_pressed(XB_Y))
			go_url(url_text);
		if (xenon_input_pressed(XB_B))
			close_url_bar();
		return;
	}

	if (menu_mode) {
		if (xenon_input_pressed(XB_UP)) {
			menu_sel = (menu_sel + MENU_ITEMS - 1) % MENU_ITEMS;
			set_dirty();
		}
		if (xenon_input_pressed(XB_DOWN)) {
			menu_sel = (menu_sel + 1) % MENU_ITEMS;
			set_dirty();
		}
		if (xenon_input_pressed(XB_A))
			menu_activate();
		if (xenon_input_pressed(XB_B) || xenon_input_pressed(XB_X)) {
			menu_mode = false;
			set_dirty();
		}
		return;
	}

	xenon_input_delta(&dx, &dy);
	if (dx != 0 || dy != 0)
		xenon_ui_set_cursor(cursor_x + dx, cursor_y + dy);

	if (xenon_input_pressed(XB_Y)) {
		open_url_bar(true);
		return;
	}
	if (xenon_input_pressed(XB_X)) {
		toast[0] = '\0';
		menu_mode = true;
		menu_sel = 0;
		set_dirty();
		return;
	}
	if (xenon_input_pressed(XB_BACK)) {
		go_home();
		return;
	}
	if (xenon_bw == NULL) {
		if (xenon_input_pressed(XB_A))
			xenon_tab_new();
		return;
	}

	if (xenon_input_pressed(XB_B))
		browser_window_history_back(xenon_bw, false);
	if (xenon_input_pressed(XB_LB))
		browser_window_scroll_at_point(xenon_bw, 600,
					       XENON_PAGE_HEIGHT / 2, 0,
					       -(XENON_PAGE_HEIGHT - 70));
	if (xenon_input_pressed(XB_RB))
		browser_window_scroll_at_point(xenon_bw, 600,
					       XENON_PAGE_HEIGHT / 2, 0,
					       XENON_PAGE_HEIGHT - 70);

	if (xenon_input_pressed(XB_A)) {
		a_consumed = true;
		if (cursor_y < CHROME_TOP)
			handle_chrome_click(cursor_x, cursor_y);
		else if (cursor_y >= BOTTOM_Y)
			handle_bottom_click(cursor_x, cursor_y);
		else if (on_home())
			handle_dial_click(cursor_x, cursor_y);
		else {
			a_consumed = false;
			browser_window_mouse_click(xenon_bw, BROWSER_MOUSE_PRESS_1,
						   cursor_x,
						   cursor_y - CHROME_TOP);
		}
	}

	if (xenon_input_released(XB_A)) {
		if (!a_consumed && !on_home() && cursor_y >= CHROME_TOP &&
		    cursor_y < BOTTOM_Y)
			browser_window_mouse_click(xenon_bw, BROWSER_MOUSE_CLICK_1,
						   cursor_x,
						   cursor_y - CHROME_TOP);
		a_consumed = false;
	}
}

/* ------------------------------------------------------------------ */
/* Drawing helpers                                                    */
/* ------------------------------------------------------------------ */

static void text_at(int x, int y, const char *text, uint32_t colour)
{
	xenon_overlay_text(x, y, text, colour);
}

static void text_bold(int x, int y, const char *text, uint32_t colour)
{
	xenon_overlay_text(x, y, text, colour);
	xenon_overlay_text(x + 1, y, text, colour);
}

static void text_centre(int cx, int y, const char *text, uint32_t colour,
			int scale)
{
	int width = (int)strlen(text) * 8 * scale;
	if (width > 0)
		xenon_overlay_text_scaled(cx - width / 2, y, text, colour, scale);
}

static void text_right(int right, int y, const char *text, uint32_t colour)
{
	int width = (int)strlen(text) * 8;
	xenon_overlay_text(right - width, y, text, colour);
}

static void badge(int cx, int cy, const char *label, uint32_t fill,
		  uint32_t ink)
{
	xenon_overlay_disc(cx, cy, 11, fill);
	text_centre(cx, cy - 8, label, ink, 1);
}

static void chevron(int cx, int cy, int dir, uint32_t colour)
{
	int tip = cx + 6 * dir;
	int tail = cx - 6 * dir;

	xenon_overlay_line(tail, cy - 10, tip, cy, 4, colour);
	xenon_overlay_line(tip, cy, tail, cy + 10, 4, colour);
}

static void draw_search_icon(int cx, int cy, uint32_t colour)
{
	xenon_overlay_ring(cx - 2, cy - 2, 9, 3, colour);
	xenon_overlay_line(cx + 5, cy + 5, cx + 13, cy + 13, 4, colour);
}

static void draw_button(int cx, int cy, int id)
{
	bool hover = over_circle(cursor_x, cursor_y, cx, cy, 25);
	uint32_t ink = C_INK;

	xenon_overlay_disc(cx, cy, 21, hover ? C_HOVER : 0xf4f9ed);
	xenon_overlay_ring(cx, cy, 21, 2, C_CHROME_LINE);

	switch (id) {
	case 0:
		chevron(cx, cy, -1, ink);
		badge(cx + 16, cy + 15, "B", C_GREEN, C_WHITE);
		break;
	case 1:
		chevron(cx, cy, 1, ink);
		break;
	case 2:
		xenon_overlay_ring(cx, cy, 11, 3, ink);
		xenon_overlay_triangle(cx + 5, cy - 15, cx + 14, cy - 13,
				       cx + 8, cy - 5, ink);
		break;
	case 3:
		xenon_overlay_triangle(cx - 12, cy - 2, cx, cy - 13,
				       cx + 12, cy - 2, ink);
		xenon_overlay_rect(cx - 9, cy - 2, cx + 9, cy + 11, ink);
		xenon_overlay_rect(cx - 3, cy + 3, cx + 3, cy + 11, 0xf4f9ed);
		break;
	case 4:
		xenon_overlay_line(cx - 11, cy - 8, cx + 11, cy - 8, 3, ink);
		xenon_overlay_line(cx - 11, cy, cx + 11, cy, 3, ink);
		xenon_overlay_line(cx - 11, cy + 8, cx + 11, cy + 8, 3, ink);
		xenon_overlay_disc(cx - 3, cy - 8, 3, ink);
		xenon_overlay_disc(cx + 5, cy, 3, ink);
		xenon_overlay_disc(cx - 6, cy + 8, 3, ink);
		badge(cx + 16, cy + 15, "X", C_GREEN, C_WHITE);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Chrome                                                             */
/* ------------------------------------------------------------------ */

static void draw_top_bar(void)
{
	int active = active_tab();
	int n = xenon_tab_count();
	const char *current = active >= 0 ? xenon_tab_url(active) : "";
	char address[300];
	size_t shown;

	xenon_overlay_rect(0, 0, XENON_WIDTH, 92, C_CHROME);
	xenon_overlay_rect(0, 92, XENON_WIDTH, 94, C_CHROME_DARK);

	/* NetSurf roundel, in the place of the IE "e". */
	xenon_overlay_disc(44, 46, 28, C_GREEN);
	xenon_overlay_ring(44, 46, 28, 3, C_GREEN_DARK);
	text_centre(44, 30, "N", C_WHITE, 2);

	/* Home button, left of the address field. */
	xenon_overlay_disc(118, 46, 24, over_circle(cursor_x, cursor_y, 118, 46, 26) ?
			   C_HOVER : 0xf4f9ed);
	xenon_overlay_ring(118, 46, 24, 2, C_CHROME_LINE);
	xenon_overlay_triangle(106, 45, 118, 33, 130, 45, C_INK);
	xenon_overlay_rect(109, 44, 127, 57, C_INK);
	xenon_overlay_rect(115, 49, 121, 57, 0xf4f9ed);

	/* Address field. */
	xenon_overlay_round_rect(ADDR_X0, 20, ADDR_X1, 70, 14, C_WHITE);
	xenon_overlay_round_frame(ADDR_X0, 20, ADDR_X1, 70, 14, 2, C_CHROME_LINE);
	badge(176, 45, "A", C_GREEN, C_WHITE);

	if (current[0] == '\0' || strstr(current, "/home.html") != NULL) {
		text_at(192, 42, "Search or enter address", C_MUTED);
	} else {
		shown = strlen(current);
		if (shown > 76) {
			snprintf(address, sizeof(address), "%.73s...",
				 current);
		} else {
			snprintf(address, sizeof(address), "%s", current);
		}
		text_at(192, 42, address, C_INK);
	}

	/* Search. */
	xenon_overlay_disc(SEARCH_CX, ADDR_CY, 19,
			   over_circle(cursor_x, cursor_y, SEARCH_CX, ADDR_CY, 22) ?
			   C_GREEN_DARK : C_GREEN);
	draw_search_icon(SEARCH_CX, ADDR_CY, C_WHITE);
	badge(SEARCH_CX + 12, ADDR_CY + 14, "Y", C_GREEN_DARK, C_WHITE);

	for (int i = 0; i < 5; i++)
		draw_button(chrome_btn_x[i], 46, i);

	/* Tab strip. */
	xenon_overlay_rect(0, 94, XENON_WIDTH, CHROME_TOP, C_CHROME_DARK);
	for (int i = 0; i < n; i++) {
		int width = tab_width();
		int x0;
		int x1;
		bool is_active = (i == active);
		char title[64];
		int max_chars;

		x0 = 16 + i * (width + 6);
		x1 = x0 + width;

		if (is_active) {
			xenon_overlay_round_rect(x0, TAB_Y, x1, TAB_Y + TAB_H,
						 8, C_WHITE);
			xenon_overlay_round_frame(x0, TAB_Y, x1, TAB_Y + TAB_H,
						  8, 2, C_GREEN);
			xenon_overlay_rect(x0 + 2, TAB_Y + 2, x1 - 2, TAB_Y + 8,
					   C_GREEN);
		} else {
			xenon_overlay_round_rect(x0, TAB_Y + 4, x1, TAB_Y + TAB_H,
						 8, C_TAB_IDLE);
		}

		max_chars = (width - 40) / 8;
		if (max_chars < 3)
			max_chars = 3;
		snprintf(title, sizeof(title), "%.*s", max_chars,
			 xenon_tab_title(i));
		text_at(x0 + 12, TAB_Y + (is_active ? 18 : 20), title,
			is_active ? C_INK : C_MUTED);

		if (is_active) {
			int cx = x1 - 16;
			int cy = TAB_Y + 26;
			xenon_overlay_line(cx - 5, cy - 5, cx + 5, cy + 5, 2,
					   C_MUTED);
			xenon_overlay_line(cx - 5, cy + 5, cx + 5, cy - 5, 2,
					   C_MUTED);
		}
	}

	/* New tab. */
	{
		int px = plus_center_x();
		int py = TAB_Y + 22;
		uint32_t fill = over_circle(cursor_x, cursor_y, px, py, 20) ?
				C_GREEN_DARK : C_GREEN;
		xenon_overlay_disc(px, py, 18, fill);
		xenon_overlay_rect(px - 8, py - 2, px + 8, py + 2, C_WHITE);
		xenon_overlay_rect(px - 2, py - 8, px + 2, py + 8, C_WHITE);
	}
}

static void draw_bottom_bar(void)
{
	char clock_text[24];

	xenon_overlay_rect(0, BOTTOM_Y, XENON_WIDTH, XENON_HEIGHT, C_CHROME);
	xenon_overlay_rect(0, BOTTOM_Y, XENON_WIDTH, BOTTOM_Y + 2,
			   C_CHROME_DARK);

	xenon_overlay_disc(34, 698, 19, C_GREEN_DARK);
	text_centre(34, 690, "P1", C_WHITE, 1);
	text_at(62, 684, "PLAYER ONE", C_INK);
	text_at(62, 702, "NetSurf 360  /  online", C_MUTED);

	xenon_overlay_disc(1104, 698, 17,
			   over_circle(cursor_x, cursor_y, 1104, 698, 20) ?
			   C_GREEN_DARK : C_GREEN);
	text_centre(1104, 690, "?", C_WHITE, 1);

	xenon_clock_text(clock_text, sizeof(clock_text));
	text_right(1218, 684, clock_text, C_INK);
	text_right(1218, 702, "UTC", C_MUTED);

	/* Mains-powered console: a full battery is the honest decoration. */
	xenon_overlay_round_frame(1232, 692, 1268, 708, 3, 2, C_INK);
	xenon_overlay_rect(1268, 696, 1273, 704, C_INK);
	xenon_overlay_rect(1236, 696, 1264, 704, C_GREEN);
}

/* ------------------------------------------------------------------ */
/* Speed dial                                                         */
/* ------------------------------------------------------------------ */

static void draw_icon(dial_icon icon, int cx, int cy)
{
	switch (icon) {
	case ICON_GLOBE:
		xenon_overlay_disc(cx, cy, 32, 0x4a7fb5);
		xenon_overlay_ring(cx, cy, 31, 2, C_WHITE);
		xenon_overlay_ring(cx, cy, 21, 2, C_WHITE);
		xenon_overlay_line(cx - 31, cy, cx + 31, cy, 2, C_WHITE);
		xenon_overlay_line(cx, cy - 31, cx, cy + 31, 2, C_WHITE);
		break;
	case ICON_SEARCH:
		xenon_overlay_disc(cx, cy, 32, C_GREEN);
		draw_search_icon(cx, cy, C_WHITE);
		break;
	case ICON_TROPHY:
		xenon_overlay_disc(cx, cy, 32, 0xf0b429);
		xenon_overlay_round_rect(cx - 15, cy - 20, cx + 15, cy + 1, 6,
					 C_WHITE);
		xenon_overlay_ring(cx - 18, cy - 9, 8, 3, C_WHITE);
		xenon_overlay_ring(cx + 18, cy - 9, 8, 3, C_WHITE);
		xenon_overlay_rect(cx - 3, cy + 1, cx + 3, cy + 12, C_WHITE);
		xenon_overlay_round_rect(cx - 13, cy + 12, cx + 13, cy + 20, 3,
					 C_WHITE);
		break;
	case ICON_PEOPLE:
		xenon_overlay_disc(cx, cy, 32, 0x5a9e6f);
		xenon_overlay_disc(cx - 9, cy - 10, 9, C_WHITE);
		xenon_overlay_disc(cx + 12, cy - 7, 7, C_WHITE);
		xenon_overlay_round_rect(cx - 23, cy + 0, cx + 4, cy + 22, 9,
					 C_WHITE);
		xenon_overlay_round_rect(cx + 4, cy + 5, cx + 22, cy + 22, 7,
					 C_WHITE);
		break;
	case ICON_PICTURE:
		xenon_overlay_disc(cx, cy, 32, 0x4aa3a2);
		xenon_overlay_round_rect(cx - 25, cy - 19, cx + 25, cy + 19, 4,
					 C_WHITE);
		xenon_overlay_triangle(cx - 19, cy + 13, cx - 3, cy - 7,
				       cx + 9, cy + 13, 0x4aa3a2);
		xenon_overlay_triangle(cx + 1, cy + 13, cx + 12, cy + 0,
				       cx + 22, cy + 13, 0x4aa3a2);
		xenon_overlay_disc(cx + 14, cy - 9, 4, 0xf0b429);
		break;
	case ICON_HELP:
		xenon_overlay_disc(cx, cy, 32, C_GREEN);
		text_centre(cx, cy - 16, "?", C_WHITE, 2);
		break;
	case ICON_PLAY:
		xenon_overlay_round_rect(cx - 30, cy - 21, cx + 30, cy + 21, 10,
					 0xe53935);
		xenon_overlay_triangle(cx - 8, cy - 13, cx - 8, cy + 13,
				       cx + 15, cy, C_WHITE);
		break;
	case ICON_ORB:
		xenon_overlay_disc(cx, cy, 32, 0x8bc34a);
		xenon_overlay_ring(cx, cy, 31, 2, 0xe6f2d5);
		text_centre(cx, cy - 16, "X", C_WHITE, 2);
		break;
	case ICON_N:
		xenon_overlay_disc(cx, cy, 32, 0xe07b39);
		text_centre(cx, cy - 16, "N", C_WHITE, 2);
		break;
	case ICON_CODE:
		xenon_overlay_disc(cx, cy, 32, 0xe44d26);
		text_centre(cx, cy - 16, "<>", C_WHITE, 2);
		break;
	case ICON_E:
		xenon_overlay_disc(cx, cy, 32, 0x8a9a7b);
		text_centre(cx, cy - 16, "e", C_WHITE, 2);
		break;
	case ICON_INFO:
		xenon_overlay_disc(cx, cy, 32, 0x4a7fb5);
		text_centre(cx, cy - 16, "i", C_WHITE, 2);
		break;
	}
}

static void control_badge(int x, int y, const char *label)
{
	int len = (int)strlen(label);

	if (strcmp(label, "STICK") == 0) {
		xenon_overlay_disc(x, y, 15, C_WHITE);
		xenon_overlay_ring(x, y, 15, 2, C_GREEN);
		xenon_overlay_disc(x, y, 7, C_GREEN);
	} else if (len > 2) {
		int half = len * 4 + 9;
		xenon_overlay_round_rect(x - half, y - 13, x + half, y + 13, 7,
					 C_GREEN);
		text_centre(x, y - 8, label, C_WHITE, 1);
	} else {
		xenon_overlay_disc(x, y, 15, C_GREEN);
		text_centre(x, y - 8, label, C_WHITE, 1);
	}
}

static void draw_start(void)
{
	static const struct {
		const char *badge;
		const char *text;
	} controls[] = {
		{ "A", "Select / open" },
		{ "B", "Back" },
		{ "X", "Options" },
		{ "Y", "Search / address" },
		{ "LB/RB", "Scroll page" },
		{ "STICK", "Move pointer" },
		{ "BACK", "Home" },
		{ "START", "Quit" },
	};

	xenon_overlay_rect(0, CHROME_TOP, XENON_WIDTH, BOTTOM_Y, C_PAGE);

	xenon_overlay_text_scaled(54, 154, "Speed Dial", C_INK, 2);
	text_at(54, 190, "A light browser for Xbox 360 - pick a page or press Y to search",
		C_MUTED);
	text_right(1226, 158, dial_page == 0 ? "Page 1 of 2" : "Page 2 of 2",
		   C_MUTED);

	for (int i = 0; i < DIAL_PER_PAGE; i++) {
		int index = dial_page * DIAL_PER_PAGE + i;
		int x0, y0, x1, y1;
		int cx;
		bool hover;

		dial_rect(index, &x0, &y0, &x1, &y1);
		hover = point_in(cursor_x, cursor_y, x0, y0, x1, y1);
		cx = (x0 + x1) / 2;

		xenon_overlay_round_rect(x0, y0, x1, y1, 12, C_WHITE);
		xenon_overlay_round_frame(x0, y0, x1, y1, 12,
					  hover ? 3 : 2,
					  hover ? C_GREEN : C_CARD_LINE);

		draw_icon(dials[index].icon, cx, y0 + 76);

		text_centre(cx, y0 + 150, dials[index].label, C_INK, 1);
		if (hover)
			text_centre(cx, y0 + 176, "A  open", C_GREEN_DARK, 1);
	}

	/* Paging arrows. */
	{
		uint32_t left = dial_page > 0 ? C_GREEN : 0xd9e5c8;
		uint32_t right = dial_page < 1 ? C_GREEN : 0xd9e5c8;
		uint32_t left_ink = dial_page > 0 ? C_WHITE : 0xa8b89a;
		uint32_t right_ink = dial_page < 1 ? C_WHITE : 0xa8b89a;

		xenon_overlay_disc(30, 319, 20, left);
		chevron(30, 319, -1, left_ink);
		xenon_overlay_disc(1250, 319, 20, right);
		chevron(1250, 319, 1, right_ink);

		xenon_overlay_disc(628, 442, 5, dial_page == 0 ? C_GREEN : 0xc5d6b0);
		xenon_overlay_disc(652, 442, 5, dial_page == 1 ? C_GREEN : 0xc5d6b0);
	}

	/* Browser Controls card. */
	xenon_overlay_round_rect(54, 456, 1226, 662, 12, 0xe8f2da);
	xenon_overlay_round_frame(54, 456, 1226, 662, 12, 2, C_CARD_LINE);
	text_bold(76, 468, "Browser Controls", C_INK);

	for (int i = 0; i < 8; i++) {
		int col = i % 4;
		int row = i / 4;
		int bx = 104 + col * 285;
		int by = 514 + row * 72;
		control_badge(bx, by, controls[i].badge);
		text_at(bx + 34, by - 8, controls[i].text, C_INK);
	}

	text_at(76, 630, "Tip: Y searches the web with DuckDuckGo Lite, X opens options.",
		C_MUTED);
}

/* ------------------------------------------------------------------ */
/* Options menu                                                       */
/* ------------------------------------------------------------------ */

static void draw_menu(void)
{
	int x0 = 872;
	int y0 = 156;
	int x1 = 1254;
	int y1 = 620;

	xenon_overlay_round_rect(x0, y0, x1, y1, 12, C_WHITE);
	xenon_overlay_round_frame(x0, y0, x1, y1, 12, 3, C_GREEN);
	xenon_overlay_round_rect(x0 + 3, y0 + 3, x1 - 3, y0 + 46, 10, C_GREEN);
	text_bold(x0 + 18, y0 + 15, "Options", C_WHITE);

	for (int i = 0; i < MENU_ITEMS; i++) {
		int y = y0 + 58 + i * 38;
		bool selected = (i == menu_sel);
		if (selected) {
			xenon_overlay_round_rect(x0 + 12, y - 6, x1 - 12, y + 30,
						 8, C_GREEN);
			text_at(x0 + 30, y, menu_items[i], C_WHITE);
		} else {
			text_at(x0 + 30, y, menu_items[i], C_INK);
		}
	}
	text_at(x0 + 18, y1 - 26, "A select   B close", C_MUTED);
}

/* ------------------------------------------------------------------ */
/* Address / keyboard                                                 */
/* ------------------------------------------------------------------ */

static void draw_key(int x, int y, int w, int h, const char *label,
		     bool selected)
{
	xenon_overlay_round_rect(x, y, x + w, y + h, 6,
				 selected ? C_GREEN : C_WHITE);
	xenon_overlay_round_frame(x, y, x + w, y + h, 6, 2,
				  selected ? C_GREEN_DARK : C_CARD_LINE);
	text_centre(x + w / 2, y + (h - 16) / 2, label,
		    selected ? C_WHITE : C_INK, 1);
}

static void draw_url_panel(void)
{
	int panel_x0 = 90;
	int panel_x1 = 1190;
	int panel_y0 = 196;
	int panel_y1 = 664;
	char line[180];
	const char *shown;
	size_t skip;

	xenon_overlay_rect(0, CHROME_TOP, XENON_WIDTH, BOTTOM_Y, 0x2a3a22);
	xenon_overlay_round_rect(panel_x0, panel_y0, panel_x1, panel_y1, 16,
				 0xeef5e4);
	xenon_overlay_round_frame(panel_x0, panel_y0, panel_x1, panel_y1, 16,
				  3, C_GREEN);

	text_bold(panel_x0 + 24, panel_y0 + 18,
		  search_mode ? "Search the web" : "Address", C_INK);
	text_right(panel_x1 - 24, panel_y0 + 18,
		   search_mode ? "DuckDuckGo Lite" : "Type a URL", C_MUTED);

	xenon_overlay_round_rect(114, 228, 1166, 270, 10, C_WHITE);
	xenon_overlay_round_frame(114, 228, 1166, 270, 10, 2, C_CARD_LINE);

	skip = 0;
	if (url_len > 120)
		skip = url_len - 120;
	shown = url_text + skip;
	snprintf(line, sizeof(line), "%s%s_", skip > 0 ? "..." : "", shown);
	text_at(130, 241, url_len == 0 ? "Search or enter address" :
		line, url_len == 0 ? C_MUTED : C_INK);
	if (url_len == 0)
		text_at(130 + (int)strlen("Search or enter address") * 8, 241,
			"_", C_INK);

	for (int i = 0; i < OSK_KEYS; i++) {
		int col = i % OSK_COLS;
		int row = i / OSK_COLS;
		int x = OSK_X + col * (OSK_KEY_W + 4);
		int y = OSK_Y + row * (OSK_KEY_H + 4);
		char label[2] = { osk_chars[i], '\0' };

		if (caps && label[0] >= 'a' && label[0] <= 'z')
			label[0] = (char)(label[0] - 'a' + 'A');
		draw_key(x, y, OSK_KEY_W, OSK_KEY_H, label, i == key_sel);
	}

	for (int i = 0; i < OSK_ACTIONS; i++) {
		int x = 114 + i * (OSK_ACTION_W + 4);
		int y = 524;
		const char *label = osk_action_names[i];
		if (i == 4 && !looks_like_url(url_text))
			label = "Search";
		draw_key(x, y, OSK_ACTION_W, 48, label,
			 key_sel == OSK_KEYS + i);
	}

	text_at(114, 596, "A type   B cancel   X delete   Y go", C_MUTED);
}

static void draw_toast(void)
{
	int width;
	int x0;

	if (toast[0] == '\0')
		return;
	width = (int)strlen(toast) * 8 + 28;
	if (width > 640)
		width = 640;
	x0 = XENON_WIDTH - width - 16;
	xenon_overlay_round_rect(x0, BOTTOM_Y - 42, x0 + width, BOTTOM_Y - 8,
				 8, 0x2e3d24);
	xenon_overlay_round_frame(x0, BOTTOM_Y - 42, x0 + width, BOTTOM_Y - 8,
				  8, 2, C_GREEN);
	text_at(x0 + 14, BOTTOM_Y - 32, toast, C_WHITE);
}

void xenon_ui_draw(void)
{
	if (on_home() && !url_mode)
		draw_start();

	draw_top_bar();
	draw_bottom_bar();

	if (menu_mode)
		draw_menu();
	if (url_mode)
		draw_url_panel();

	draw_toast();

	if (!cursor_hidden)
		xenon_overlay_arrow(cursor_x, cursor_y);
}
