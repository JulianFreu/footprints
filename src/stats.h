#ifndef STATS_H
#define STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "gpx_types.h"

// Aggregation across tracks: what a day, a week, a month or a year of activity
// adds up to. The rest of the application asks about one track at a time --
// track_format renders a single run, the run list a row per run -- so this is
// the only place a question is asked of the collection as a whole.
//
// Nothing here knows about SDL, Clay or the panel that draws it; the panel is
// src/ui_stats.c. That split is what puts the calendar arithmetic, which is
// where the corners are, under the test suite.

// The x axis, at four resolutions.
typedef enum StatsPeriod {
    STATS_PERIOD_DAY = 0,
    STATS_PERIOD_WEEK,
    STATS_PERIOD_MONTH,
    STATS_PERIOD_YEAR,
    STATS_PERIOD_COUNT
} StatsPeriod;

// The y axis. Every one of these is reduced from a single float member of
// GpxTrack; which member and how it is reduced is the table in stats.c.
typedef enum StatsMetric {
    STATS_METRIC_TOTAL_DISTANCE = 0, // km, summed
    STATS_METRIC_LONGEST_RUN,        // km, the largest single track
    STATS_METRIC_TOTAL_TIME,         // seconds, summed
    STATS_METRIC_TOTAL_ASCENT,       // metres, summed
    STATS_METRIC_COUNT
} StatsMetric;

typedef struct StatsBucket {
    // 00:00:00 UTC on the period's first day.
    time_t start_utc;
    // Every metric, filled in the same pass. Computing all four costs one more
    // add per track and means switching which are drawn needs no rebuild.
    float value[STATS_METRIC_COUNT];
    // Zero marks a period nothing happened in, which is a bar of height zero
    // rather than a bucket that is missing.
    int track_count;
} StatsBucket;

typedef struct StatsSeries {
    // Oldest first; buckets[count - 1] is the newest. Every period between the
    // two ends is present, including the empty ones: the x axis is a calendar,
    // not a list of the days that had a run on them.
    StatsBucket *buckets;
    int count;
    // Allocated slots, kept between rebuilds so the common case allocates
    // nothing.
    int capacity;
    StatsPeriod period;
} StatsSeries;

// Longest series that will be built. A GPX file whose timestamps are wrong by
// decades would otherwise ask for an allocation the size of the mistake.
#define STATS_MAX_BUCKETS 100000

// Room for the string stats_format_value writes.
#define STATS_VALUE_MAX 16

// The instant the period containing `utc` begins, at 00:00:00 UTC. Weeks begin
// on Monday, ISO 8601's reckoning and the one the "KW nn" label counts by.
// (time_t)-1 passes through unchanged, so a track the parser found no time on
// stays out of the series rather than landing in 1970.
time_t stats_period_start(time_t utc, StatsPeriod period);

// The start of the period `count` periods after the one beginning at `start`.
// Negative counts step back. Months and years vary in length, so this is
// calendar arithmetic rather than a multiplication.
time_t stats_period_step(time_t start, StatsPeriod period, int count);

// How many whole periods separate two period starts, positive when `newer` is
// the later of the two. This is what puts a track in its bucket in one step
// instead of walking the axis.
int stats_period_distance(time_t older_start, time_t newer_start, StatsPeriod period);

// "Daily", "Weekly", "Monthly", "Yearly" -- what the timescale buttons say.
const char *stats_period_label(StatsPeriod period);

// "Total distance" and so on, and the unit its axis is measured in.
const char *stats_metric_label(StatsMetric metric);
const char *stats_metric_unit(StatsMetric metric);

// The value written the way an axis shows it: shortened past a thousand, and
// in whole hours for a duration. A single track's duration is shown as
// hh:mm:ss, but a year of them reads "1247:33:09", which is not a number
// anyone can take in at a glance.
void stats_format_value(StatsMetric metric, float value, char *out, size_t size);

// Buckets every track with visible_in_list into consecutive periods and reduces
// each metric over them. The series' buffer is reused and only ever grown.
// False means there was nothing to show or the allocation failed, and leaves
// the series empty rather than stale.
bool stats_build(StatsSeries *series, const GpxCollection *collection,
                 StatsPeriod period);

// Releases the buffer. Nothing in the series survives it.
void stats_free(StatsSeries *series);

// The largest value of `metric` over buckets [first, first + count), clamped to
// the series, or 0 if that range holds nothing. Each metric is scaled by its
// own maximum, which is what lets two of them share a plot and still each be
// read off an axis of its own.
float stats_max(const StatsSeries *series, StatsMetric metric, int first, int count);

#endif
