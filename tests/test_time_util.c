// The module under test is #included rather than linked so its static helpers
// are reachable. Each src/*.c is included by exactly one test file.
#include "../src/time_util.c"

#include "harness.h"

void run_time_util_tests(void) {
    SUITE("time_util: iso8601_to_utc");
    // 2025-08-24T10:00:00Z. Checked against `date -u -d ... +%s`.
    CHECK_INT(iso8601_to_utc("2025-08-24T10:00:00Z"), 1756029600);
    // The epoch itself, to pin down that this is UTC and not local time.
    CHECK_INT(iso8601_to_utc("1970-01-01T00:00:00Z"), 0);
    // Fractional seconds and a missing Z are both tolerated.
    CHECK_INT(iso8601_to_utc("2025-08-24T10:00:00.123Z"), 1756029600);
    CHECK_INT(iso8601_to_utc("2025-08-24T10:00:00"), 1756029600);
    // Leap day, since the month/day arithmetic is hand-rolled into struct tm.
    CHECK_INT(iso8601_to_utc("2024-02-29T12:00:00Z"), 1709208000);

    SUITE("time_util: dates before the epoch count backwards");
    // This is the case the two platforms used to disagree about, and why the
    // calendar arithmetic is done here rather than by the C library: timegm
    // returns the negative second count, while Microsoft's _mkgmtime and
    // gmtime_s refuse anything earlier than 1970 outright. A GPX with a broken
    // clock is dated here, and stats.c steps back a year at a time into it.
    CHECK_INT(iso8601_to_utc("1969-12-31T23:59:00Z"), -60);
    CHECK_INT(iso8601_to_utc("1969-01-01T00:00:00Z"), -31536000);
    // Before a leap day, and before a century that is not a leap year.
    CHECK_INT(iso8601_to_utc("1968-02-29T00:00:00Z"), -58060800);
    CHECK_INT(iso8601_to_utc("1900-03-01T00:00:00Z"), -2203891200);

    SUITE("time_util: the two directions are inverses either side of the epoch");
    {
        // Every field round-trips, which is what stats.c relies on when it
        // takes a date apart, adds to one field and puts it back together.
        static const char *const stamps[] = {
            "2025-08-24T10:00:00Z",
            "1970-01-01T00:00:00Z",
            "1969-07-20T20:17:40Z",
            "1901-12-13T20:45:52Z",
            "2000-02-29T23:59:59Z",
            "1899-12-31T00:00:01Z",
        };
        for (size_t i = 0; i < sizeof(stamps) / sizeof(stamps[0]); i++) {
            time_t utc = iso8601_to_utc(stamps[i]);
            struct tm tm;
            CHECK(utc_to_tm(utc, &tm));
            CHECK_INT(utc_from_tm(&tm), utc);
        }
    }

    SUITE("time_util: the weekday is right on both sides of the epoch");
    {
        struct tm tm;
        // 1970-01-01 was a Thursday, and tm_wday counts from Sunday.
        CHECK(utc_to_tm(0, &tm));
        CHECK_INT(tm.tm_wday, 4);
        CHECK_INT(tm.tm_yday, 0);
        // The day before it was a Wednesday -- the remainder that C's % leaves
        // negative, and what the week bucketing in stats.c reads.
        CHECK(utc_to_tm(-86400, &tm));
        CHECK_INT(tm.tm_wday, 3);
        CHECK_INT(tm.tm_yday, 364);
        // A Sunday, so the wrap at the end of the week is covered too.
        CHECK(utc_to_tm(iso8601_to_utc("2025-08-24T00:00:00Z"), &tm));
        CHECK_INT(tm.tm_wday, 0);
    }

    SUITE("time_util: out-of-range fields are carried, not rejected");
    {
        // stats_period_step adds to one field and hands the result back, which
        // only works because the normalisation carries it.
        struct tm tm;
        CHECK(utc_to_tm(iso8601_to_utc("2021-01-01T00:00:00Z"), &tm));
        tm.tm_mon -= 24; // two years back, through a month field of -24
        CHECK_INT(utc_from_tm(&tm), iso8601_to_utc("2019-01-01T00:00:00Z"));
        // And the tm it hands back has been normalised into real fields.
        CHECK_INT(tm.tm_mon, 0);
        CHECK_INT(tm.tm_year, 2019 - 1900);

        CHECK(utc_to_tm(iso8601_to_utc("1970-03-01T00:00:00Z"), &tm));
        tm.tm_mday -= 100; // back over the epoch by way of a negative day
        CHECK_INT(utc_from_tm(&tm), iso8601_to_utc("1969-11-21T00:00:00Z"));
    }

    SUITE("time_util: iso8601_to_utc rejects malformed input");
    CHECK_INT(iso8601_to_utc(""), (time_t)-1);
    CHECK_INT(iso8601_to_utc("not a timestamp"), (time_t)-1);
    CHECK_INT(iso8601_to_utc("2025-08-24"), (time_t)-1);

    SUITE("time_util: european_date_to_utc");
    // Midnight UTC on the given day; this is what the date filter compares to.
    CHECK_INT(european_date_to_utc("24.08.2025"), 1755993600);
    CHECK_INT(european_date_to_utc("01.01.1980"), 315532800);
    CHECK_INT(european_date_to_utc(""), (time_t)-1);
    CHECK_INT(european_date_to_utc("garbage"), (time_t)-1);

    SUITE("time_util: the two parsers agree on the same instant");
    // A track starting at midnight must not be filtered out by a bound set to
    // its own date -- the bug that motivated a single UTC parser.
    CHECK_INT(iso8601_to_utc("2025-08-24T00:00:00Z"),
              european_date_to_utc("24.08.2025"));

    SUITE("time_util: utc_to_display_strings");
    char date[16], time_str[16];
    CHECK(utc_to_display_strings(iso8601_to_utc("2025-08-24T15:02:15Z"),
                                 date, sizeof(date), time_str, sizeof(time_str)));
    CHECK_STR(date, "24.08.2025");
    CHECK_STR(time_str, "15:02");
    // Rendered as UTC, not in whatever zone the machine happens to be in.
    CHECK(utc_to_display_strings(0, date, sizeof(date), time_str, sizeof(time_str)));
    CHECK_STR(date, "01.01.1970");
    CHECK_STR(time_str, "00:00");
    // A track with no usable timestamp is reported rather than rendered.
    CHECK(!utc_to_display_strings((time_t)-1, date, sizeof(date), time_str,
                                  sizeof(time_str)));
}
