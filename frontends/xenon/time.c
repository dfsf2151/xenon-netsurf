/*
 * Corrects the console clock from HTTP Date headers for TLS.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <mbedtls/platform_time.h>

#include "utils/log.h"

#include "xenon/time.h"

enum {
	SECONDS_PER_DAY = 86400,
	/* A header further than this from the anchor is not a drift
	 * correction; it means one of the two clocks is wrong. */
	REANCHOR_SECONDS = 30 * SECONDS_PER_DAY,
};

/* Epoch seconds reported by the first Date header, and the monotonic
 * milliseconds at that moment. Zero until the first header. */
static int64_t anchor_date;
static int64_t anchor_ms;

/* newlib's time() goes through a _gettimeofday stub xsdk does not fill in,
 * so read the clock the same way the rest of the title does. */
static int64_t local_seconds(void)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0)
		return 0;
	return (int64_t)tv.tv_sec;
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t days_from_civil(int year, int month, int day)
{
	/* Howard Hinnant's civil-from-days, inverted. 1970-01-01 is day 0. */
	year -= month <= 2;
	const int era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yoe = (unsigned)(year - era * 400);
	const unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool parse_http_date(const char *text, int64_t *out)
{
	static const char *const months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	char weekday[4] = { 0 };
	char month[4] = { 0 };
	int day = 0;
	int year = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;

	if (sscanf(text, "%3s, %d %3s %d %d:%d:%d", weekday, &day, month,
		   &year, &hour, &minute, &second) != 7)
		return false;

	int month_index = -1;
	for (int i = 0; i < 12; i++) {
		if (strcmp(month, months[i]) == 0) {
			month_index = i;
			break;
		}
	}
	if (month_index < 0 || year < 1970 || day < 1 || day > 31 ||
	    hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
	    second < 0 || second > 60)
		return false;

	*out = days_from_civil(year, month_index + 1, day) * SECONDS_PER_DAY +
	       hour * 3600 + minute * 60 + second;
	return true;
}

bool xenon_time_has_anchor(void)
{
	return anchor_date != 0;
}

void xenon_time_note_http_date(const char *date)
{
	int64_t reported;
	int64_t current;

	if (date == NULL || !parse_http_date(date, &reported))
		return;

	if (anchor_date == 0) {
		anchor_date = reported;
		anchor_ms = monotonic_ms();
		NSLOG(netsurf, INFO, "time: clock set from the server's "
		      "date (%lld)", (long long)reported);
		return;
	}

	current = anchor_date + (monotonic_ms() - anchor_ms) / 1000;
	if (reported - current > REANCHOR_SECONDS ||
	    current - reported > REANCHOR_SECONDS) {
		NSLOG(netsurf, INFO, "time: server date is %lld s from "
		      "ours; re-anchoring", (long long)(reported - current));
		anchor_date = reported;
		anchor_ms = monotonic_ms();
	}
}

time_t xenon_time(time_t *t)
{
	int64_t now;

	if (anchor_date != 0) {
		now = anchor_date + (monotonic_ms() - anchor_ms) / 1000;
	} else {
		now = local_seconds();
	}

	if (t != NULL)
		*t = (time_t)now;
	return (time_t)now;
}

void xenon_clock_text(char *buf, size_t size)
{
	time_t now = xenon_time(NULL);
	struct tm tm;
	int hour;
	const char *suffix;

	if (gmtime_r(&now, &tm) == NULL) {
		snprintf(buf, size, "--:--");
		return;
	}
	hour = tm.tm_hour % 12;
	if (hour == 0)
		hour = 12;
	suffix = tm.tm_hour < 12 ? "AM" : "PM";
	snprintf(buf, size, "%d:%02d %s", hour, tm.tm_min, suffix);
}

void xenon_time_init(void)
{
	int ret = mbedtls_platform_set_time(xenon_time);
	if (ret != 0)
		NSLOG(netsurf, INFO, "time: cannot install the clock (%d)", ret);
}
