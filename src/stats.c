#include "stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "time_util.h"

typedef enum StatsReduce {
    STATS_REDUCE_SUM,
    STATS_REDUCE_MAX
} StatsReduce;

typedef enum StatsValueFormat {
    STATS_VALUE_NUMBER,
    STATS_VALUE_HOURS
} StatsValueFormat;

typedef struct StatsMetricDef {
    size_t member;
    StatsReduce reduce;
    const char *label;
    const char *unit;
    StatsValueFormat format;
} StatsMetricDef;

#define MEMBER(field) offsetof(GpxTrack, field)

static const StatsMetricDef metric_defs[STATS_METRIC_COUNT] = {
    [STATS_METRIC_TOTAL_DISTANCE] = {MEMBER(distance), STATS_REDUCE_SUM, "Total distance", "km", STATS_VALUE_NUMBER},
    [STATS_METRIC_LONGEST_RUN] = {MEMBER(distance), STATS_REDUCE_MAX, "Longest run", "km", STATS_VALUE_NUMBER},
    [STATS_METRIC_TOTAL_TIME] = {MEMBER(duration_secs), STATS_REDUCE_SUM, "Total activity time", "h", STATS_VALUE_HOURS},
    [STATS_METRIC_TOTAL_ASCENT] = {MEMBER(elev_up), STATS_REDUCE_SUM, "Total ascended meters", "m", STATS_VALUE_NUMBER},
};

#undef MEMBER

static const char *period_labels[STATS_PERIOD_COUNT] = {
    [STATS_PERIOD_DAY] = "Daily",
    [STATS_PERIOD_WEEK] = "Weekly",
    [STATS_PERIOD_MONTH] = "Monthly",
    [STATS_PERIOD_YEAR] = "Yearly",
};

static bool period_is_valid(StatsPeriod period) {
    return period >= 0 && period < STATS_PERIOD_COUNT;
}

static bool metric_is_valid(StatsMetric metric) {
    return metric >= 0 && metric < STATS_METRIC_COUNT;
}

const char *stats_period_label(StatsPeriod period) {
    return period_is_valid(period) ? period_labels[period] : "";
}

const char *stats_metric_label(StatsMetric metric) {
    return metric_is_valid(metric) ? metric_defs[metric].label : "";
}

const char *stats_metric_unit(StatsMetric metric) {
    return metric_is_valid(metric) ? metric_defs[metric].unit : "";
}

// --- The calendar ---

// Seconds in a day and a week. Only ever used to divide two midnights that are
// already known to be whole days apart -- every timestamp in the application is
// UTC, so no day here is ever the 23 hours a daylight-saving change makes it.
#define SECONDS_PER_DAY 86400
#define SECONDS_PER_WEEK (7 * SECONDS_PER_DAY)

time_t stats_period_start(time_t utc, StatsPeriod period) {
    struct tm tm;
    if (!period_is_valid(period) || !utc_to_tm(utc, &tm))
        return (time_t)-1;

    // Every period begins at midnight, so the time of day always goes.
    // Truncating with utc % SECONDS_PER_DAY would not do: C's % rounds toward
    // zero, so a timestamp before 1970 -- which a GPX with a broken clock
    // produces -- would land on the midnight after it rather than before.
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;

    switch (period) {
    case STATS_PERIOD_DAY:
        break;
    case STATS_PERIOD_WEEK:
        // tm_wday is 0 on Sunday, so (tm_wday + 6) % 7 is days since Monday.
        // Stepping back over the start of a month leaves tm_mday zero or
        // negative, which is exactly what timegm normalises.
        tm.tm_mday -= (tm.tm_wday + 6) % 7;
        break;
    case STATS_PERIOD_MONTH:
        tm.tm_mday = 1;
        break;
    case STATS_PERIOD_YEAR:
        tm.tm_mday = 1;
        tm.tm_mon = 0;
        break;
    default:
        return (time_t)-1;
    }

    return utc_from_tm(&tm);
}

time_t stats_period_step(time_t start, StatsPeriod period, int count) {
    struct tm tm;
    if (!period_is_valid(period) || !utc_to_tm(start, &tm))
        return (time_t)-1;

    // Carried through timegm rather than added as seconds: a month is whatever
    // number of days the calendar says it is, and February 2024 is not
    // February 2025.
    switch (period) {
    case STATS_PERIOD_DAY:
        tm.tm_mday += count;
        break;
    case STATS_PERIOD_WEEK:
        tm.tm_mday += 7 * count;
        break;
    case STATS_PERIOD_MONTH:
        tm.tm_mon += count;
        break;
    case STATS_PERIOD_YEAR:
        tm.tm_year += count;
        break;
    default:
        return (time_t)-1;
    }

    return utc_from_tm(&tm);
}

int stats_period_distance(time_t older_start, time_t newer_start, StatsPeriod period) {
    if (older_start == (time_t)-1 || newer_start == (time_t)-1)
        return 0;

    // Days and weeks are a fixed number of seconds apart and both arguments are
    // midnights, so the division is exact. Months and years have to be counted
    // rather than measured.
    switch (period) {
    case STATS_PERIOD_DAY:
        return (int)((newer_start - older_start) / SECONDS_PER_DAY);
    case STATS_PERIOD_WEEK:
        return (int)((newer_start - older_start) / SECONDS_PER_WEEK);
    case STATS_PERIOD_MONTH:
    case STATS_PERIOD_YEAR: {
        struct tm older, newer;
        if (!utc_to_tm(older_start, &older) || !utc_to_tm(newer_start, &newer))
            return 0;
        int years = newer.tm_year - older.tm_year;
        if (period == STATS_PERIOD_YEAR)
            return years;
        return years * 12 + (newer.tm_mon - older.tm_mon);
    }
    default:
        return 0;
    }
}

