/*
 * Xenon browser chrome: a dark tab strip and toolbar, a Speed Dial home,
 * an options drawer, an on-screen keyboard and a status line.
 *
 * The page is drawn by NetSurf into the area between the fixed chrome
 * (xenon_plot_* adds XENON_CHROME_TOP). This file draws the chrome over a
 * copy of the page at each present, so moving the pointer never lays the
 * page out again.
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
#include "netsurf/keypress.h"
#include "netsurf/clipboard.h"

#include "desktop/browser_history.h"

#include "xenon/audio.h"
#include "xenon/browser.h"
#include "xenon/input.h"
#include "xenon/time.h"
#include "xenon/ui.h"
#include "xenon/xenon.h"

volatile bool xenon_quit_requested;

enum {
	TOAST_MS = 3500,
	URL_MAX = 480,

	CHROME_TOP = XENON_CHROME_TOP,
	CHROME_BOTTOM = XENON_CHROME_BOTTOM,
	BOTTOM_Y = XENON_HEIGHT - CHROME_BOTTOM,

	/* Tab strip. */
	STRIP_H = 42,
	TAB_X = 12,
	TAB_Y = 7,
	TAB_GAP = 4,
	TAB_MAX_W = 240,
	TAB_MIN_W = 120,
	TAB_LIMIT = 6,

	/* Toolbar. */
	BAR_CY = STRIP_H + 30,
	BTN_R = 21,
	ADDR_X0 = 226,
	ADDR_X1 = 1144,
	ADDR_Y0 = BAR_CY - 21,
	ADDR_Y1 = BAR_CY + 21,
	MENU_CX = 1176,
	MENU2_CX = 1236,

	/* Speed Dial. */
	DIAL_COUNT = 12,
	DIAL_COLS = 6,
	DIAL_W = 170,
	DIAL_H = 140,
	DIAL_GAP = 20,
	DIAL_X = (XENON_WIDTH - (DIAL_COLS * DIAL_W + (DIAL_COLS - 1) * DIAL_GAP)) / 2,
	DIAL_Y = 330,
	SEARCH_X0 = 320,
	SEARCH_X1 = 960,
	SEARCH_Y0 = 250,
	SEARCH_Y1 = 302,

	/* On-screen keyboard. */
	OSK_COLS = 12,
	OSK_ROWS = 4,
	OSK_KEYS = OSK_COLS * OSK_ROWS,
	OSK_ACTIONS = 6,
	OSK_KEY_W = 72,
	OSK_KEY_H = 54,
	OSK_KEY_GAP = 8,
	OSK_X = (XENON_WIDTH - (OSK_COLS * OSK_KEY_W + (OSK_COLS - 1) * OSK_KEY_GAP)) / 2,
	OSK_Y = 292,
	OSK_ACTION_Y = OSK_Y + OSK_ROWS * (OSK_KEY_H + OSK_KEY_GAP) + 4,
	OSK_ACTION_H = 52,

	/* Options drawer. */
	DRAWER_X0 = 900,
	MENU_ROW_H = 46,
};

/* Palette: a dark chrome with the console's green. */
enum {
	C_STRIP = 0x111418,
	C_BAR = 0x1e2228,
	C_BAR_LINE = 0x2c3239,
	C_FIELD = 0x2b3038,
	C_FIELD_HOVER = 0x363c45,
	C_TEXT = 0xeceff3,
	C_MUTED = 0x9aa3ad,
	C_FAINT = 0x5d6670,
	C_ACCENT = 0x7cc242,
	C_ACCENT_DARK = 0x4f8f25,
	C_HOVER = 0x343a43,
	C_CARD = 0x252a31,
	C_CARD_HOVER = 0x2f353e,
	C_HOME_TOP = 0x1b2129,
	C_HOME_BOTTOM = 0x0d1013,
	C_WHITE = 0xffffff,
	C_PAD_A = 0x6cc04a,
	C_PAD_B = 0xe2483d,
	C_PAD_X = 0x2f8fdd,
	C_PAD_Y = 0xf3c13a,
};

static const char osk_chars[OSK_KEYS] = {
	'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
	'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
	'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
	'.', '/', ':', '-', '_', '~', '?', '=', '&', '%', '@', '+',
};

static const char *const osk_action_names[OSK_ACTIONS] = {
	"Shift", "Space", "Delete", "Clear", "Go", "Cancel"
};

static int cursor_x = 640;
static int cursor_y = 300;
static bool cursor_hidden;

static bool url_mode;
static bool search_mode;
/* The keyboard types into a field of the page rather than the address. */
static bool field_mode;
/* The field is a password: its text can't be read back, so the keyboard
 * starts empty and shows dots. */
static bool field_password;
static uint64_t last_page_press;

/* The clipboard: what the page last copied, and what a paste puts in. */
static char *clip_text;
static size_t clip_len;
static bool caps;
static int key_sel;
static char url_text[URL_MAX + 1];
static size_t url_len;

static bool menu_mode;
static int menu_sel;
static bool a_consumed;

static char toast[200];
static uint64_t toast_until;
static char status_text[256];

static bool dirty;
static bool cursor_moved;
static int last_clock_min = -1;
static uint64_t last_loading_tick;
static unsigned loading_phase;

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
	uint32_t colour;
};

static const struct dial dials[DIAL_COUNT] = {
	{ "Wikipedia", "https://en.wikipedia.org/wiki/Xbox_360", ICON_GLOBE, 0x3b78c4 },
	{ "Search", "", ICON_SEARCH, 0x5aa83a },
	{ "Hacker News", "https://news.ycombinator.com/", ICON_TROPHY, 0xe8892b },
	{ "Simple Wiki", "https://simple.wikipedia.org/wiki/Web_browser", ICON_PEOPLE, 0x3f9d72 },
	{ "Pictures", "https://commons.wikimedia.org/wiki/Main_Page", ICON_PICTURE, 0x2f9fa3 },
	{ "Help", "file:///game:/help.html", ICON_HELP, 0x6b7a8a },
	{ "YouTube", "https://www.youtube.com/", ICON_PLAY, 0xe0332b },
	{ "Xbox 360", "https://en.wikipedia.org/wiki/Xbox_360", ICON_ORB, 0x6aa82f },
	{ "NetSurf", "https://www.netsurf-browser.org/", ICON_N, 0xd9692b },
	{ "HTML", "https://en.wikipedia.org/wiki/HTML", ICON_CODE, 0xd8492a },
	{ "Example", "https://example.com/", ICON_E, 0x7b6fc6 },
	{ "About", "file:///game:/about.html", ICON_INFO, 0x3b78c4 },
};

