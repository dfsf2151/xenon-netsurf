/*
 * Xenon (Xbox 360) frontend shared state.
 *
 * The frontend draws into a single 1280x720 XRGB buffer; the browser core
 * gets its plot operations through plot.c and the result is presented at
 * the end of a redraw.
 */

#ifndef NETSURF_XENON_XENON_H
#define NETSURF_XENON_XENON_H

#include <stdbool.h>
#include <stdint.h>

#define XENON_WIDTH 1280
#define XENON_HEIGHT 720

/* The page lives between the fixed top chrome and the status bar. */
#define XENON_CHROME_TOP 146
#define XENON_CHROME_BOTTOM 44
#define XENON_PAGE_HEIGHT (XENON_HEIGHT - XENON_CHROME_TOP - XENON_CHROME_BOTTOM)

struct browser_window;

extern struct browser_window *xenon_bw;
extern volatile bool xenon_needs_redraw;
extern volatile bool xenon_quit_requested;

extern uint32_t xenon_fb[XENON_WIDTH * XENON_HEIGHT];

void xenon_plot_reset(void);
void xenon_plot_stats(void);
/* Presents the framebuffer and leaves the display open for later redraws;
 * xenon_screen_close hands the screen back to the system. */
void xenon_screen_present(void);
void xenon_screen_close(void);
/* Writes the page framebuffer beside the log, for tests. */
void xenon_screen_dump(const char *name);

/* Chrome overlay: draws into a presented tile buffer, or a linear copy
 * for screenshots. */
void xenon_overlay_begin(uint32_t *dst, uint32_t width, uint32_t height);
void xenon_overlay_begin_linear(uint32_t *dst, uint32_t width, uint32_t height);
void xenon_overlay_rect(int x0, int y0, int x1, int y1, uint32_t rgb);
void xenon_overlay_text(int x, int ytop, const char *text, uint32_t rgb);
void xenon_overlay_text_scaled(int x, int ytop, const char *text, uint32_t rgb,
			       int scale);
void xenon_overlay_line(int x0, int y0, int x1, int y1, int width, uint32_t rgb);
void xenon_overlay_disc(int cx, int cy, int radius, uint32_t rgb);
void xenon_overlay_ring(int cx, int cy, int radius, int thickness, uint32_t rgb);
void xenon_overlay_round_rect(int x0, int y0, int x1, int y1, int radius,
			      uint32_t rgb);
void xenon_overlay_round_frame(int x0, int y0, int x1, int y1, int radius,
			       int thickness, uint32_t rgb);
void xenon_overlay_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
			    uint32_t rgb);
void xenon_overlay_arrow(int x, int y);

#endif /* NETSURF_XENON_XENON_H */
