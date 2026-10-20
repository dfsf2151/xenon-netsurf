/*
 * Xenon (Xbox 360) frontend entry point.
 *
 * Starts NetSurf with the xenon table, loads a URL (url.txt next to the
 * xex, or a built-in default), renders it into the framebuffer and
 * presents it. No interactive input yet: this is the rendering spike.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdint.h>

#include "utils/log.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "netsurf/bitmap.h"
#include "netsurf/browser_window.h"
#include "netsurf/misc.h"
#include "netsurf/netsurf.h"
#include "netsurf/plotters.h"
#include "content/backing_store.h"
#include "content/urldb.h"

#include "xenon/401login.h"
#include "xenon/audio.h"
#include "xenon/bitmap.h"
#include "xenon/browser.h"
#include "xenon/fetch.h"
#include "xenon/http.h"
#include "xenon/filetype.h"
#include "xenon/input.h"
#include "xenon/layout.h"
#include "xenon/output.h"
#include "xenon/plot.h"
#include "xenon/schedule.h"
#include "xenon/time.h"
#include "xenon/ui.h"
#include "xenon/xenon.h"

static char *xenon_respath_list[] = { "res", NULL };
char **respaths = xenon_respath_list;

static void die(const char *error)
{
	fprintf(stderr, "xenon: %s\n", error);
	exit(EXIT_FAILURE);
}

/* Sites whose scripts need more than this browser's ECMAScript 5 and only
 * slow the page down: their pages load with scripts off. One host suffix a
 * line in nojs.txt beside the XEX, # for comments; without the file, the
 * list below. */
static char nojs_hosts[32][64];
static int nojs_count = -1;

static void nojs_load(void)
{
	static const char *const builtin[] = {
		"instagram.com", "cdninstagram.com", "facebook.com", "threads.net",
	};
	FILE *fp = fopen("nojs.txt", "r");
	char line[128];

	nojs_count = 0;
	if (fp == NULL) {
		for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++)
			snprintf(nojs_hosts[nojs_count++], sizeof(nojs_hosts[0]), "%s",
				 builtin[i]);
		return;
	}
	while (nojs_count < 32 && fgets(line, sizeof(line), fp) != NULL) {
		char *end = line + strcspn(line, " \t\r\n#");
		*end = '\0';
		if (line[0] != '\0')
			snprintf(nojs_hosts[nojs_count++], sizeof(nojs_hosts[0]), "%s", line);
	}
	fclose(fp);
}

bool html_scripting_allowed(struct nsurl *url)
{
	lwc_string *host = nsurl_get_component(url, NSURL_HOST);
	bool allowed = true;

	if (host == NULL)
		return true;
	if (nojs_count < 0)
		nojs_load();
	for (int i = 0; i < nojs_count && allowed; i++) {
		size_t hl = lwc_string_length(host);
		size_t sl = strlen(nojs_hosts[i]);
		const char *h = lwc_string_data(host);
		if (hl >= sl && strcasecmp(h + hl - sl, nojs_hosts[i]) == 0 &&
		    (hl == sl || h[hl - sl - 1] == '.'))
			allowed = false;
	}
	lwc_string_unref(host);
	return allowed;
}

/* The first cookie file that can be written: beside the XEX on a console's
 * USB drive or disk, cache:\\ under Xenia. */
static const char *cookie_path(void)
{
	static const char *const paths[] = { "Cookies", "cache:\\Cookies" };
	static const char *chosen;

	if (chosen != NULL)
		return chosen;
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		FILE *fp = fopen(paths[i], "ab");
		if (fp != NULL) {
			fclose(fp);
			chosen = paths[i];
			return chosen;
		}
	}
	chosen = paths[0];
	return chosen;
}

static nserror set_defaults(struct nsoption_s *defaults)
{
	(void)defaults;
	nsoption_setnull_charp(cookie_file, strdup("Cookies"));
	nsoption_setnull_charp(cookie_jar, strdup("Cookies"));
	nsoption_setnull_charp(url_file, strdup("URLs"));

	/* The console has room for this; the stock 12 MB gives images only
	 * a 3 MB cache, which a Wikipedia article overflows. */
	nsoption_set_int(memory_cache_size, 64 * 1024 * 1024);

	/* Duktape runs page scripts (ECMAScript 5.1 and NetSurf's DOM). A
	 * script gets 5 s before it is stopped, so a heavy page can't hang
	 * the console. */
	nsoption_set_bool(enable_javascript, true);
	nsoption_set_int(script_timeout, 5);

	/* Backing store writes go nowhere useful on a game partition. */
	nsoption_set_uint(disc_cache_size, 0);

	return NSERROR_OK;
}

static bool nslog_stream_configure(FILE *fptr)
{
	setbuf(fptr, NULL);
	return true;
}

