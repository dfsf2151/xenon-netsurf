/*
 * Xenon browser chrome: cursor, address bar, on-screen keyboard, toasts.
 */

#ifndef NETSURF_XENON_UI_H
#define NETSURF_XENON_UI_H

#include <stdbool.h>

void xenon_ui_init(void);
/* Handles the pad edges for this frame; may navigate the browser. */
void xenon_ui_update(void);
/* Draws the chrome into the current present target. */
void xenon_ui_draw(void);
bool xenon_ui_dirty_take(void);
void xenon_ui_set_cursor(int x, int y);
void xenon_ui_toast(const char *text);
void xenon_ui_script_url(const char *url);
/* A tab, title, URL or loading state changed outside the UI. */
void xenon_ui_chrome_changed(void);

#endif
