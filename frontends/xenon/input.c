/*
 * Xenon input: the pad through XAM, overlaid with a scripted source.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "utils/log.h"

#include "xsdk/kernel.h"

#include "xenon/input.h"
#include "xenon/ui.h"
#include "xenon/xenon.h"

enum {
	/* Sticks below this stay centred. */
	DEAD_ZONE = 9000,
	/* Cursor pixels per frame at full deflection. */
	STICK_SPEED = 14,
	DPAD_SPEED = 10,
	/* Page pixels per frame at full right-stick deflection. */
	SCROLL_SPEED = 48,
	/* How long a scripted "tap" holds its button down. */
	TAP_MS = 90,
	/* Scripted button holds in flight at once. */
	PENDING_RELEASES = 8,
};

static const uint16_t button_masks[XB_COUNT] = {
	[XB_A] = INPUT_GAMEPAD_A,
	[XB_B] = INPUT_GAMEPAD_B,
	[XB_X] = INPUT_GAMEPAD_X,
	[XB_Y] = INPUT_GAMEPAD_Y,
	[XB_LB] = INPUT_GAMEPAD_LEFT_SHOULDER,
	[XB_RB] = INPUT_GAMEPAD_RIGHT_SHOULDER,
	[XB_START] = INPUT_GAMEPAD_START,
	[XB_BACK] = INPUT_GAMEPAD_BACK,
	[XB_UP] = INPUT_GAMEPAD_DPAD_UP,
	[XB_DOWN] = INPUT_GAMEPAD_DPAD_DOWN,
	[XB_LEFT] = INPUT_GAMEPAD_DPAD_LEFT,
	[XB_RIGHT] = INPUT_GAMEPAD_DPAD_RIGHT,
};

static uint16_t pad_buttons;
static uint16_t pad_previous;
static int pad_dx;
static int pad_dy;
static int pad_scroll_x;
static int pad_scroll_y;

static FILE *script;
static uint64_t script_start;
static uint16_t script_buttons;
static uint16_t script_previous;

static struct {
	uint16_t mask;
	uint64_t at;
} pending[PENDING_RELEASES];

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool button_named(const char *name, uint16_t *mask)
{
	static const struct {
		const char *name;
		enum xenon_button button;
	} names[] = {
		{ "A", XB_A }, { "B", XB_B }, { "X", XB_X }, { "Y", XB_Y },
		{ "LB", XB_LB }, { "RB", XB_RB }, { "START", XB_START }, { "BACK", XB_BACK },
		{ "UP", XB_UP }, { "DOWN", XB_DOWN }, { "LEFT", XB_LEFT }, { "RIGHT", XB_RIGHT },
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcasecmp(name, names[i].name) == 0) {
			*mask = button_masks[names[i].button];
			return true;
		}
	}
	return false;
}

static void script_schedule_release(uint16_t mask, uint64_t at)
{
	for (size_t i = 0; i < PENDING_RELEASES; i++) {
		if (pending[i].mask == 0) {
			pending[i].mask = mask;
			pending[i].at = at;
			return;
		}
	}
	/* No slot: drop the oldest. */
	pending[0].mask = mask;
	pending[0].at = at;
}

static void script_run_command(char *line, uint64_t now)
{
	char *command = strtok(line, " \t\r\n");
	if (command == NULL)
		return;

	if (strcmp(command, "move") == 0) {
		char *xs = strtok(NULL, " \t\r\n");
		char *ys = strtok(NULL, " \t\r\n");
		if (xs != NULL && ys != NULL)
			xenon_ui_set_cursor(atoi(xs), atoi(ys));
	} else if (strcmp(command, "press") == 0 || strcmp(command, "release") == 0 ||
		   strcmp(command, "tap") == 0) {
		char *name = strtok(NULL, " \t\r\n");
		uint16_t mask = 0;
		if (name == NULL || !button_named(name, &mask))
			return;
		if (strcmp(command, "press") == 0)
			script_buttons |= mask;
		else if (strcmp(command, "release") == 0)
			script_buttons &= ~mask;
		else {
			script_buttons |= mask;
			script_schedule_release(mask, now + TAP_MS);
		}
	} else if (strcmp(command, "url") == 0) {
		char *url = strtok(NULL, "\r\n");
		if (url != NULL)
			xenon_ui_script_url(url);
	} else if (strcmp(command, "dump") == 0) {
		char *name = strtok(NULL, " \t\r\n");
		xenon_screen_dump(name != NULL ? name : "shot");
	} else if (strcmp(command, "quit") == 0) {
		xenon_quit_requested = true;
	}
}