static void xenon_quit(void)
{
	xenon_fetch_filetype_fin();
}

static nserror gui_launch_url(struct nsurl *url)
{
	fprintf(stderr, "xenon: launch url %s\n", nsurl_access(url));
	return NSERROR_OK;
}

static nserror gui_present_cookies(const char *search_term)
{
	(void)search_term;
	return NSERROR_OK;
}

static struct gui_misc_table xenon_misc_table = {
	.schedule = xenon_schedule,
	.quit = xenon_quit,
	.launch_url = gui_launch_url,
	.login = gui_401login_open,
	.present_cookies = gui_present_cookies,
};

extern void *sbrk(intptr_t);

static unsigned int heap_kb(void)
{
	void *brk = sbrk(0);
	if (brk == (void *)-1)
		return 0;
	return (unsigned int)((uintptr_t)brk / 1024);
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(int ms)
{
	struct timespec ts;
	if (ms <= 0)
		return;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

static void choose_url(char *buf, size_t size)
{
	FILE *fp = fopen("url.txt", "rb");

	buf[0] = '\0';
	if (fp != NULL) {
		size_t n = fread(buf, 1, size - 1, fp);
		fclose(fp);
		buf[n] = '\0';
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
			buf[--n] = '\0';
		}
	}
	if (buf[0] == '\0') {
		snprintf(buf, size, "file:///game:/home.html");
	}
}

/* What the page buffer holds, so a redraw can reuse it: a scroll shifts
 * the pixels and draws only the strip that came into view, and a change in
 * part of the page redraws only that part. */
static struct {
	bool valid;
	struct browser_window *bw;
	int sx, sy;
} drawn_page;

static bool damage_all = true;
static bool damage_some;
static struct rect damage; /* page coordinates */
static unsigned redraws_full, redraws_scroll, redraws_part;

void xenon_damage_all(void)
{
	damage_all = true;
	xenon_needs_redraw = true;
}

void xenon_damage_rect(const struct rect *rect)
{
	if (!damage_some) {
		damage = *rect;
		damage_some = true;
	} else {
		if (rect->x0 < damage.x0)
			damage.x0 = rect->x0;
		if (rect->y0 < damage.y0)
			damage.y0 = rect->y0;
		if (rect->x1 > damage.x1)
			damage.x1 = rect->x1;
		if (rect->y1 > damage.y1)
			damage.y1 = rect->y1;
	}
	xenon_needs_redraw = true;
}

static void redraw_clip(struct rect clip, int sx, int sy)
{
	struct redraw_context ctx = {
		.interactive = false,
		.background_images = true,
		.plot = xenon_plotters,
	};

	if (clip.x0 < 0)
		clip.x0 = 0;
	if (clip.y0 < 0)
		clip.y0 = 0;
	if (clip.x1 > XENON_WIDTH)
		clip.x1 = XENON_WIDTH;
	if (clip.y1 > XENON_PAGE_HEIGHT)
		clip.y1 = XENON_PAGE_HEIGHT;
	if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1)
		return;
	xenon_plot_clear(&clip);
	browser_window_redraw(xenon_bw, -sx, -sy, &clip, &ctx);
}

static void redraw_window(void)
{
	struct rect whole = { 0, 0, XENON_WIDTH, XENON_PAGE_HEIGHT };
	int sx = 0;
	int sy = 0;
	int dy;

	xenon_needs_redraw = false;
	xenon_plot_begin();

	if (xenon_bw == NULL) {
		xenon_plot_clear(&whole);
		drawn_page.valid = false;
		damage_all = false;
		damage_some = false;
		return;
	}

	xenon_get_active_scroll(&sx, &sy);
	dy = sy - drawn_page.sy;

	if (damage_all || !drawn_page.valid || drawn_page.bw != xenon_bw ||
	    sx != drawn_page.sx || dy >= XENON_PAGE_HEIGHT || -dy >= XENON_PAGE_HEIGHT) {
		redraw_clip(whole, sx, sy);
		redraws_full++;
	} else {
		if (dy != 0) {
			struct rect strip = whole;

			xenon_plot_shift_page(dy);
			if (dy > 0)
				strip.y0 = XENON_PAGE_HEIGHT - dy;
			else
				strip.y1 = -dy;
			redraw_clip(strip, sx, sy);
			redraws_scroll++;
		}
		if (damage_some) {
			struct rect view = {
				damage.x0 - sx, damage.y0 - sy,
				damage.x1 - sx, damage.y1 - sy,
			};
			redraw_clip(view, sx, sy);
			redraws_part++;
		}
	}

	drawn_page.valid = true;
	drawn_page.bw = xenon_bw;
	drawn_page.sx = sx;
	drawn_page.sy = sy;
	damage_all = false;
	damage_some = false;
}