static const char *const menu_items[] = {
	"New tab", "Reload page", "Home", "Close tab",
	"Play demo.wav", "Play demo.mp3", "Sound test", "Help", "Quit"
};
#define MENU_ITEMS ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

enum { BTN_BACK, BTN_FORWARD, BTN_RELOAD, BTN_HOME, BTN_COUNT };
static const int bar_btn_x[BTN_COUNT] = { 34, 84, 134, 184 };

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int hover_key(int x, int y);

static void set_dirty(void)
{
	dirty = true;
}

void xenon_ui_chrome_changed(void)
{
	set_dirty();
}

int xenon_ui_dirty_take(void)
{
	int flags = (dirty ? XENON_UI_DIRTY_FULL : 0) |
		    (cursor_moved ? XENON_UI_DIRTY_CURSOR : 0);
	dirty = false;
	cursor_moved = false;
	return flags;
}

bool xenon_ui_cursor(int *x, int *y)
{
	*x = cursor_x;
	*y = cursor_y;
	return !cursor_hidden && !menu_mode && !url_mode;
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
		int before = hover_key(cursor_x, cursor_y);
		cursor_x = x;
		cursor_y = y;
		/* Only a change of what is under the pointer redraws the
		 * chrome; otherwise just the pointer moves. */
		if (hover_key(x, y) != before)
			set_dirty();
		else
			cursor_moved = true;
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

void xenon_ui_status(const char *text)
{
	if (text == NULL)
		text = "";
	if (strcmp(text, status_text) == 0)
		return;
	snprintf(status_text, sizeof(status_text), "%s", text);
	set_dirty();
}

void xenon_ui_init(void)
{
	url_text[0] = '\0';
	url_len = 0;
	cursor_x = 640;
	cursor_y = 360;
	key_sel = 0;
	menu_sel = 0;
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

static int tab_width(void)
{
	int n = xenon_tab_count();
	int width;

	if (n < 1)
		return TAB_MAX_W;
	width = (1000 - (n - 1) * TAB_GAP) / n;
	if (width > TAB_MAX_W)
		width = TAB_MAX_W;
	if (width < TAB_MIN_W)
		width = TAB_MIN_W;
	return width;
}

static int tab_x0(int index)
{
	return TAB_X + index * (tab_width() + TAB_GAP);
}

static int plus_center_x(void)
{
	return tab_x0(xenon_tab_count()) + 18;
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
		}
		xenon_ui_toast(xenon_audio_status());
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
		xenon_ui_status(search ? "Searching\xe2\x80\xa6" : "Loading\xe2\x80\xa6");
		set_dirty();
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
	field_mode = false;
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
	/* Drop a whole UTF-8 character. */
	do
		url_len--;
	while (url_len > 0 && ((unsigned char)url_text[url_len] & 0xc0) == 0x80);
	url_text[url_len] = '\0';
	set_dirty();
}

static void close_url_bar(void)
{
	url_mode = false;
	field_mode = false;
	field_password = false;
	cursor_hidden = false;
	set_dirty();
}

static void clip_set(const char *text, size_t len)
{
	char *copy = malloc(len + 1);

	if (copy == NULL)
		return;
	memcpy(copy, text, len);
	copy[len] = '\0';
	free(clip_text);
	clip_text = copy;
	clip_len = len;
}

static void clipboard_get(char **buffer, size_t *length)
{
	*buffer = NULL;
	*length = 0;
	if (clip_text == NULL)
		return;
	*buffer = malloc(clip_len + 1);
	if (*buffer == NULL)
		return;
	memcpy(*buffer, clip_text, clip_len + 1);
	*length = clip_len;
}

static void clipboard_set(const char *buffer, size_t length,
			  nsclipboard_styles styles[], int n_styles)
{
	(void)styles;
	(void)n_styles;
	clip_set(buffer, length);
}

static struct gui_clipboard_table clipboard_table = {
	.get = clipboard_get,
	.set = clipboard_set,
};

struct gui_clipboard_table *xenon_clipboard_table = &clipboard_table;

/* Whether text is nothing but the bullets a password field shows. */
static bool all_bullets(const char *text, size_t len)
{
	if (len == 0 || len % 3 != 0)
		return false;
	for (size_t i = 0; i < len; i += 3)
		if (memcmp(text + i, "\xe2\x80\xa2", 3) != 0)
			return false;
	return true;
}

/* The focused field's text, through select-all and copy. */
static size_t field_read(char *out, size_t size)
{
	size_t n = 0;

	clip_set("", 0);
	browser_window_key_press(xenon_bw, NS_KEY_SELECT_ALL);
	browser_window_key_press(xenon_bw, NS_KEY_COPY_SELECTION);
	browser_window_key_press(xenon_bw, NS_KEY_TEXT_END);
	if (clip_text != NULL) {
		n = clip_len < size - 1 ? clip_len : size - 1;
		memcpy(out, clip_text, n);
	}
	out[n] = '\0';
	return n;
}

/* Done: put the keyboard's text in the field in place of what was there. */
static void field_commit(void)
{
	if (xenon_bw == NULL)
		return;
	browser_window_key_press(xenon_bw, NS_KEY_SELECT_ALL);
	if (url_len > 0) {
		clip_set(url_text, url_len);
		browser_window_key_press(xenon_bw, NS_KEY_PASTE);
	} else {
		browser_window_key_press(xenon_bw, NS_KEY_DELETE_LEFT);
	}
	browser_window_key_press(xenon_bw, NS_KEY_TEXT_END);
	clip_set("", 0);
}

/* Go: an address or search for the address bar. For a page field, Done:
 * the text goes in and the form stays as it is, so the next field can be
 * filled before the page's own button sends it. */
static void keyboard_go(void)
{
	if (field_mode) {
		field_commit();
		close_url_bar();
		return;
	}
	go_url(url_text);
}

void xenon_ui_caret(bool shown)
{
	if (!shown || url_mode || menu_mode || xenon_bw == NULL)
		return;
	if (now_ms() - last_page_press > 1500)
		return;
	last_page_press = 0;

	/* A field was just picked: start from its text. A password field
	 * copies as bullets; an empty one shows its kind once a character is
	 * in it, so try one and take it out again. */
	url_len = field_read(url_text, sizeof(url_text));
	field_password = all_bullets(url_text, url_len);
	if (url_len == 0) {
		char probe[16];
		browser_window_key_press(xenon_bw, 'x');
		field_password = all_bullets(probe, field_read(probe, sizeof(probe)));
		browser_window_key_press(xenon_bw, NS_KEY_SELECT_ALL);
		browser_window_key_press(xenon_bw, NS_KEY_DELETE_LEFT);
	}
	if (field_password) {
		url_len = 0;
		url_text[0] = '\0';
	}
	clip_set("", 0);

	toast[0] = '\0';
	search_mode = false;
	field_mode = true;
	url_mode = true;
	cursor_hidden = true;
	key_sel = 0;
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
		keyboard_go();
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
			if (row > OSK_ROWS - 1) {
				/* Down from the last row reaches the actions. */
				int action = col * OSK_ACTIONS / OSK_COLS;
				key_sel = OSK_KEYS + action;
				set_dirty();
				return;
			}
			key_sel = row * OSK_COLS + col;
		} else if (dx != 0) {
			col = (col + dx + OSK_COLS) % OSK_COLS;
			key_sel = row * OSK_COLS + col;
		}
	} else {
		int action = key_sel - OSK_KEYS;
		if (dy < 0) {
			int c = action * OSK_COLS / OSK_ACTIONS + 1;
			key_sel = (OSK_ROWS - 1) * OSK_COLS + (c < OSK_COLS ? c : OSK_COLS - 1);
		} else if (dx != 0) {
			action = (action + dx + OSK_ACTIONS) % OSK_ACTIONS;
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
	int col = index % DIAL_COLS;
	int row = index / DIAL_COLS;
	*x0 = DIAL_X + col * (DIAL_W + DIAL_GAP);
	*y0 = DIAL_Y + row * (DIAL_H + DIAL_GAP);
	*x1 = *x0 + DIAL_W;
	*y1 = *y0 + DIAL_H;
}

/* What the pointer is over, as far as drawing goes: two positions with the
 * same key draw the same chrome. */
static int hover_key(int x, int y)
{
	int n = xenon_tab_count();
	int width = tab_width();

	if (url_mode || menu_mode)
		return 0;
	if (y < STRIP_H) {
		for (int i = 0; i < n; i++) {
			int x0 = tab_x0(i);
			if (point_in(x, y, x0, TAB_Y, x0 + width, STRIP_H))
				return 100 + i * 2 +
				       (over_circle(x, y, x0 + width - 18, TAB_Y + 18, 12) ? 1 : 0);
		}
		return over_circle(x, y, plus_center_x(), STRIP_H / 2 + 3, 18) ? 200 : 1;
	}
	if (y < CHROME_TOP) {
		for (int i = 0; i < BTN_COUNT; i++)
			if (over_circle(x, y, bar_btn_x[i], BAR_CY, BTN_R + 4))
				return 300 + i;
		if (point_in(x, y, ADDR_X0, ADDR_Y0, ADDR_X1, ADDR_Y1))
			return 310;
		if (over_circle(x, y, MENU_CX, BAR_CY, BTN_R + 4))
			return 311;
		if (over_circle(x, y, MENU2_CX, BAR_CY, BTN_R + 4))
			return 312;
		return 2;
	}
	if (y >= BOTTOM_Y)
		return 3;
	if (on_home()) {
		if (point_in(x, y, SEARCH_X0, SEARCH_Y0, SEARCH_X1, SEARCH_Y1))
			return 400;
		for (int i = 0; i < DIAL_COUNT; i++) {
			int x0, y0, x1, y1;
			dial_rect(i, &x0, &y0, &x1, &y1);
			if (point_in(x, y, x0, y0, x1, y1))
				return 500 + i;
		}
	}
	return 4;
}

static void new_tab(void)
{
	if (xenon_tab_count() >= TAB_LIMIT)
		xenon_ui_toast("Six tabs is the limit");
	else
		xenon_tab_new();
}

static void open_menu(void)
{
	toast[0] = '\0';
	menu_mode = true;
	menu_sel = 0;
	set_dirty();
}

static void handle_tabs_click(int x, int y)
{
	int n = xenon_tab_count();
	int width = tab_width();

	for (int i = 0; i < n; i++) {
		int x0 = tab_x0(i);
		if (x < x0 || x >= x0 + width || y < TAB_Y || y >= STRIP_H)
			continue;
		if (x >= x0 + width - 32 && i == active_tab()) {
			xenon_tab_close(i);
			if (xenon_tab_count() == 0)
				xenon_tab_new();
		} else {
			xenon_tab_activate(i);
		}
		return;
	}

	if (over_circle(x, y, plus_center_x(), STRIP_H / 2 + 3, 18))
		new_tab();
}

static void handle_chrome_click(int x, int y)
{
	if (y < STRIP_H) {
		handle_tabs_click(x, y);
		return;
	}
	if (point_in(x, y, ADDR_X0, ADDR_Y0, ADDR_X1, ADDR_Y1)) {
		open_url_bar(false);
		return;
	}
	if (over_circle(x, y, MENU_CX, BAR_CY, BTN_R + 4)) {
		open_url_bar(true);
		return;
	}
	if (over_circle(x, y, MENU2_CX, BAR_CY, BTN_R + 4)) {
		open_menu();
		return;
	}

	for (int i = 0; i < BTN_COUNT; i++) {
		if (!over_circle(x, y, bar_btn_x[i], BAR_CY, BTN_R + 4))
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
		}
		return;
	}
}

