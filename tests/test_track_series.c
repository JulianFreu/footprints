#include "../src/track_series.c"

#include "harness.h"

#include <stdlib.h>

// The pace window is where the corners are: it is measured across a fixed
// stretch of ground rather than between two fixes, so what it does at the ends
// of a track, across a gap in the timestamps, and on a track shorter than the
// window itself are all worth pinning down. Synthetic tracks throughout, where
// the right answer is arithmetic rather than a judgement about whether the
// graph looks about right.

// A track laid out at a fixed spacing, with a time and a heart rate per point.
// A negative second means the point carried no time, and a zero heart rate
// means it carried none -- which is how the parser spells both.
typedef struct Sample {
    float metres;  // from the start of the track
    float seconds; // from the start of the track, negative for "no time"
    uint16_t bpm;
} Sample;

static GpxTrack build(const Sample *samples, int count) {
    GpxTrack track = {0};
    track.points = calloc((size_t)count, sizeof(GpxPoint));
    track.total_points = count;
    track.points_capacity = count;

    for (int i = 0; i < count; i++) {
        track.points[i].partial_distance = samples[i].metres;
        track.points[i].elapsed_secs = samples[i].seconds < 0.0f ? NAN : samples[i].seconds;
        track.points[i].heart_rate = samples[i].bpm;
        track.points[i].elevation = 100.0f + (float)i;
    }
    return track;
}

// A steady runner: `count` points, `spacing` metres apart, at `pace` seconds
// per kilometre.
static GpxTrack build_steady(int count, float spacing, float pace, uint16_t bpm) {
    Sample *samples = calloc((size_t)count, sizeof(Sample));
    for (int i = 0; i < count; i++) {
        samples[i].metres = spacing * (float)i;
        samples[i].seconds = samples[i].metres / 1000.0f * pace;
        samples[i].bpm = bpm;
    }
    GpxTrack track = build(samples, count);
    free(samples);
    return track;
}

static void destroy(GpxTrack *track) {
    free(track->points);
    track->points = NULL;
}