int main(int argc, char **argv)
{
	struct netsurf_table xenon_table = {
		.misc = &xenon_misc_table,
		.window = xenon_window_table,
		.download = xenon_download_table,
		.fetch = xenon_fetch_table,
		.bitmap = xenon_bitmap_table,
		.layout = xenon_layout_table,
		.clipboard = xenon_clipboard_table,
		.llcache = filesystem_llcache_table,
	};
	char url_text[1024];
	nsurl *url = NULL;
	nserror ret;
	uint64_t start;
	bool drawn = false;
	bool present_full = false;
	bool present_cursor = false;
	int fake_argc = 0;
	char *fake_argv[] = { NULL };

	(void)argc;
	(void)argv;

	fprintf(stderr, "xenon: heap after startup: %u kB\n", heap_kb());

	ret = netsurf_register(&xenon_table);
	if (ret != NSERROR_OK)
		die("operation table registration failed");

	setbuf(stdin, NULL);
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	{
		FILE *flag = fopen("debug.txt", "rb");
		xenon_debug_log = flag != NULL;
		if (flag != NULL)
			fclose(flag);
	}
	nslog_init(nslog_stream_configure, &fake_argc, fake_argv);
	verbose_log = xenon_debug_log;

	ret = nsoption_init(set_defaults, &nsoptions, &nsoptions_default);
	if (ret != NSERROR_OK)
		die("options init failed");

	/* Images decode straight into bytes in this order. */
	bitmap_set_format(&(bitmap_fmt_t) {
		.layout = BITMAP_LAYOUT_R8G8B8A8,
		.pma = false,
	});

	ret = netsurf_init(NULL);
	if (ret != NSERROR_OK)
		die("NetSurf init failed");

	/* Cookies live beside the XEX between runs, so a site you signed in to
	 * stays signed in; Xenia keeps game:\\ read-only, so cache:\\ there. */
	urldb_load_cookies(cookie_path());

	ret = fetch_http_register();
	if (ret != NSERROR_OK)
		die("HTTP fetcher registration failed");

	xenon_fetch_filetype_init("mime.types");

	choose_url(url_text, sizeof(url_text));
	fprintf(stderr, "xenon: loading %s\n", url_text);

	ret = nsurl_create(url_text, &url);
	if (ret != NSERROR_OK) {
		fprintf(stderr, "xenon: bad url\n");
	} else {
		ret = browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, NULL);
		nsurl_unref(url);
		if (ret != NSERROR_OK)
			fprintf(stderr, "xenon: browser window create failed %d\n", (int)ret);
	}

	start = now_ms();

	xenon_input_init();
	xenon_time_init();
	xenon_ui_init();
	{
		struct rect page = { 0, 0, XENON_WIDTH, XENON_PAGE_HEIGHT };
		xenon_plot_clear(&page);
	}
	if (!xenon_audio_init())
		fprintf(stderr, "xenon: audio unavailable: %s\n", xenon_audio_status());

	/* The browser runs until START, or a scripted "quit". */
	for (;;) {
		int delay;

		xenon_input_poll();
		xenon_ui_update();
		delay = xenon_schedule_run();

		if (xenon_needs_redraw) {
			redraw_window();
			(void)xenon_ui_dirty_take();
			present_full = true;
		} else {
			int flags = xenon_ui_dirty_take();
			if (flags & XENON_UI_DIRTY_FULL)
				present_full = true;
			else if (flags & XENON_UI_DIRTY_CURSOR)
				present_cursor = true;
		}

		/* A present that found no free buffer is retried on the next
		 * pass rather than waited for. */
		if (present_full || present_cursor) {
			if (xenon_screen_present(present_full)) {
				present_full = false;
				present_cursor = false;
				if (!drawn) {
					fprintf(stderr, "xenon: first paint after %u ms\n",
						(unsigned)(now_ms() - start));
					drawn = true;
				}
			}
		}

		if (xenon_quit_requested)
			break;

		if (delay < 0 || delay > 50)
			delay = 20;
		if (delay < 8)
			delay = 8;
		sleep_ms(delay);
	}

	if (!drawn)
		redraw_window();

	fprintf(stderr, "xenon: render done after %u ms\n", (unsigned)(now_ms() - start));
	fprintf(stderr, "xenon: heap after render: %u kB\n", heap_kb());
	xenon_plot_stats();
	fprintf(stderr, "xenon: page redraws: %u whole, %u scrolled, %u partial\n",
		redraws_full, redraws_scroll, redraws_part);
	xenon_screen_stats();
	xenon_screen_dump("fb");

	xenon_screen_close();

	urldb_save_cookies(cookie_path());

	xenon_kill_browser_windows();
	netsurf_exit();
	nsoption_finalise(nsoptions, nsoptions_default);
	nslog_finalise();

	fprintf(stderr, "xenon: finished\n");
	return 0;
}