// --- Formatting ---

// Shortened past a thousand, because an axis 56 pixels wide has no room for
// "12480" and no use for the last three digits of it either.
static void format_number(float value, char *out, size_t size) {
    if (value < 10.0f)
        snprintf(out, size, "%.1f", (double)value);
    else if (value < 1000.0f)
        snprintf(out, size, "%.0f", (double)value);
    else
        snprintf(out, size, "%.1fk", (double)value / 1000.0);
}

void stats_format_value(StatsMetric metric, float value, char *out, size_t size) {
    if (size == 0)
        return;
    if (!metric_is_valid(metric)) {
        out[0] = '\0';
        return;
    }

    if (metric_defs[metric].format == STATS_VALUE_HOURS) {
        // Under an hour there are no whole hours to show, and "0.4" of one is
        // not how anyone reads a training week.
        if (value < 3600.0f) {
            snprintf(out, size, "%d min", (int)(value / 60.0f));
            return;
        }
        format_number(value / 3600.0f, out, size);
        return;
    }

    format_number(value, out, size);
}

// --- Building the series ---

// Reads the GpxTrack member a metric row points at. Every one of them is a
// float, but taken through memcpy rather than a cast so the offset does not
// have to be an aligned one.
static float track_metric_value(const GpxTrack *track, size_t member_offset) {
    float value;
    memcpy(&value, (const char *)track + member_offset, sizeof(value));
    return value;
}

// Whether a track is one of the ones being counted. A track the filter panel
// has hidden is not, so the plot and the run list always agree; a track the
// parser found no timestamp on has no period to belong to.
static bool track_counts(const GpxTrack *track) {
    return track->visible_in_list && track->start_utc != (time_t)-1;
}

static bool series_reserve(StatsSeries *series, int count) {
    if (count <= series->capacity)
        return true;

    StatsBucket *grown = realloc(series->buckets, (size_t)count * sizeof(*grown));
    if (!grown)
        return false;

    series->buckets = grown;
    series->capacity = count;
    return true;
}

// Leaves the series empty rather than stale: a caller that ignores the false
// draws nothing, which is right, instead of last time's bars.
static bool series_empty(StatsSeries *series, StatsPeriod period) {
    series->count = 0;
    series->period = period;
    return false;
}

bool stats_build(StatsSeries *series, const GpxCollection *collection,
                 StatsPeriod period) {
    if (!series || !collection || !period_is_valid(period))
        return series ? series_empty(series, period) : false;

    // First pass: the two ends of the axis.
    time_t oldest = 0, newest = 0;
    bool any = false;
    for (int i = 0; i < collection->total_tracks; i++) {
        const GpxTrack *track = &collection->tracks[i];
        if (!track_counts(track))
            continue;

        time_t start = stats_period_start(track->start_utc, period);
        if (start == (time_t)-1)
            continue;

        if (!any || start < oldest)
            oldest = start;
        if (!any || start > newest)
            newest = start;
        any = true;
    }

    if (!any)
        return series_empty(series, period);

    int count = stats_period_distance(oldest, newest, period) + 1;
    // A single track dated 1970 would otherwise ask for twenty thousand daily
    // buckets. The data being wrong is not something this can fix, but it is
    // not a reason to allocate the mistake either.
    if (count < 1 || count > STATS_MAX_BUCKETS)
        return series_empty(series, period);

    if (!series_reserve(series, count))
        return series_empty(series, period);

    // Laying the axis out before anything is added to it is what makes the
    // periods nothing happened in exist. They are the gaps in the training, and
    // a plot that closed them up would be lying about the calendar.
    memset(series->buckets, 0, (size_t)count * sizeof(*series->buckets));
    for (int i = 0; i < count; i++)
        series->buckets[i].start_utc = stats_period_step(oldest, period, i);

    series->count = count;
    series->period = period;

    // Second pass: every metric of every track into the bucket it falls in.
    for (int i = 0; i < collection->total_tracks; i++) {
        const GpxTrack *track = &collection->tracks[i];
        if (!track_counts(track))
            continue;

        time_t start = stats_period_start(track->start_utc, period);
        if (start == (time_t)-1)
            continue;

        int index = stats_period_distance(oldest, start, period);
        if (index < 0 || index >= count)
            continue;

        StatsBucket *bucket = &series->buckets[index];
        for (int m = 0; m < STATS_METRIC_COUNT; m++) {
            float value = track_metric_value(track, metric_defs[m].member);
            if (metric_defs[m].reduce == STATS_REDUCE_MAX) {
                if (value > bucket->value[m])
                    bucket->value[m] = value;
            } else {
                bucket->value[m] += value;
            }
        }
        bucket->track_count++;
    }

    return true;
}

void stats_free(StatsSeries *series) {
    if (!series)
        return;
    free(series->buckets);
    series->buckets = NULL;
    series->count = 0;
    series->capacity = 0;
}

float stats_max(const StatsSeries *series, StatsMetric metric, int first, int count) {
    if (!series || !metric_is_valid(metric))
        return 0.0f;

    if (first < 0) {
        count += first;
        first = 0;
    }
    if (first + count > series->count)
        count = series->count - first;

    float max = 0.0f;
    for (int i = 0; i < count; i++) {
        float value = series->buckets[first + i].value[metric];
        if (value > max)
            max = value;
    }
    return max;
}
