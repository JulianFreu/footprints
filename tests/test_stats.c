#include "../src/stats.c"

#include "harness.h"

// The calendar is where the corners are: weeks that start on a Monday and cross
// a month end, a February that has 29 days once every four years, a December
// that is followed by a January. All of it is exercised here rather than by
// looking at the plot and deciding the bars look about right.

static time_t at(const char *iso) {
    return iso8601_to_utc(iso);
}

// One track, dated, with the four numbers the metrics reduce.
static GpxTrack track_at(const char *iso, float distance, float duration,
                         float elev_up) {
    GpxTrack track = {0};
    track.start_utc = at(iso);
    track.end_utc = track.start_utc + (time_t)duration;
    track.distance = distance;
    track.duration_secs = duration;
    track.elev_up = elev_up;
    track.visible_in_list = true;
    return track;
}

void run_stats_tests(void) {
    char text[STATS_VALUE_MAX];

    SUITE("stats: every period and metric is described");
    // A missing row would be a designated-initialiser hole: a metric with no
    // label and an offset of zero, which reads whatever member comes first.
    for (int p = 0; p < STATS_PERIOD_COUNT; p++)
        CHECK(stats_period_label((StatsPeriod)p)[0] != '\0');
    for (int m = 0; m < STATS_METRIC_COUNT; m++) {
        CHECK(stats_metric_label((StatsMetric)m)[0] != '\0');
        CHECK(stats_metric_unit((StatsMetric)m)[0] != '\0');
        CHECK(metric_defs[m].member != 0 || m == 0);
    }

    SUITE("stats: a day begins at midnight");
    CHECK_INT(stats_period_start(at("2025-03-09T23:59:59Z"), STATS_PERIOD_DAY),
              at("2025-03-09T00:00:00Z"));
    CHECK_INT(stats_period_start(at("2025-03-09T00:00:00Z"), STATS_PERIOD_DAY),
              at("2025-03-09T00:00:00Z"));
    // One second either side of midnight is two different days.
    CHECK(stats_period_start(at("2025-03-08T23:59:59Z"), STATS_PERIOD_DAY) !=
          stats_period_start(at("2025-03-09T00:00:00Z"), STATS_PERIOD_DAY));

    SUITE("stats: a week begins on the Monday, across a month end");
    // Mon 27.10.2025 through Sun 02.11.2025 is one week, and stepping back to
    // its Monday from November leaves tm_mday negative for timegm to normalise.
    const char *week_days[] = {
        "2025-10-27T06:00:00Z", "2025-10-28T06:00:00Z", "2025-10-29T06:00:00Z",
        "2025-10-30T06:00:00Z", "2025-10-31T06:00:00Z", "2025-11-01T06:00:00Z",
        // Sunday, the tm_wday == 0 case the (wday + 6) % 7 exists for.
        "2025-11-02T23:00:00Z"};
    for (size_t i = 0; i < sizeof(week_days) / sizeof(week_days[0]); i++)
        CHECK_INT(stats_period_start(at(week_days[i]), STATS_PERIOD_WEEK),
                  at("2025-10-27T00:00:00Z"));
    // The Monday after is a different week.
    CHECK_INT(stats_period_start(at("2025-11-03T00:00:00Z"), STATS_PERIOD_WEEK),
              at("2025-11-03T00:00:00Z"));

    SUITE("stats: months and years, including a leap day");
    CHECK_INT(stats_period_start(at("2024-02-29T12:00:00Z"), STATS_PERIOD_MONTH),
              at("2024-02-01T00:00:00Z"));
    CHECK_INT(stats_period_start(at("2024-02-29T12:00:00Z"), STATS_PERIOD_YEAR),
              at("2024-01-01T00:00:00Z"));
    CHECK_INT(stats_period_start(at("2025-12-31T23:59:59Z"), STATS_PERIOD_YEAR),
              at("2025-01-01T00:00:00Z"));
    CHECK_INT(stats_period_start(at("2026-01-01T00:00:00Z"), STATS_PERIOD_YEAR),
              at("2026-01-01T00:00:00Z"));

    SUITE("stats: a month is as long as the calendar says");
    // February 2024 has 29 days, so a month is never a fixed number of seconds.
    time_t feb_2024 = at("2024-02-01T00:00:00Z");
    time_t mar_2024 = stats_period_step(feb_2024, STATS_PERIOD_MONTH, 1);
    CHECK_INT(mar_2024, at("2024-03-01T00:00:00Z"));
    CHECK_INT(mar_2024 - feb_2024, 29 * 86400);
    CHECK_INT(stats_period_step(mar_2024, STATS_PERIOD_MONTH, -1), feb_2024);
    // February 2025 has 28.
    CHECK_INT(stats_period_step(at("2025-02-01T00:00:00Z"), STATS_PERIOD_MONTH, 1) -
                  at("2025-02-01T00:00:00Z"),
              28 * 86400);

    SUITE("stats: stepping over a year boundary");
    CHECK_INT(stats_period_step(at("2025-12-01T00:00:00Z"), STATS_PERIOD_MONTH, 1),
              at("2026-01-01T00:00:00Z"));
    CHECK_INT(stats_period_distance(at("2025-12-01T00:00:00Z"),
                                    at("2026-01-01T00:00:00Z"), STATS_PERIOD_MONTH),
              1);
    CHECK_INT(stats_period_step(at("2025-12-29T00:00:00Z"), STATS_PERIOD_WEEK, 1),
              at("2026-01-05T00:00:00Z"));

    SUITE("stats: distance is the inverse of step");
    // The whole bucket index rests on this, so it is checked as the property it
    // is rather than at a handful of dates.
    for (int p = 0; p < STATS_PERIOD_COUNT; p++) {
        time_t origin = stats_period_start(at("2021-07-14T09:30:00Z"), (StatsPeriod)p);
        for (int n = -200; n <= 200; n++) {
            time_t stepped = stats_period_step(origin, (StatsPeriod)p, n);
            CHECK_INT(stats_period_distance(origin, stepped, (StatsPeriod)p), n);
            // And a stepped-to period start is already a period start.
            CHECK_INT(stats_period_start(stepped, (StatsPeriod)p), stepped);
        }
    }

    SUITE("stats: values are written the way an axis shows them");
    stats_format_value(STATS_METRIC_TOTAL_DISTANCE, 8.25f, text, sizeof(text));
    CHECK_STR(text, "8.2");
    stats_format_value(STATS_METRIC_TOTAL_DISTANCE, 128.0f, text, sizeof(text));
    CHECK_STR(text, "128");
    stats_format_value(STATS_METRIC_TOTAL_ASCENT, 12480.0f, text, sizeof(text));
    CHECK_STR(text, "12.5k");
    // Under an hour there are no whole hours to show.
    stats_format_value(STATS_METRIC_TOTAL_TIME, 3599.0f, text, sizeof(text));
    CHECK_STR(text, "59 min");
    stats_format_value(STATS_METRIC_TOTAL_TIME, 3600.0f, text, sizeof(text));
    CHECK_STR(text, "1.0");
    // A month of running: track_format would have made this "104:10:00".
    stats_format_value(STATS_METRIC_TOTAL_TIME, 375000.0f, text, sizeof(text));
    CHECK_STR(text, "104");
    // A year of it shortens like any other big number, rather than becoming the
    // "1247:33:09" a single track's format would have produced.
    stats_format_value(STATS_METRIC_TOTAL_TIME, 4491189.0f, text, sizeof(text));
    CHECK_STR(text, "1.2k");

    SUITE("stats: the periods nothing happened in are still on the axis");
    GpxTrack gap_tracks[2] = {
        track_at("2025-03-01T08:00:00Z", 10.0f, 3600.0f, 100.0f),
        track_at("2025-03-06T08:00:00Z", 5.0f, 1800.0f, 50.0f)};
    GpxCollection gaps = {0};
    gaps.tracks = gap_tracks;
    gaps.total_tracks = 2;

    StatsSeries series = {0};
    CHECK(stats_build(&series, &gaps, STATS_PERIOD_DAY));
    CHECK_INT(series.count, 6);
    CHECK_INT(series.buckets[0].track_count, 1);
    CHECK_INT(series.buckets[5].track_count, 1);
    for (int i = 1; i <= 4; i++) {
        CHECK_INT(series.buckets[i].track_count, 0);
        for (int m = 0; m < STATS_METRIC_COUNT; m++)
            CHECK_NEAR(series.buckets[i].value[m], 0.0, 1e-6);
    }
    for (int i = 1; i < series.count; i++)
        CHECK_INT(series.buckets[i].start_utc - series.buckets[i - 1].start_utc, 86400);

    SUITE("stats: metrics are summed, except the longest run");
    GpxTrack week_tracks[4] = {
        // Three in the week of Mon 27.10.2025 ...
        track_at("2025-10-27T08:00:00Z", 10.0f, 3600.0f, 100.0f),
        track_at("2025-10-29T08:00:00Z", 21.0f, 7200.0f, 250.0f),
        track_at("2025-11-02T08:00:00Z", 5.0f, 1500.0f, 20.0f),
        // ... and one in the week after.
        track_at("2025-11-04T08:00:00Z", 8.0f, 2400.0f, 60.0f)};
    GpxCollection weeks = {0};
    weeks.tracks = week_tracks;
    weeks.total_tracks = 4;

    CHECK(stats_build(&series, &weeks, STATS_PERIOD_WEEK));
    CHECK_INT(series.count, 2);
    CHECK_INT(series.buckets[0].start_utc, at("2025-10-27T00:00:00Z"));
    CHECK_INT(series.buckets[0].track_count, 3);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_DISTANCE], 36.0, 1e-4);
    // The largest, not the sum and not the last one added.
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_LONGEST_RUN], 21.0, 1e-4);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_TIME], 12300.0, 1e-2);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_ASCENT], 370.0, 1e-4);
    CHECK_INT(series.buckets[1].track_count, 1);
    CHECK_NEAR(series.buckets[1].value[STATS_METRIC_LONGEST_RUN], 8.0, 1e-4);

    SUITE("stats: monthly and yearly reduce the same tracks");
    CHECK(stats_build(&series, &weeks, STATS_PERIOD_MONTH));
    CHECK_INT(series.count, 2); // October and November
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_DISTANCE], 31.0, 1e-4);
    CHECK_NEAR(series.buckets[1].value[STATS_METRIC_TOTAL_DISTANCE], 13.0, 1e-4);
    CHECK(stats_build(&series, &weeks, STATS_PERIOD_YEAR));
    CHECK_INT(series.count, 1);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_DISTANCE], 44.0, 1e-4);

    SUITE("stats: a hidden track is not counted");
    // The middle one of the three, so the bucket stays and only shrinks.
    week_tracks[1].visible_in_list = false;
    CHECK(stats_build(&series, &weeks, STATS_PERIOD_WEEK));
    CHECK_INT(series.count, 2);
    CHECK_INT(series.buckets[0].track_count, 2);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_DISTANCE], 15.0, 1e-4);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_LONGEST_RUN], 10.0, 1e-4);
    week_tracks[1].visible_in_list = true;

    SUITE("stats: hiding the newest track shortens the axis");
    week_tracks[3].visible_in_list = false;
    CHECK(stats_build(&series, &weeks, STATS_PERIOD_WEEK));
    CHECK_INT(series.count, 1);
    CHECK_INT(series.buckets[0].start_utc, at("2025-10-27T00:00:00Z"));
    week_tracks[3].visible_in_list = true;

    SUITE("stats: a bucket left empty by the filter is still on the axis");
    // Hide the only track of the first week: the series now runs from the
    // second week alone, so hide a middle one instead by making three weeks.
    GpxTrack spread[3] = {
        track_at("2025-01-06T08:00:00Z", 10.0f, 3600.0f, 100.0f),
        track_at("2025-01-13T08:00:00Z", 12.0f, 3600.0f, 100.0f),
        track_at("2025-01-20T08:00:00Z", 14.0f, 3600.0f, 100.0f)};
    GpxCollection three = {0};
    three.tracks = spread;
    three.total_tracks = 3;
    spread[1].visible_in_list = false;
    CHECK(stats_build(&series, &three, STATS_PERIOD_WEEK));
    CHECK_INT(series.count, 3);
    CHECK_INT(series.buckets[1].track_count, 0);
    CHECK_NEAR(series.buckets[1].value[STATS_METRIC_TOTAL_DISTANCE], 0.0, 1e-6);
    spread[1].visible_in_list = true;

    SUITE("stats: a track with no timestamp is left out");
    GpxTrack undated[2] = {
        track_at("2025-03-01T08:00:00Z", 10.0f, 3600.0f, 100.0f),
        track_at("2025-03-02T08:00:00Z", 7.0f, 1800.0f, 40.0f)};
    // What gpx_parse_file leaves behind when no <trkpt> carried a time. One of
    // these would otherwise stretch the axis back to 1969.
    undated[1].start_utc = (time_t)-1;
    GpxCollection dateless = {0};
    dateless.tracks = undated;
    dateless.total_tracks = 2;
    CHECK(stats_build(&series, &dateless, STATS_PERIOD_DAY));
    CHECK_INT(series.count, 1);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_DISTANCE], 10.0, 1e-4);

    SUITE("stats: an activity with no path is counted like any other");
    // A treadmill run, as gpx_parse_file leaves it: dated and with its totals,
    // but no points and nowhere on the map. The panel counts a track by its
    // numbers, not by whether it went anywhere, and this is that contract.
    GpxTrack indoor[2] = {
        track_at("2025-04-07T18:00:00Z", 8.0f, 2700.0f, 0.0f),
        track_at("2025-04-07T06:00:00Z", 12.0f, 3600.0f, 250.0f)};
    indoor[0].total_points = 0;
    indoor[0].has_path = false;
    indoor[1].has_path = true;
    GpxCollection mixed = {0};
    mixed.tracks = indoor;
    mixed.total_tracks = 2;
    CHECK(stats_build(&series, &mixed, STATS_PERIOD_DAY));
    CHECK_INT(series.count, 1);
    CHECK_INT(series.buckets[0].track_count, 2);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_DISTANCE], 20.0, 1e-4);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_TIME], 6300.0, 1e-4);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_TOTAL_ASCENT], 250.0, 1e-4);
    CHECK_NEAR(series.buckets[0].value[STATS_METRIC_LONGEST_RUN], 12.0, 1e-4);

    SUITE("stats: nothing to show leaves the series empty, not stale");
    CHECK(stats_build(&series, &weeks, STATS_PERIOD_WEEK));
    CHECK_INT(series.count, 2);
    for (int i = 0; i < 4; i++)
        week_tracks[i].visible_in_list = false;
    CHECK(!stats_build(&series, &weeks, STATS_PERIOD_WEEK));
    CHECK_INT(series.count, 0);
    GpxCollection nothing = {0};
    CHECK(!stats_build(&series, &nothing, STATS_PERIOD_DAY));
    CHECK_INT(series.count, 0);
    for (int i = 0; i < 4; i++)
        week_tracks[i].visible_in_list = true;

    SUITE("stats: the buffer is grown and then reused");
    CHECK(stats_build(&series, &gaps, STATS_PERIOD_DAY));
    CHECK_INT(series.count, 6);
    int grown_capacity = series.capacity;
    CHECK(grown_capacity >= 6);
    // A coarser scale needs fewer buckets, so nothing is reallocated ...
    CHECK(stats_build(&series, &gaps, STATS_PERIOD_YEAR));
    CHECK_INT(series.count, 1);
    CHECK_INT(series.capacity, grown_capacity);
    // ... and going back is still correct rather than reading last time's tail.
    CHECK(stats_build(&series, &gaps, STATS_PERIOD_DAY));
    CHECK_INT(series.count, 6);
    CHECK_INT(series.buckets[3].track_count, 0);

    SUITE("stats: the maximum over the window the plot draws");
    CHECK(stats_build(&series, &weeks, STATS_PERIOD_WEEK));
    CHECK_NEAR(stats_max(&series, STATS_METRIC_TOTAL_DISTANCE, 0, 2), 36.0, 1e-4);
    CHECK_NEAR(stats_max(&series, STATS_METRIC_TOTAL_DISTANCE, 1, 1), 8.0, 1e-4);
    // A range that runs off either end is clamped rather than read past.
    CHECK_NEAR(stats_max(&series, STATS_METRIC_TOTAL_DISTANCE, 1, 99), 8.0, 1e-4);
    CHECK_NEAR(stats_max(&series, STATS_METRIC_TOTAL_DISTANCE, -5, 99), 36.0, 1e-4);
    CHECK_NEAR(stats_max(&series, STATS_METRIC_TOTAL_DISTANCE, 9, 3), 0.0, 1e-6);

    stats_free(&series);
    CHECK(series.buckets == NULL);
    CHECK_INT(series.capacity, 0);
}
