/*
 * Xenon input: the pad, plus a scripted source for emulator runs.
 *
 * input.txt next to the xex, if present, is a list of "<ms> <command>"
 * lines. Commands: move X Y, press B, release B, tap B, url URL,
 * dump NAME, quit. Buttons: A B X Y LB RB START BACK UP DOWN LEFT RIGHT.
 */

#ifndef NETSURF_XENON_INPUT_H
#define NETSURF_XENON_INPUT_H

#include <stdbool.h>

enum xenon_button {
	XB_A,
	XB_B,
	XB_X,
	XB_Y,
	XB_LB,
	XB_RB,
	XB_START,
	XB_BACK,
	XB_UP,
	XB_DOWN,
	XB_LEFT,
	XB_RIGHT,
	XB_COUNT
};

void xenon_input_init(void);
void xenon_input_poll(void);

bool xenon_input_down(enum xenon_button button);
bool xenon_input_pressed(enum xenon_button button);
bool xenon_input_released(enum xenon_button button);

/* Cursor movement for this frame, in pixels. */
void xenon_input_delta(int *dx, int *dy);
/* Page scroll asked for by the right stick this frame, in pixels. */
void xenon_input_scroll(int *dx, int *dy);

#endif
