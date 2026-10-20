/*
 * Clip-board clock for certificate checks.
 *
 * The console's clock is the kernel's system time; it is right when the
 * user set it or Xbox LIVE did, and arbitrary on an offline RGH box. Any
 * HTTP response carries a Date header, so we take the clock from the first
 * one we see and advance it with the monotonic timer. That keeps
 * certificates valid even when the guest clock jumps around (the emulator
 * does) or was never set.
 *
 * Before the first Date header the local clock is used as-is.
 */

#ifndef NETSURF_XENON_TIME_H
#define NETSURF_XENON_TIME_H

#include <stdbool.h>
#include <time.h>

/* Installs the corrected clock as mbedTLS's time source. */
void xenon_time_init(void);

/* The corrected current time. */
time_t xenon_time(time_t *t);

/* RFC 1123 date from an HTTP header, e.g. "Wed, 21 Oct 2015 07:28:00 GMT". */
void xenon_time_note_http_date(const char *date);

/* True once a Date header has set the clock. */
bool xenon_time_has_anchor(void);

/* Current time as "1:07 AM", for the status bar. */
void xenon_clock_text(char *buf, size_t size);

#endif
