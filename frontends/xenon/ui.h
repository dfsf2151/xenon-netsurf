/*
 * Xenon browser chrome: cursor, address bar, on-screen keyboard, toasts.
 */

#ifndef NETSURF_XENON_UI_H
#define NETSURF_XENON_UI_H

#include <stdbool.h>

struct gui_clipboard_table;

void xenon_ui_init(void);
/* Handles the pad edges for this frame; may navigate the browser. */
void xenon_ui_update(void);
/* Draws the chrome into the current present target. */
void xenon_ui_draw(void);
enum { XENON_UI_DIRTY_FULL = 1, XENON_UI_DIRTY_CURSOR = 2 };
/* What changed since the last call: XENON_UI_DIRTY_* flags. */
int xenon_ui_dirty_take(void);
/* The pointer's position; false when it is hidden. */
bool xenon_ui_cursor(int *x, int *y);
void xenon_ui_set_cursor(int x, int y);
void xenon_ui_toast(const char *text);
/* The status line at the foot of the screen (loading, link targets). */
void xenon_ui_status(const char *text);
void xenon_ui_script_url(const char *url);
/* A tab, title, URL or loading state changed outside the UI. */
void xenon_ui_chrome_changed(void);
/* The page placed (true) or removed its text caret. A caret placed just
 * after the user pressed A on the page opens the keyboard for that field. */
void xenon_ui_caret(bool shown);

/* The clipboard the core copies into and pastes from. */
extern struct gui_clipboard_table *xenon_clipboard_table;

#endif
