#include "time_util.h"

#include <stdio.h>

// timegm() is the POSIX counterpart to mktime() that reads the struct tm as
// UTC instead of local time. Windows spells it _mkgmtime.
static time_t tm_to_utc(struct tm *tm) {
#if defined(_WIN32) || defined(_WIN64)
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
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
    return tm_to_utc(&tm);
}

time_t european_date_to_utc(const char *date) {
    struct tm tm = {0};
    int day, month, year;

    if (sscanf(date, "%d.%d.%d", &day, &month, &year) != 3)
        return (time_t)-1;

    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_isdst = 0;
    return tm_to_utc(&tm);
}

bool utc_to_display_strings(time_t utc,
                            char *out_date, size_t date_size,
                            char *out_time, size_t time_size) {
    if (utc == (time_t)-1)
        return false;

    struct tm tm;
#if defined(_WIN32) || defined(_WIN64)
    if (gmtime_s(&tm, &utc) != 0)
        return false;
#else
    if (!gmtime_r(&utc, &tm))
        return false;
#endif

    strftime(out_date, date_size, "%d.%m.%Y", &tm);
    strftime(out_time, time_size, "%H:%M", &tm);
    return true;
}
