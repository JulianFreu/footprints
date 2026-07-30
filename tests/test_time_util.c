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