void run_track_series_tests(void) {
    TrackSeries series;

    SUITE("track_series: pace over a steady run");
    {
        // Fifty points ten metres apart is half a kilometre, which is well
        // past the hundred-metre window.
        GpxTrack track = build_steady(50, 10.0f, 300.0f, 150);
        CHECK(track_series_build(&track, TRACK_SERIES_PACE, &series));
        CHECK(series.present);
        CHECK(series.invert); // quickest at the top
        CHECK_INT(series.count, 50);
        // Every point, including the two the window is clamped at, reads the
        // pace the runner actually held.
        for (int i = 0; i < series.count; i++)
            CHECK_NEAR(series.values[i], 300.0, 0.01);
        CHECK_NEAR(series.min, 300.0, 0.01);
        CHECK_NEAR(series.max, 300.0, 0.01);
        track_series_free(&series);
        destroy(&track);
    }

    SUITE("track_series: the window spans at least PACE_WINDOW_METERS");
    {
        // A sample every two hundred metres, so the window cannot be narrower
        // than one segment however little it is asked for.
        GpxTrack track = build_steady(10, 200.0f, 360.0f, 0);
        CHECK(track_series_build(&track, TRACK_SERIES_PACE, &series));
        for (int i = 0; i < series.count; i++)
            CHECK_NEAR(series.values[i], 360.0, 0.01);
        track_series_free(&series);
        destroy(&track);
    }

    SUITE("track_series: a track shorter than the window is measured end to end");
    {
        // Thirty metres in total, against a hundred-metre window.
        const Sample samples[] = {{0.0f, 0.0f, 0}, {15.0f, 6.0f, 0}, {30.0f, 12.0f, 0}};
        GpxTrack track = build(samples, 3);
        CHECK(track_series_build(&track, TRACK_SERIES_PACE, &series));
        // 30 m in 12 s is 400 seconds per kilometre.
        for (int i = 0; i < series.count; i++)
            CHECK_NEAR(series.values[i], 400.0, 0.01);
        track_series_free(&series);
        destroy(&track);
    }

    SUITE("track_series: untimed points leave a gap rather than a wild number");
    {
        // A route drawn on a map rather than recorded: distances but no times.
        Sample samples[20];
        for (int i = 0; i < 20; i++) {
            samples[i].metres = 20.0f * (float)i;
            samples[i].seconds = -1.0f;
            samples[i].bpm = 0;
        }
        GpxTrack track = build(samples, 20);
        CHECK(!track_series_build(&track, TRACK_SERIES_PACE, &series));
        CHECK(!series.present);
        for (int i = 0; i < series.count; i++)
            CHECK(isnan(series.values[i]));
        track_series_free(&series);
        destroy(&track);
    }

    SUITE("track_series: a stretch above the speed ceiling is left out");
    {
        // Half a kilometre at a plausible pace, then a jump of five kilometres
        // in ten seconds -- the drive home the record search is guarded against.
        Sample samples[30];
        for (int i = 0; i < 25; i++) {
            samples[i].metres = 20.0f * (float)i;
            samples[i].seconds = samples[i].metres / 1000.0f * 300.0f;
            samples[i].bpm = 0;
        }
        for (int i = 25; i < 30; i++) {
            samples[i].metres = 500.0f + 5000.0f * (float)(i - 24);
            samples[i].seconds = 144.0f + 10.0f * (float)(i - 24);
            samples[i].bpm = 0;
        }
        GpxTrack track = build(samples, 30);
        CHECK(track_series_build(&track, TRACK_SERIES_PACE, &series));
        // The honest first half still reads its own pace...
        CHECK_NEAR(series.values[5], 300.0, 0.01);
        // ...and the drive is a gap, not a two-second kilometre that would
        // flatten everything else into the bottom of the graph.
        CHECK(isnan(series.values[29]));
        CHECK(series.min >= 100.0f);
        track_series_free(&series);
        destroy(&track);
    }

    SUITE("track_series: heart rate present and absent");
    {
        GpxTrack with_hr = build_steady(20, 20.0f, 300.0f, 0);
        // Two of the twenty carried a reading; the rest are gaps, and the
        // range is taken over the two that are there.
        with_hr.points[3].heart_rate = 120;
        with_hr.points[9].heart_rate = 176;
        CHECK(track_series_build(&with_hr, TRACK_SERIES_HEART_RATE, &series));
        CHECK(series.present);
        CHECK_NEAR(series.min, 120.0, 0.001);
        CHECK_NEAR(series.max, 176.0, 0.001);
        CHECK(isnan(series.values[0]));
        CHECK_NEAR(series.values[3], 120.0, 0.001);
        track_series_free(&series);
        destroy(&with_hr);

        GpxTrack without_hr = build_steady(20, 20.0f, 300.0f, 0);
        CHECK(!track_series_build(&without_hr, TRACK_SERIES_HEART_RATE, &series));
        CHECK(!series.present);
        track_series_free(&series);
        destroy(&without_hr);
    }

    SUITE("track_series: elevation comes through as it is");
    {
        GpxTrack track = build_steady(10, 20.0f, 300.0f, 0);
        CHECK(track_series_build(&track, TRACK_SERIES_ELEVATION, &series));
        CHECK(!series.invert);
        CHECK_NEAR(series.min, 100.0, 0.001);
        CHECK_NEAR(series.max, 109.0, 0.001);
        track_series_free(&series);
        destroy(&track);
    }

    SUITE("track_series: a track too short to draw");
    {
        const Sample samples[] = {{0.0f, 0.0f, 100}};
        GpxTrack track = build(samples, 1);
        CHECK(!track_series_build(&track, TRACK_SERIES_ELEVATION, &series));
        // Safe to free whatever the build did or did not allocate.
        track_series_free(&series);
        destroy(&track);
    }

    SUITE("track_series: the drawn range has a floor under it");
    {
        // A run held at one pace: the samples span a fraction of a second per
        // kilometre, and drawing that across the whole box would turn the noise
        // on it into a mountain range.
        GpxTrack steady = build_steady(50, 10.0f, 300.0f, 0);
        CHECK(track_series_build(&steady, TRACK_SERIES_PACE, &series));
        float min, max;
        track_series_range(&series, &min, &max);
        CHECK(max - min >= 60.0f);
        // Centred on the pace actually held, rather than pinned to one edge.
        CHECK_NEAR((min + max) / 2.0f, 300.0, 0.5);
        track_series_free(&series);
        destroy(&steady);

        // A series with a real spread keeps it, plus the headroom at each end.
        GpxTrack varied = build_steady(20, 20.0f, 300.0f, 0);
        for (int i = 0; i < varied.total_points; i++)
            varied.points[i].heart_rate = (uint16_t)(100 + 5 * i);
        CHECK(track_series_build(&varied, TRACK_SERIES_HEART_RATE, &series));
        track_series_range(&series, &min, &max);
        CHECK(min < 100.0f);
        CHECK(max > 195.0f);
        CHECK_NEAR(max - min, (195.0 - 100.0) * 1.2, 0.01);
        track_series_free(&series);
        destroy(&varied);
    }

    SUITE("track_series: index_at_fraction");
    {
        GpxTrack track = build_steady(11, 100.0f, 300.0f, 0); // 0..1000 m
        CHECK_INT(track_series_index_at_fraction(&track, 0.0f), 0);
        CHECK_INT(track_series_index_at_fraction(&track, 1.0f), 10);
        CHECK_INT(track_series_index_at_fraction(&track, 0.5f), 5);
        // Nearest, not the one after: 0.46 is 460 m, which is closer to the
        // point at 500 than to the one at 400.
        CHECK_INT(track_series_index_at_fraction(&track, 0.46f), 5);
        CHECK_INT(track_series_index_at_fraction(&track, 0.44f), 4);
        // Clamped rather than indexing past either end.
        CHECK_INT(track_series_index_at_fraction(&track, -3.0f), 0);
        CHECK_INT(track_series_index_at_fraction(&track, 7.0f), 10);
        destroy(&track);

        // A recording that never moved has nowhere to point at but its start.
        GpxTrack stationary = build_steady(5, 0.0f, 300.0f, 0);
        CHECK_INT(track_series_index_at_fraction(&stationary, 0.7f), 0);
        destroy(&stationary);

        CHECK_INT(track_series_index_at_fraction(NULL, 0.5f), -1);
    }

    SUITE("track_series: display forms");
    {
        char text[TRACK_SERIES_TEXT_MAX];
        CHECK_STR(track_series_format(TRACK_SERIES_ELEVATION, 312.7f, text, sizeof(text)), "312");
        CHECK_STR(track_series_format(TRACK_SERIES_PACE, 312.0f, text, sizeof(text)), "5:12");
        CHECK_STR(track_series_format(TRACK_SERIES_PACE, 605.0f, text, sizeof(text)), "10:05");
        CHECK_STR(track_series_format(TRACK_SERIES_HEART_RATE, 148.0f, text, sizeof(text)), "148");
        CHECK_STR(track_series_format(TRACK_SERIES_HEART_RATE, NAN, text, sizeof(text)), "--");
        CHECK_STR(track_series_unit(TRACK_SERIES_PACE), "min/km");
        CHECK_STR(track_series_label(TRACK_SERIES_HEART_RATE), "Heart rate");
    }
}
