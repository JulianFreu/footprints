#include "time_util.h"

#include <stdio.h>

// Its own rather than shared with the one in stats.c: the two modules are
// separate translation units, and a constant this fixed is not worth a header.
#define SECONDS_PER_DAY 86400

// The calendar, done here rather than asked of the C library.
//
// This used to be timegm() on POSIX and _mkgmtime()/gmtime_s() on Windows, and
// those two do not agree about the one case the application actually cares
// about: Microsoft's pair refuse any instant before 1970 and answer -1, while
// timegm() returns the negative second count. A GPX with a broken clock is
// dated 1970 or earlier often enough to matter, stats.c steps backwards through
// the calendar a year at a time, and the same file reading differently on the
// two platforms is worse than either behaviour on its own.
//
// So both directions are integer arithmetic over the proleptic Gregorian
// calendar -- Howard Hinnant's days_from_civil and civil_from_days -- which is
// exact, has no epoch of its own to be limited by, and gives both platforms the
// same answer by construction.

// Division that rounds towards negative infinity, which is what stepping into
// the years before the epoch needs; C's own division rounds towards zero.
static long long floor_div(long long numerator, long long denominator) {
    long long quotient = numerator / denominator;
    if (numerator % denominator != 0 && (numerator < 0) != (denominator < 0))
        quotient--;
    return quotient;
}

// Days from 1970-01-01 to y-m-d. `m` is [1, 12] and `d` is [1, 31]; the year is
// the real one, not an offset from 1900.
static long long days_from_civil(long long y, int m, int d) {
    // March-based years, so the leap day is the last day of the year and every
    // other month sits at a fixed offset from the one before it.
    y -= m <= 2;

    const long long era = floor_div(y, 400);
    const long long year_of_era = y - era * 400; // [0, 399]
    const long long day_of_year = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

    // 719468 is the number of days from 0000-03-01 to 1970-01-01.
    return era * 146097 + day_of_era - 719468;
}

// The inverse: the civil date `days` after 1970-01-01, which may be before it.
static void civil_from_days(long long days, long long *y, int *m, int *d) {
    days += 719468;

    const long long era = floor_div(days, 146097);
    const long long day_of_era = days - era * 146097; // [0, 146096]
    const long long year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const long long day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const long long month_index = (5 * day_of_year + 2) / 153;

    *d = (int)(day_of_year - (153 * month_index + 2) / 5 + 1);
    *m = (int)(month_index + (month_index < 10 ? 3 : -9));
    *y = year_of_era + era * 400 + (*m <= 2);
}

time_t utc_from_tm(struct tm *tm) {
    // Every field is carried rather than rejected. Adding to tm_mday or tm_mon
    // is how stats.c walks the calendar, so "the minus fiftieth of March" and
    // "month -46" both have to name a real instant.
    const long long months = (long long)tm->tm_year * 12 + tm->tm_mon;
    const long long year = floor_div(months, 12);
    const int month = (int)(months - year * 12);

    const long long days =
        days_from_civil(year + 1900, month + 1, 1) + (long long)tm->tm_mday - 1;
    const long long seconds = days * SECONDS_PER_DAY + (long long)tm->tm_hour * 3600 +
                              (long long)tm->tm_min * 60 + tm->tm_sec;

    const time_t utc = (time_t)seconds;
    // The caller is promised a normalised tm, and the reverse direction is
    // where the normalised fields already come from.
    utc_to_tm(utc, tm);
    return utc;
}

bool utc_to_tm(time_t utc, struct tm *out) {
    if (utc == (time_t)-1)
        return false;

    const long long seconds = (long long)utc;
    const long long days = floor_div(seconds, SECONDS_PER_DAY);
    const int time_of_day = (int)(seconds - days * SECONDS_PER_DAY);

    long long year;
    int month, day;
    civil_from_days(days, &year, &month, &day);

    *out = (struct tm){0};
    out->tm_year = (int)(year - 1900);
    out->tm_mon = month - 1;
    out->tm_mday = day;
    out->tm_hour = time_of_day / 3600;
    out->tm_min = (time_of_day % 3600) / 60;
    out->tm_sec = time_of_day % 60;
    // 1970-01-01 was a Thursday, and tm_wday counts from Sunday. The + 11 is
    // what keeps the remainder positive for the days before the epoch, where
    // C's % gives a negative one.
    out->tm_wday = (int)(((days % 7) + 11) % 7);
    out->tm_yday = (int)(days - days_from_civil(year, 1, 1));
    out->tm_isdst = 0;
    return true;
}

// Fills tm from the leading "YYYY-MM-DDTHH:MM:SS" of timestr. Trailing
// fractional seconds and the 'Z' suffix are ignored.
static bool parse_iso8601_fields(const char *timestr, struct tm *tm) {
    int year, month, day, hour, min, sec;
    if (sscanf(timestr, "%4d-%2d-%2dT%2d:%2d:%2d",
               &year, &month, &day, &hour, &min, &sec) != 6)
        return false;

    tm->tm_year = year - 1900; // years since 1900
    tm->tm_mon = month - 1;    // months since January [0-11]
    tm->tm_mday = day;
    tm->tm_hour = hour;
    tm->tm_min = min;
    tm->tm_sec = sec;
    tm->tm_isdst = 0;
    return true;
}

time_t iso8601_to_utc(const char *timestr) {
    struct tm tm = {0};
    if (!parse_iso8601_fields(timestr, &tm))
        return (time_t)-1;
    return utc_from_tm(&tm);
}

time_t european_date_to_utc(const char *date) {
    struct tm tm = {0};
    int day, month, year;

    if (sscanf(date, "%d.%d.%d", &day, &month, &year) != 3)
        return (time_t)-1;

    // Checked rather than left to roll over: utc_from_tm normalises whatever it
    // is given, so "99.99.2025" would come back as a real instant in 2033. The
    // filter field asks about every date on the way to the one being typed, and
    // a nonsense one has to read as no date rather than as somewhere else.
    if (day < 1 || day > 31 || month < 1 || month > 12 || year < 1 || year > 9999)
        return (time_t)-1;

    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_isdst = 0;
    return utc_from_tm(&tm);
}

bool utc_to_display_strings(time_t utc,
                            char *out_date, size_t date_size,
                            char *out_time, size_t time_size) {
    struct tm tm;
    if (!utc_to_tm(utc, &tm))
        return false;

    strftime(out_date, date_size, "%d.%m.%Y", &tm);
    strftime(out_time, time_size, "%H:%M", &tm);
    return true;
}