static void script_poll(uint64_t now)
{
	static char pending_line[1024];
	static bool has_pending;
	char line[1024];

	if (script == NULL)
		return;

	for (;;) {
		char *current;
		char *space;
		uint64_t due;

		if (has_pending) {
			memcpy(line, pending_line, sizeof(line));
			has_pending = false;
			current = line;
		} else {
			if (fgets(line, sizeof(line), script) == NULL)
				break;
			current = line;
		}

		if (current[0] == '#' || current[0] == '\n' || current[0] == '\r')
			continue;
		space = strchr(current, ' ');
		if (space == NULL)
			continue;

		due = strtoull(current, NULL, 10);
		if (due > now - script_start) {
			memcpy(pending_line, current, sizeof(pending_line));
			has_pending = true;
			break;
		}
		script_run_command(space + 1, now);
	}

	for (size_t i = 0; i < PENDING_RELEASES; i++) {
		if (pending[i].mask != 0 && now >= pending[i].at) {
			script_buttons &= ~pending[i].mask;
			pending[i].mask = 0;
		}
	}
}

void xenon_input_init(void)
{
	script = fopen("input.txt", "r");
	if (script != NULL) {
		script_start = now_ms();
		NSLOG(netsurf, INFO, "input: playing input.txt commands");
	}
}

void xenon_input_poll(void)
{
	INPUT_STATE state;
	uint64_t now = now_ms();

	pad_previous = pad_buttons;
	pad_buttons = 0;
	pad_dx = 0;
	pad_dy = 0;
	pad_scroll_x = 0;
	pad_scroll_y = 0;

	if (XamInputGetState(0, 0, &state) == ERROR_SUCCESS) {
		const INPUT_GAMEPAD *pad = &state.gamepad;
		pad_buttons = pad->buttons;

		if (pad->buttons & INPUT_GAMEPAD_DPAD_LEFT)
			pad_dx -= DPAD_SPEED;
		if (pad->buttons & INPUT_GAMEPAD_DPAD_RIGHT)
			pad_dx += DPAD_SPEED;
		if (pad->buttons & INPUT_GAMEPAD_DPAD_UP)
			pad_dy -= DPAD_SPEED;
		if (pad->buttons & INPUT_GAMEPAD_DPAD_DOWN)
			pad_dy += DPAD_SPEED;

		if (pad->thumb_lx < -DEAD_ZONE || pad->thumb_lx > DEAD_ZONE)
			pad_dx += (int32_t)pad->thumb_lx * STICK_SPEED / 32768;
		/* The stick's Y is positive upwards; the screen's grows down. */
		if (pad->thumb_ly < -DEAD_ZONE || pad->thumb_ly > DEAD_ZONE)
			pad_dy -= (int32_t)pad->thumb_ly * STICK_SPEED / 32768;

		/* The right stick scrolls: slow near the centre, fast at
		 * the edge. Up on the stick scrolls the page up. */
		if (pad->thumb_ry < -DEAD_ZONE || pad->thumb_ry > DEAD_ZONE) {
			int32_t v = pad->thumb_ry;
			pad_scroll_y = -(int)((int64_t)v * (v < 0 ? -v : v) *
					      SCROLL_SPEED / (32768LL * 32768LL));
		}
		if (pad->thumb_rx < -DEAD_ZONE || pad->thumb_rx > DEAD_ZONE) {
			int32_t v = pad->thumb_rx;
			pad_scroll_x = (int)((int64_t)v * (v < 0 ? -v : v) *
					     SCROLL_SPEED / (32768LL * 32768LL));
		}
	}

	script_previous = script_buttons;
	script_poll(now);
}

bool xenon_input_down(enum xenon_button button)
{
	uint16_t mask = button_masks[button];
	return (pad_buttons & mask) != 0 || (script_buttons & mask) != 0;
}

bool xenon_input_pressed(enum xenon_button button)
{
	uint16_t mask = button_masks[button];
	return ((pad_buttons & ~pad_previous) & mask) != 0 ||
	       ((script_buttons & ~script_previous) & mask) != 0;
}

bool xenon_input_released(enum xenon_button button)
{
	uint16_t mask = button_masks[button];
	return ((pad_previous & ~pad_buttons) & mask) != 0 ||
	       ((script_previous & ~script_buttons) & mask) != 0;
}

void xenon_input_delta(int *dx, int *dy)
{
	*dx = pad_dx;
	*dy = pad_dy;
}

void xenon_input_scroll(int *dx, int *dy)
{
	*dx = pad_scroll_x;
	*dy = pad_scroll_y;
}