static void handle_dial_click(int x, int y)
{
	if (point_in(x, y, SEARCH_X0, SEARCH_Y0, SEARCH_X1, SEARCH_Y1)) {
		open_url_bar(true);
		return;
	}

	for (int i = 0; i < DIAL_COUNT; i++) {
		int x0, y0, x1, y1;
		dial_rect(i, &x0, &y0, &x1, &y1);
		if (!point_in(x, y, x0, y0, x1, y1))
			continue;
		if (dials[i].url[0] == '\0')
			open_url_bar(true);
		else
			go_url(dials[i].url);
		return;
	}
}

static void menu_activate(void)
{
	int index = active_tab();

	switch (menu_sel) {
	case 0:
		new_tab();
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
			xenon_ui_toast("Playing a 660 Hz test tone");
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

	/* The progress bar moves at 10 frames a second while loading. */
	if (xenon_is_loading() && now - last_loading_tick > 100) {
		last_loading_tick = now;
		loading_phase++;
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
			keyboard_go();
		if (xenon_input_pressed(XB_B))
			close_url_bar();
		if (xenon_input_pressed(XB_LB)) {
			caps = !caps;
			set_dirty();
		}
		if (xenon_input_pressed(XB_RB))
			url_insert(' ');
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
		open_menu();
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
	/* A box under the pointer that scrolls takes the scroll first; the
	 * page itself scrolls in the frontend. */
	if (!on_home()) {
		int page = XENON_PAGE_HEIGHT - 70;
		int sdx = 0;
		int sdy = 0;

		if (xenon_input_pressed(XB_LB))
			sdy = -page;
		if (xenon_input_pressed(XB_RB))
			sdy = page;
		if (sdy != 0 &&
		    !browser_window_scroll_at_point(xenon_bw, cursor_x,
						    cursor_y - CHROME_TOP, 0, sdy))
			xenon_scroll_by(0, sdy);

		xenon_input_scroll(&sdx, &sdy);
		if (sdx != 0 || sdy != 0)
			xenon_scroll_by(sdx, sdy);
	}

	if (xenon_input_pressed(XB_A)) {
		a_consumed = true;
		if (cursor_y < CHROME_TOP)
			handle_chrome_click(cursor_x, cursor_y);
		else if (cursor_y >= BOTTOM_Y)
			; /* the status line has nothing to press */
		else if (on_home())
			handle_dial_click(cursor_x, cursor_y);
		else {
			a_consumed = false;
			last_page_press = now_ms();
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

static int text(int x, int y, const char *s, uint32_t colour, int px)
{
	return xenon_overlay_text(x, y, s, colour, px, XENON_TEXT_REGULAR);
}

static int text_b(int x, int y, const char *s, uint32_t colour, int px)
{
	return xenon_overlay_text(x, y, s, colour, px, XENON_TEXT_BOLD);
}

static void text_centre(int cx, int y, const char *s, uint32_t colour, int px,
			int weight)
{
	int width = xenon_overlay_text_width(s, px, weight);
	xenon_overlay_text(cx - width / 2, y, s, colour, px, weight);
}

static void text_right(int right, int y, const char *s, uint32_t colour, int px)
{
	int width = xenon_overlay_text_width(s, px, XENON_TEXT_REGULAR);
	xenon_overlay_text(right - width, y, s, colour, px, XENON_TEXT_REGULAR);
}

static uint32_t pad_colour(const char *label)
{
	switch (label[0]) {
	case 'A':
		return C_PAD_A;
	case 'B':
		return label[1] == '\0' ? C_PAD_B : C_FIELD;
	case 'X':
		return C_PAD_X;
	case 'Y':
		return C_PAD_Y;
	default:
		return C_FIELD;
	}
}

/* A controller button: a coloured disc for A/B/X/Y, a pill for the rest.
 * Returns the right edge. */
static int pad_glyph(int x, int cy, const char *label)
{
	uint32_t fill = pad_colour(label);
	int len = (int)strlen(label);

	if (len == 1) {
		xenon_overlay_disc(x + 11, cy, 11, fill);
		text_centre(x + 11, cy + 5, label,
			    label[0] == 'Y' ? 0x2a2208 : C_WHITE, 14, XENON_TEXT_BOLD);
		return x + 22;
	}
	{
		int w = xenon_overlay_text_width(label, 12, XENON_TEXT_BOLD) + 16;
		xenon_overlay_round_rect(x, cy - 10, x + w, cy + 10, 10, fill);
		text_centre(x + w / 2, cy + 4, label, C_TEXT, 12, XENON_TEXT_BOLD);
		return x + w;
	}
}

/* A hint: a button glyph followed by what it does. Returns the right edge. */
static int hint(int x, int cy, const char *button, const char *what, uint32_t ink)
{
	x = pad_glyph(x, cy, button) + 7;
	return text(x, cy + 5, what, ink, 14) + 18;
}

static int hint_width(const char *button, const char *what)
{
	int w = strlen(button) == 1 ? 22 :
		xenon_overlay_text_width(button, 12, XENON_TEXT_BOLD) + 16;
	return w + 7 + xenon_overlay_text_width(what, 14, XENON_TEXT_REGULAR) + 18;
}

static void chevron(int cx, int cy, int dir, uint32_t colour)
{
	int tip = cx + 5 * dir;
	int tail = cx - 5 * dir;

	xenon_overlay_line(tail, cy - 9, tip, cy, 3, colour);
	xenon_overlay_line(tip, cy, tail, cy + 9, 3, colour);
}

static void draw_search_icon(int cx, int cy, int size, uint32_t colour)
{
	int r = size * 9 / 16;
	xenon_overlay_ring(cx - size / 8, cy - size / 8, r, size >= 20 ? 3 : 2, colour);
	xenon_overlay_line(cx + r / 2 + 1, cy + r / 2 + 1, cx + size / 2 + 2,
			   cy + size / 2 + 2, size >= 20 ? 4 : 3, colour);
}

static void draw_lock(int x, int cy, uint32_t colour)
{
	xenon_overlay_ring(x + 6, cy - 4, 5, 2, colour);
	xenon_overlay_round_rect(x, cy - 3, x + 13, cy + 7, 2, colour);
}

static void draw_bar_button(int cx, int cy, int id, bool enabled)
{
	bool hover = !url_mode && !menu_mode && over_circle(cursor_x, cursor_y, cx, cy, BTN_R + 4);
	uint32_t ink = enabled ? C_TEXT : C_FAINT;

	if (hover)
		xenon_overlay_disc(cx, cy, BTN_R, C_HOVER);

	switch (id) {
	case BTN_BACK:
		xenon_overlay_line(cx - 9, cy, cx + 9, cy, 3, ink);
		chevron(cx - 4, cy, -1, ink);
		break;
	case BTN_FORWARD:
		xenon_overlay_line(cx - 9, cy, cx + 9, cy, 3, ink);
		chevron(cx + 4, cy, 1, ink);
		break;
	case BTN_RELOAD:
		if (xenon_is_loading()) {
			/* Stop: an X while loading. */
			xenon_overlay_line(cx - 7, cy - 7, cx + 7, cy + 7, 3, ink);
			xenon_overlay_line(cx - 7, cy + 7, cx + 7, cy - 7, 3, ink);
		} else {
			xenon_overlay_ring(cx, cy, 9, 3, ink);
			xenon_overlay_rect(cx + 1, cy - 11, cx + 11, cy, hover ? C_HOVER : C_BAR);
			xenon_overlay_triangle(cx + 1, cy - 14, cx + 1, cy - 4, cx + 9, cy - 9, ink);
		}
		break;
	case BTN_HOME:
		xenon_overlay_triangle(cx - 11, cy - 1, cx, cy - 11, cx + 11, cy - 1, ink);
		xenon_overlay_round_rect(cx - 8, cy - 2, cx + 8, cy + 10, 1, ink);
		xenon_overlay_rect(cx - 2, cy + 3, cx + 2, cy + 10, hover ? C_HOVER : C_BAR);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Chrome                                                             */
/* ------------------------------------------------------------------ */

static void draw_tabs(void)
{
	int active = active_tab();
	int n = xenon_tab_count();
	int width = tab_width();
	char clock_text[24];

	xenon_overlay_rect(0, 0, XENON_WIDTH, STRIP_H, C_STRIP);

	for (int i = 0; i < n; i++) {
		int x0 = tab_x0(i);
		int x1 = x0 + width;
		bool is_active = i == active;
		bool hover = !url_mode && !menu_mode &&
			     point_in(cursor_x, cursor_y, x0, TAB_Y, x1, STRIP_H);
		struct gui_window *g = xenon_tab_at(i);
		int title_x = x0 + 14;

		if (is_active) {
			xenon_overlay_round_rect(x0, TAB_Y, x1, STRIP_H + 10, 10, C_BAR);
		} else if (hover) {
			xenon_overlay_round_rect(x0 + 2, TAB_Y + 3, x1 - 2, STRIP_H - 4, 8,
						 0x1d2126);
		} else if (i + 1 != active && i + 1 < n) {
			xenon_overlay_rect(x1 + TAB_GAP / 2 - 1, TAB_Y + 9, x1 + TAB_GAP / 2,
					   STRIP_H - 9, 0x2a3037);
		}

		/* A loading tab shows a spinning dot pair; a loaded one a dot. */
		if (g != NULL && g->loading) {
			int phase = (int)(loading_phase % 8);
			static const int ox[8] = { 0, 4, 6, 4, 0, -4, -6, -4 };
			static const int oy[8] = { -6, -4, 0, 4, 6, 4, 0, -4 };
			xenon_overlay_ring(x0 + 22, TAB_Y + 18, 7, 2, C_FAINT);
			xenon_overlay_disc(x0 + 22 + ox[phase], TAB_Y + 18 + oy[phase], 2, C_ACCENT);
		} else {
			xenon_overlay_disc(x0 + 22, TAB_Y + 18, 6,
					   is_active ? C_ACCENT : C_FAINT);
		}
		title_x = x0 + 36;

		xenon_overlay_text_fit(title_x, TAB_Y + 23, xenon_tab_title(i),
				       is_active ? C_TEXT : C_MUTED, 15, XENON_TEXT_REGULAR,
				       x1 - title_x - (is_active ? 34 : 12));

		if (is_active) {
			int cx = x1 - 18;
			int cy = TAB_Y + 18;
			if (over_circle(cursor_x, cursor_y, cx, cy, 12))
				xenon_overlay_disc(cx, cy, 11, C_HOVER);
			xenon_overlay_line(cx - 4, cy - 4, cx + 4, cy + 4, 2, C_MUTED);
			xenon_overlay_line(cx - 4, cy + 4, cx + 4, cy - 4, 2, C_MUTED);
		}
	}

	/* New tab. */
	{
		int px = plus_center_x();
		int py = STRIP_H / 2 + 3;
		if (over_circle(cursor_x, cursor_y, px, py, 18))
			xenon_overlay_disc(px, py, 15, C_HOVER);
		xenon_overlay_line(px - 7, py, px + 7, py, 2, C_TEXT);
		xenon_overlay_line(px, py - 7, px, py + 7, 2, C_TEXT);
	}

	xenon_clock_text(clock_text, sizeof(clock_text));
	text_right(XENON_WIDTH - 18, 28, clock_text, C_MUTED, 15);
}

static void draw_toolbar(void)
{
	int active = active_tab();
	const char *current = active >= 0 ? xenon_tab_url(active) : "";
	bool home = current[0] == '\0' || strstr(current, "/home.html") != NULL;
	bool hover = !url_mode && !menu_mode &&
		     point_in(cursor_x, cursor_y, ADDR_X0, ADDR_Y0, ADDR_X1, ADDR_Y1);
	int text_x = ADDR_X0 + 22;

	xenon_overlay_rect(0, STRIP_H, XENON_WIDTH, CHROME_TOP, C_BAR);

	draw_bar_button(bar_btn_x[BTN_BACK], BAR_CY, BTN_BACK,
			xenon_bw != NULL && browser_window_back_available(xenon_bw));
	draw_bar_button(bar_btn_x[BTN_FORWARD], BAR_CY, BTN_FORWARD,
			xenon_bw != NULL && browser_window_forward_available(xenon_bw));
	draw_bar_button(bar_btn_x[BTN_RELOAD], BAR_CY, BTN_RELOAD, xenon_bw != NULL);
	draw_bar_button(bar_btn_x[BTN_HOME], BAR_CY, BTN_HOME, true);

	/* Address pill. */
	xenon_overlay_round_rect(ADDR_X0, ADDR_Y0, ADDR_X1, ADDR_Y1, 21,
				 hover ? C_FIELD_HOVER : C_FIELD);
	if (home) {
		draw_search_icon(text_x + 7, BAR_CY, 16, C_MUTED);
		text(text_x + 26, BAR_CY + 6, "Search the web or type an address",
		     C_MUTED, 17);
	} else {
		bool secure = strncmp(current, "https://", 8) == 0;
		const char *rest = current;
		char host[256];
		size_t hl;

		if (secure) {
			draw_lock(text_x, BAR_CY, C_MUTED);
			rest += 8;
		} else if (strncmp(current, "http://", 7) == 0) {
			rest += 7;
		}
		text_x += secure ? 24 : 0;

		/* The host bright, the path dimmed, as desktop browsers do. */
		hl = strcspn(rest, "/?#");
		if (hl >= sizeof(host))
			hl = sizeof(host) - 1;
		memcpy(host, rest, hl);
		host[hl] = '\0';
		{
			int end = text(text_x, BAR_CY + 6, host, C_TEXT, 17);
			if (rest[hl] != '\0')
				xenon_overlay_text_fit(end, BAR_CY + 6, rest + hl, C_MUTED, 17,
						       XENON_TEXT_REGULAR, ADDR_X1 - 60 - end);
		}
	}
	if (hover) {
		pad_glyph(ADDR_X1 - 34, BAR_CY, "A");
	}

	/* Search and menu. */
	if (!url_mode && !menu_mode && over_circle(cursor_x, cursor_y, MENU_CX, BAR_CY, BTN_R + 4))
		xenon_overlay_disc(MENU_CX, BAR_CY, BTN_R, C_HOVER);
	draw_search_icon(MENU_CX, BAR_CY, 18, C_TEXT);
	if (!url_mode && !menu_mode && over_circle(cursor_x, cursor_y, MENU2_CX, BAR_CY, BTN_R + 4))
		xenon_overlay_disc(MENU2_CX, BAR_CY, BTN_R, C_HOVER);
	for (int i = -1; i <= 1; i++)
		xenon_overlay_disc(MENU2_CX, BAR_CY + i * 7, 2, C_TEXT);

	/* Progress: a sweep along the toolbar's foot while loading. */
	xenon_overlay_rect(0, CHROME_TOP - 1, XENON_WIDTH, CHROME_TOP, C_BAR_LINE);
	if (xenon_is_loading()) {
		int w = 260;
		int x = (int)((loading_phase * 34) % (XENON_WIDTH + w)) - w;
		xenon_overlay_rect(x < 0 ? 0 : x, CHROME_TOP - 3, x + w, CHROME_TOP,
				   C_ACCENT);
	}
}

static void draw_status_bar(void)
{
	static const struct {
		const char *button;
		const char *what;
	} hints[] = {
		{ "A", "Open" }, { "B", "Back" }, { "X", "Menu" },
		{ "Y", "Search" }, { "RS", "Scroll" },
	};
	int total = 0;
	int x;

	xenon_overlay_rect(0, BOTTOM_Y, XENON_WIDTH, XENON_HEIGHT, C_STRIP);

	for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++)
		total += hint_width(hints[i].button, hints[i].what);
	x = XENON_WIDTH - 10 - total;
	for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++)
		x = hint(x, BOTTOM_Y + CHROME_BOTTOM / 2, hints[i].button, hints[i].what,
			 C_MUTED);

	if (status_text[0] != '\0')
		xenon_overlay_text_fit(16, BOTTOM_Y + 20, status_text, C_MUTED, 14,
				       XENON_TEXT_REGULAR, XENON_WIDTH - total - 60);
}

/* ------------------------------------------------------------------ */
/* Speed dial                                                         */
/* ------------------------------------------------------------------ */

static void draw_icon(dial_icon icon, int cx, int cy, uint32_t colour)
{
	const uint32_t w = C_WHITE;

	xenon_overlay_disc(cx, cy, 30, colour);

	switch (icon) {
	case ICON_GLOBE:
		xenon_overlay_ring(cx, cy, 17, 2, w);
		xenon_overlay_ring(cx, cy, 9, 2, w);
		xenon_overlay_line(cx - 17, cy, cx + 17, cy, 2, w);
		xenon_overlay_line(cx, cy - 17, cx, cy + 17, 2, w);
		break;
	case ICON_SEARCH:
		draw_search_icon(cx, cy, 26, w);
		break;
	case ICON_TROPHY:
		xenon_overlay_round_rect(cx - 11, cy - 15, cx + 11, cy + 1, 5, w);
		xenon_overlay_ring(cx - 13, cy - 7, 6, 2, w);
		xenon_overlay_ring(cx + 13, cy - 7, 6, 2, w);
		xenon_overlay_rect(cx - 2, cy + 1, cx + 2, cy + 9, w);
		xenon_overlay_round_rect(cx - 10, cy + 9, cx + 10, cy + 15, 2, w);
		break;
	case ICON_PEOPLE:
		xenon_overlay_disc(cx - 6, cy - 7, 6, w);
		xenon_overlay_disc(cx + 9, cy - 5, 5, w);
		xenon_overlay_round_rect(cx - 16, cy + 1, cx + 3, cy + 15, 7, w);
		xenon_overlay_round_rect(cx + 4, cy + 4, cx + 16, cy + 15, 5, w);
		break;
	case ICON_PICTURE:
		xenon_overlay_round_rect(cx - 17, cy - 13, cx + 17, cy + 13, 3, w);
		xenon_overlay_triangle(cx - 13, cy + 9, cx - 3, cy - 5, cx + 6, cy + 9, colour);
		xenon_overlay_triangle(cx + 1, cy + 9, cx + 8, cy, cx + 14, cy + 9, colour);
		xenon_overlay_disc(cx + 9, cy - 6, 3, 0xf3c13a);
		break;
	case ICON_HELP:
		text_centre(cx, cy + 10, "?", w, 30, XENON_TEXT_BOLD);
		break;
	case ICON_PLAY:
		xenon_overlay_triangle(cx - 7, cy - 11, cx - 7, cy + 11, cx + 12, cy, w);
		break;
	case ICON_ORB:
		xenon_overlay_ring(cx, cy, 17, 3, w);
		xenon_overlay_line(cx - 8, cy - 8, cx + 8, cy + 8, 4, w);
		xenon_overlay_line(cx - 8, cy + 8, cx + 8, cy - 8, 4, w);
		break;
	case ICON_N:
		text_centre(cx, cy + 10, "N", w, 30, XENON_TEXT_BOLD);
		break;
	case ICON_CODE:
		text_centre(cx, cy + 8, "</>", w, 22, XENON_TEXT_BOLD);
		break;
	case ICON_E:
		text_centre(cx, cy + 9, "e", w, 32, XENON_TEXT_BOLD);
		break;
	case ICON_INFO:
		text_centre(cx, cy + 10, "i", w, 30, XENON_TEXT_BOLD);
		break;
	}
}

static void draw_home(void)
{
	char clock_text[24];
	bool search_hover = !menu_mode &&
			    point_in(cursor_x, cursor_y, SEARCH_X0, SEARCH_Y0, SEARCH_X1, SEARCH_Y1);

	xenon_overlay_gradient(0, CHROME_TOP, XENON_WIDTH, BOTTOM_Y, C_HOME_TOP, C_HOME_BOTTOM);

	/* The time, large, as the dashboard does. */
	xenon_clock_text(clock_text, sizeof(clock_text));
	text_centre(XENON_WIDTH / 2, 200, clock_text, C_TEXT, 58, XENON_TEXT_REGULAR);
	text_centre(XENON_WIDTH / 2, 232, "NetSurf 360", C_MUTED, 17, XENON_TEXT_REGULAR);

	/* Search pill. */
	xenon_overlay_shadow(SEARCH_X0, SEARCH_Y0, SEARCH_X1, SEARCH_Y1, 26, 6, 70);
	xenon_overlay_round_rect(SEARCH_X0, SEARCH_Y0, SEARCH_X1, SEARCH_Y1, 26,
				 search_hover ? C_FIELD_HOVER : C_FIELD);
	if (search_hover)
		xenon_overlay_round_frame(SEARCH_X0, SEARCH_Y0, SEARCH_X1, SEARCH_Y1, 26, 2,
					  C_ACCENT);
	draw_search_icon(SEARCH_X0 + 32, (SEARCH_Y0 + SEARCH_Y1) / 2, 18, C_MUTED);
	text(SEARCH_X0 + 58, (SEARCH_Y0 + SEARCH_Y1) / 2 + 7,
	     "Search the web or type an address", C_MUTED, 19);
	pad_glyph(SEARCH_X1 - 42, (SEARCH_Y0 + SEARCH_Y1) / 2, "Y");

	for (int i = 0; i < DIAL_COUNT; i++) {
		int x0, y0, x1, y1;
		int cx;
		bool hover;

		dial_rect(i, &x0, &y0, &x1, &y1);
		hover = !menu_mode && point_in(cursor_x, cursor_y, x0, y0, x1, y1);
		cx = (x0 + x1) / 2;

		if (hover) {
			y0 -= 4;
			y1 -= 4;
			xenon_overlay_shadow(x0, y0, x1, y1, 16, 10, 120);
		}
		xenon_overlay_round_rect(x0, y0, x1, y1, 16, hover ? C_CARD_HOVER : C_CARD);
		if (hover)
			xenon_overlay_round_frame(x0, y0, x1, y1, 16, 3, C_ACCENT);

		draw_icon(dials[i].icon, cx, y0 + 56, dials[i].colour);
		xenon_overlay_text_fit(cx - xenon_overlay_text_width(dials[i].label, 16,
								    XENON_TEXT_REGULAR) / 2,
				       y0 + 116, dials[i].label, hover ? C_TEXT : 0xc9d0d8,
				       16, XENON_TEXT_REGULAR, DIAL_W - 16);
	}

	text_centre(XENON_WIDTH / 2, 668,
		    "Move with the left stick, press A to open. BACK returns here.",
		    C_FAINT, 14, XENON_TEXT_REGULAR);
}

/* ------------------------------------------------------------------ */
/* Options drawer                                                     */
/* ------------------------------------------------------------------ */

static void draw_menu(void)
{
	int x0 = DRAWER_X0;
	int y0 = CHROME_TOP;
	int y1 = BOTTOM_Y;

	xenon_overlay_rect_a(0, y0, x0, y1, 0x000000, 120);
	xenon_overlay_rect_a(x0 - 10, y0, x0, y1, 0x000000, 60);
	xenon_overlay_rect(x0, y0, XENON_WIDTH, y1, C_BAR);
	xenon_overlay_rect(x0, y0, x0 + 1, y1, C_BAR_LINE);

	text_b(x0 + 28, y0 + 46, "Menu", C_TEXT, 24);

	for (int i = 0; i < MENU_ITEMS; i++) {
		int y = y0 + 72 + i * MENU_ROW_H;
		bool selected = i == menu_sel;

		if (selected) {
			xenon_overlay_round_rect(x0 + 14, y, XENON_WIDTH - 14, y + MENU_ROW_H - 6,
						 10, C_ACCENT_DARK);
			xenon_overlay_rect(x0 + 14, y + 8, x0 + 18, y + MENU_ROW_H - 14,
					   C_ACCENT);
		}
		text(x0 + 32, y + 26, menu_items[i], selected ? C_WHITE : C_TEXT, 18);
		if (i == 4 || i == 8)
			xenon_overlay_rect(x0 + 28, y - 3, XENON_WIDTH - 28, y - 2, C_BAR_LINE);
	}

	{
		int x = x0 + 28;
		x = hint(x, y1 - 26, "A", "Select", C_MUTED);
		hint(x, y1 - 26, "B", "Close", C_MUTED);
	}
}

/* ------------------------------------------------------------------ */
/* Address / keyboard                                                 */
/* ------------------------------------------------------------------ */

static void draw_key(int x, int y, int w, int h, const char *label,
		     bool selected, bool special)
{
	uint32_t fill = selected ? C_ACCENT : (special ? 0x323841 : C_FIELD);

	if (selected)
		xenon_overlay_shadow(x, y, x + w, y + h, 10, 6, 90);
	xenon_overlay_round_rect(x, y, x + w, y + h, 10, fill);
	text_centre(x + w / 2, y + h / 2 + 7, label,
		    selected ? 0x10220a : C_TEXT, special ? 17 : 21,
		    selected ? XENON_TEXT_BOLD : XENON_TEXT_REGULAR);
}

static void draw_url_panel(void)
{
	int panel_x0 = OSK_X - 40;
	int panel_x1 = XENON_WIDTH - OSK_X + 40;
	int panel_y0 = 140;
	int panel_y1 = 680;
	int field_y0 = 206;
	int field_y1 = 258;
	int tx;

	xenon_overlay_rect_a(0, 0, XENON_WIDTH, XENON_HEIGHT, 0x000000, 150);
	xenon_overlay_shadow(panel_x0, panel_y0, panel_x1, panel_y1, 22, 14, 150);
	xenon_overlay_round_rect(panel_x0, panel_y0, panel_x1, panel_y1, 22, C_BAR);

	text_b(panel_x0 + 40, panel_y0 + 44,
	       field_mode ? (field_password ? "Password" : "Type into the page") :
	       search_mode ? "Search the web" : "Go to address", C_TEXT, 22);
	text_right(panel_x1 - 40, panel_y0 + 42,
		   search_mode ? "DuckDuckGo Lite" : "", C_MUTED, 15);

	xenon_overlay_round_rect(OSK_X, field_y0, XENON_WIDTH - OSK_X, field_y1, 26,
				 C_FIELD);
	xenon_overlay_round_frame(OSK_X, field_y0, XENON_WIDTH - OSK_X, field_y1, 26, 2,
				  C_ACCENT);
	draw_search_icon(OSK_X + 30, (field_y0 + field_y1) / 2, 18, C_MUTED);

	tx = OSK_X + 56;
	if (url_len == 0) {
		text(tx, field_y1 - 19, "Type with the keyboard below", C_FAINT, 20);
		xenon_overlay_rect(tx - 2, field_y0 + 13, tx, field_y1 - 13, C_ACCENT);
	} else {
		/* Show the end of long text so the caret stays visible; a
		 * password as dots. */
		static char dots[URL_MAX * 3 + 1];
		const char *shown = url_text;
		if (field_password) {
			size_t n = 0;
			for (size_t i = 0; i < url_len && n + 3 < sizeof(dots); i++) {
				memcpy(dots + n, "\xe2\x80\xa2", 3);
				n += 3;
			}
			dots[n] = '\0';
			shown = dots;
		}
		int avail = XENON_WIDTH - OSK_X - 30 - tx;
		while (*shown != '\0' &&
		       xenon_overlay_text_width(shown, 20, XENON_TEXT_REGULAR) > avail)
			shown++;
		int end = text(tx, field_y1 - 19, shown, C_TEXT, 20);
		xenon_overlay_rect(end + 1, field_y0 + 13, end + 3, field_y1 - 13, C_ACCENT);
	}

	for (int i = 0; i < OSK_KEYS; i++) {
		int col = i % OSK_COLS;
		int row = i / OSK_COLS;
		int x = OSK_X + col * (OSK_KEY_W + OSK_KEY_GAP);
		int y = OSK_Y + row * (OSK_KEY_H + OSK_KEY_GAP);
		char label[2] = { osk_chars[i], '\0' };

		if (caps && label[0] >= 'a' && label[0] <= 'z')
			label[0] = (char)(label[0] - 'a' + 'A');
		draw_key(x, y, OSK_KEY_W, OSK_KEY_H, label, i == key_sel, false);
	}

	{
		int total = OSK_COLS * OSK_KEY_W + (OSK_COLS - 1) * OSK_KEY_GAP;
		int w = (total - (OSK_ACTIONS - 1) * OSK_KEY_GAP) / OSK_ACTIONS;
		for (int i = 0; i < OSK_ACTIONS; i++) {
			int x = OSK_X + i * (w + OSK_KEY_GAP);
			const char *label = osk_action_names[i];
			if (i == 0 && caps)
				label = "SHIFT";
			if (i == 4 && field_mode)
				label = "Done";
			else if (i == 4 && !looks_like_url(url_text))
				label = "Search";
			draw_key(x, OSK_ACTION_Y, w, OSK_ACTION_H, label,
				 key_sel == OSK_KEYS + i, true);
		}
	}

	{
		int x = OSK_X;
		int cy = panel_y1 - 24;
		x = hint(x, cy, "A", "Type", C_MUTED);
		x = hint(x, cy, "X", "Delete", C_MUTED);
		x = hint(x, cy, "Y", field_mode ? "Done" : "Go", C_MUTED);
		x = hint(x, cy, "B", "Cancel", C_MUTED);
		x = hint(x, cy, "LB", "Shift", C_MUTED);
		hint(x, cy, "RB", "Space", C_MUTED);
	}
}

/* A thin thumb at the right edge of the page shows where the view is. */
static void draw_scroll_bar(void)
{
	int y, view, total;
	int track = BOTTOM_Y - CHROME_TOP - 8;
	int thumb, top;

	if (!xenon_scroll_info(&y, &view, &total))
		return;
	thumb = track * view / total;
	if (thumb < 32)
		thumb = 32;
	top = CHROME_TOP + 4 + (int)((int64_t)(track - thumb) * y / (total - view));
	xenon_overlay_round_rect_a(XENON_WIDTH - 9, top, XENON_WIDTH - 3, top + thumb, 3,
				   0x202428, 150);
}

static void draw_toast(void)
{
	int width;
	int x0;
	int y0 = BOTTOM_Y - 62;

	if (toast[0] == '\0')
		return;
	width = xenon_overlay_text_width(toast, 16, XENON_TEXT_REGULAR) + 44;
	if (width > 900)
		width = 900;
	x0 = (XENON_WIDTH - width) / 2;
	xenon_overlay_shadow(x0, y0, x0 + width, y0 + 42, 21, 8, 110);
	xenon_overlay_round_rect(x0, y0, x0 + width, y0 + 42, 21, 0x30363e);
	xenon_overlay_text_fit(x0 + 22, y0 + 27, toast, C_TEXT, 16, XENON_TEXT_REGULAR,
			       width - 44);
}

void xenon_ui_draw(void)
{
	if (on_home() && !url_mode)
		draw_home();

	draw_toolbar();
	draw_tabs();
	draw_status_bar();

	if (!on_home())
		draw_scroll_bar();
	if (menu_mode)
		draw_menu();
	if (url_mode)
		draw_url_panel();

	draw_toast();
}
