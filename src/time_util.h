#ifndef TIME_UTIL_H
#define TIME_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Every timestamp in the application is handled as UTC. GPX records times in
// UTC, so a bound parsed in local time would shift the filter boundary by the
// machine's offset.

// Parses "YYYY-MM-DDTHH:MM:SS[.sss][Z]", ignoring anything after the seconds.
// Returns (time_t)-1 if the string does not start with that shape.
time_t iso8601_to_utc(const char *timestr);

// Parses the "DD.MM.YYYY" form the filter fields are edited in, at midnight.
// Returns (time_t)-1 on a malformed string.
time_t european_date_to_utc(const char *date);

// Renders a UTC timestamp into the "DD.MM.YYYY" and "HH:MM" strings the UI
// displays. Returns false for (time_t)-1, i.e. a track with no usable time.
bool utc_to_display_strings(time_t utc,
                            char *out_date, size_t date_size,
                            char *out_time, size_t time_size);

// The two halves of reading a timestamp as UTC rather than as local time, which
// is the one thing the C library spells differently on Windows. Spelled here
// once so that whatever needs to take a date apart does not have to.

// Normalises `tm` in place and returns the instant it names. Out-of-range
// fields are carried, so subtracting from tm_mday or tm_mon is how a step
// backwards through the calendar is written.
time_t utc_from_tm(struct tm *tm);

// Fills `out` with the UTC calendar fields of `utc`, tm_wday and tm_yday
// included. Returns false for (time_t)-1.
bool utc_to_tm(time_t utc, struct tm *out);

#endif
