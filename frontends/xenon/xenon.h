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
#define XENON_CHROME_TOP 104
#define XENON_CHROME_BOTTOM 30
#define XENON_PAGE_HEIGHT (XENON_HEIGHT - XENON_CHROME_TOP - XENON_CHROME_BOTTOM)

struct browser_window;

extern struct browser_window *xenon_bw;
extern volatile bool xenon_needs_redraw;
extern volatile bool xenon_quit_requested;

extern uint32_t xenon_fb[XENON_WIDTH * XENON_HEIGHT];

struct rect;

/* Page drawing: begin a redraw (resets the counters), clear a page rectangle
 * to white, and move the page pixels up by dy rows (down when negative). */
void xenon_plot_begin(void);
void xenon_plot_clear(const struct rect *area);
void xenon_plot_shift_page(int dy);

/* What needs drawing again: all of the page, or a rectangle in page
 * coordinates. Either asks for a redraw. */
void xenon_damage_all(void);
void xenon_damage_rect(const struct rect *rect);
void xenon_plot_stats(void);
/* Presents the framebuffer and leaves the display open for later redraws;
 * xenon_screen_close hands the screen back to the system. full composes
 * page and chrome again; otherwise only the pointer moved, and only the
 * tiles under its old and new places are written. False when no buffer
 * came free in time; call again later. */
bool xenon_screen_present(bool full);
void xenon_screen_close(void);
/* Writes the page framebuffer beside the log, for tests. */
void xenon_screen_dump(const char *name);
/* Present timing and font cache figures, to the log. */
void xenon_screen_stats(void);

/* Chrome overlay. The UI draws into a linear 0x00RRGGBB buffer (the
 * composed frame, or a screenshot copy); the present step tiles it. Every
 * shape takes an alpha (255 opaque) and has anti-aliased edges. */
void xenon_overlay_begin_linear(uint32_t *dst, uint32_t width, uint32_t height);
void xenon_overlay_clip(int x0, int y0, int x1, int y1);
void xenon_overlay_unclip(void);
void xenon_overlay_rect(int x0, int y0, int x1, int y1, uint32_t rgb);
void xenon_overlay_rect_a(int x0, int y0, int x1, int y1, uint32_t rgb, unsigned alpha);
void xenon_overlay_gradient(int x0, int y0, int x1, int y1, uint32_t top, uint32_t bottom);
void xenon_overlay_round_rect(int x0, int y0, int x1, int y1, int radius, uint32_t rgb);
void xenon_overlay_round_rect_a(int x0, int y0, int x1, int y1, int radius,
				uint32_t rgb, unsigned alpha);
void xenon_overlay_round_frame(int x0, int y0, int x1, int y1, int radius,
			       int thickness, uint32_t rgb);
void xenon_overlay_shadow(int x0, int y0, int x1, int y1, int radius, int spread,
			  unsigned alpha);
void xenon_overlay_disc(int cx, int cy, int radius, uint32_t rgb);
void xenon_overlay_disc_a(int cx, int cy, int radius, uint32_t rgb, unsigned alpha);
void xenon_overlay_ring(int cx, int cy, int radius, int thickness, uint32_t rgb);
void xenon_overlay_line(int x0, int y0, int x1, int y1, int width, uint32_t rgb);
void xenon_overlay_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
			    uint32_t rgb);
void xenon_overlay_arrow(int x, int y);

/* UI text: the sans faces at a pixel size, baseline at y. */
enum { XENON_TEXT_REGULAR = 0, XENON_TEXT_BOLD = 1 };
int xenon_overlay_text_width(const char *text, int px, int weight);
int xenon_overlay_text(int x, int y, const char *text, uint32_t rgb, int px, int weight);
/* Text cut to max_width with an ellipsis. */
int xenon_overlay_text_fit(int x, int y, const char *text, uint32_t rgb, int px,
			   int weight, int max_width);

#endif /* NETSURF_XENON_XENON_H */
