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

#include "xenon/401login.h"
#include "xenon/audio.h"
#include "xenon/bitmap.h"
#include "xenon/browser.h"
#include "xenon/fetch.h"
#include "xenon/http.h"
#include "xenon/filetype.h"
#include "xenon/input.h"
#include "xenon/layout.h"
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

static nserror set_defaults(struct nsoption_s *defaults)
{
	(void)defaults;
	nsoption_setnull_charp(cookie_file, strdup("Cookies"));
	nsoption_setnull_charp(cookie_jar, strdup("Cookies"));
	nsoption_setnull_charp(url_file, strdup("URLs"));

	/* The console has room for this; the stock 12 MB gives images only
	 * a 3 MB cache, which a Wikipedia article overflows. */
	nsoption_set_int(memory_cache_size, 64 * 1024 * 1024);

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

static void redraw_window(void)
{
	struct rect r = { 0, 0, XENON_WIDTH, XENON_PAGE_HEIGHT };
	struct redraw_context ctx = {
		.interactive = false,
		.background_images = true,
		.plot = xenon_plotters,
	};
	int sx = 0;
	int sy = 0;

	xenon_plot_reset();
	xenon_needs_redraw = false;

	if (xenon_bw == NULL)
		return;

	xenon_get_active_scroll(&sx, &sy);
	browser_window_redraw(xenon_bw, -sx, -sy, &r, &ctx);
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
		.llcache = filesystem_llcache_table,
	};
	char url_text[1024];
	nsurl *url = NULL;
	nserror ret;
	uint64_t start;
	bool drawn = false;
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

	nslog_init(nslog_stream_configure, &fake_argc, fake_argv);
	verbose_log = true;

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
			xenon_screen_present();
			if (!drawn) {
				fprintf(stderr, "xenon: first paint after %u ms\n",
					(unsigned)(now_ms() - start));
				drawn = true;
			}
		} else if (xenon_ui_dirty_take()) {
			xenon_screen_present();
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
	xenon_screen_dump("fb");

	xenon_screen_close();

	xenon_kill_browser_windows();
	netsurf_exit();
	nsoption_finalise(nsoptions, nsoptions_default);
	nslog_finalise();

	fprintf(stderr, "xenon: finished\n");
	return 0;
}
