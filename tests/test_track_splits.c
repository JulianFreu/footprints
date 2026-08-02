#include "../src/track_splits.c"

#include "harness.h"

#include <stdlib.h>

// The sliding window is where the corners are: a record that sits inside a
// longer run, a five-kilometre mark that falls between two GPS fixes, a watch
// that lost its signal in the middle. All of it is exercised here against
// synthetic tracks, where the right answer is known exactly, rather than by
// looking at the panel and deciding the times look about right.

// A track built from (metres, seconds) pairs. partial_distance is what the
// window reads, so the coordinates are left at zero; the parser fills both, but
// nothing here goes near a coordinate.
typedef struct Sample {
    float metres;
    double seconds; // from the start of the track
} Sample;

static GpxTrack build(const Sample *samples, int count, time_t **times_out) {
    GpxTrack track = {0};
    track.points = calloc((size_t)count, sizeof(GpxPoint));
    track.total_points = count;
    track.points_capacity = count;

    time_t *times = calloc((size_t)count, sizeof(time_t));
    const time_t epoch = 1700000000; // an arbitrary recent second

    for (int i = 0; i < count; i++) {
        track.points[i].partial_distance = samples[i].metres;
        times[i] = epoch + (time_t)samples[i].seconds;
    }

    *times_out = times;
    return track;
}

static void destroy(GpxTrack *track, time_t *times) {
    free(track->points);
    track->points = NULL;
    free(times);
}

// A track run at a constant pace, sampled every `step` metres.
static GpxTrack constant_pace(float total_metres, float step,
                              double secs_per_metre, int *count_out,
                              time_t **times_out) {
    int count = (int)(total_metres / step) + 1;
    Sample *samples = calloc((size_t)count, sizeof(Sample));
    for (int i = 0; i < count; i++) {
        samples[i].metres = (float)i * step;
        samples[i].seconds = (double)samples[i].metres * secs_per_metre;
    }
    GpxTrack track = build(samples, count, times_out);
    free(samples);
    if (count_out)
        *count_out = count;
    return track;
}

void run_track_splits_tests(void) {
    time_t *times = NULL;
    GpxTrack track;

    SUITE("splits: every distance is a real one");
    // A missing row would be a designated-initialiser hole: a distance of zero,
    // which every track covers instantly and which would hold every record.
    for (int s = 0; s < SPLIT_COUNT; s++)
        CHECK(track_splits_metres((SplitDistance)s) > 0.0f);
    CHECK_NEAR(track_splits_metres(SPLIT_5K), 5000.0, 1e-6);
    CHECK_NEAR(track_splits_metres(SPLIT_10K), 10000.0, 1e-6);
    CHECK_NEAR(track_splits_metres(SPLIT_HALF_MARATHON), 21097.5, 1e-6);
    CHECK_NEAR(track_splits_metres(SPLIT_MARATHON), 42195.0, 1e-6);
    // Out of range asks for nothing rather than reading off the table.
    CHECK_NEAR(track_splits_metres(SPLIT_COUNT), 0.0, 1e-9);
    CHECK_NEAR(track_splits_metres((SplitDistance)-1), 0.0, 1e-9);

    SUITE("splits: a constant pace over ten kilometres");
    // 5:00/km, sampled once a second. The five and ten kilometre marks land on
    // a sample, so there is nothing to interpolate and the answer is exact.
    track = constant_pace(10000.0f, 5.0f, 0.3, NULL, &times);
    track_splits_compute(&track, times);
    CHECK_NEAR(track.splits[SPLIT_5K], 1500.0, 0.01);
    CHECK_NEAR(track.splits[SPLIT_10K], 3000.0, 0.01);
    // Not far enough for either of the long ones, which stay unset.
    CHECK_NEAR(track.splits[SPLIT_HALF_MARATHON], 0.0, 1e-9);
    CHECK_NEAR(track.splits[SPLIT_MARATHON], 0.0, 1e-9);
    destroy(&track, times);

    SUITE("splits: the record inside a longer run");
    // The whole point of the exercise. A half marathon at 6:00/km with the
    // stretch from 5 km to 15 km run at 4:00/km: the fastest ten kilometres is
    // that stretch, not the first ten and not the last.
    {
        // A shade over the half marathon, so that distance has exactly one
        // window and the ten kilometres inside it have many.
        int count = 21200 / 10 + 1;
        Sample *samples = calloc((size_t)count, sizeof(Sample));
        double seconds = 0.0;
        for (int i = 0; i < count; i++) {
            samples[i].metres = (float)(i * 10);
            samples[i].seconds = seconds;
            // The pace of the segment leaving this sample.
            bool fast = samples[i].metres >= 5000.0f && samples[i].metres < 15000.0f;
            seconds += 10.0 * (fast ? 0.24 : 0.36); // 4:00/km and 6:00/km
        }
        track = build(samples, count, &times);
        free(samples);
        track_splits_compute(&track, times);

        // Ten kilometres at 4:00/km is 2400 s. The first ten would be 5 km at
        // 6:00 plus 5 km at 4:00 -- 3000 s -- so a search that only ever
        // started at the beginning would be 600 s slow.
        CHECK_NEAR(track.splits[SPLIT_10K], 2400.0, 1.0);
        CHECK(track.splits[SPLIT_10K] < 3000.0f);
        CHECK_NEAR(track.splits[SPLIT_5K], 1200.0, 1.0);
        // The half marathon has only one window, so it is the whole track.
        CHECK(track.splits[SPLIT_HALF_MARATHON] > 0.0f);
        CHECK_NEAR(track.splits[SPLIT_MARATHON], 0.0, 1e-9);
        destroy(&track, times);
    }

    SUITE("splits: the mark falls between two fixes");
    // Fixes 60 m and 15 s apart at a constant 4:10/km, so the 5 km mark sits
    // 20 m into a segment and the true time is 1250 s. Snapping to the sample
    // before it would report 1265 s: fifteen seconds of a five-kilometre
    // record, which is the whole reason the ends are interpolated.
    track = constant_pace(6000.0f, 60.0f, 0.25, NULL, &times);
    track_splits_compute(&track, times);
    CHECK_NEAR(track.splits[SPLIT_5K], 1250.0, 0.05);
    CHECK(track.splits[SPLIT_5K] < 1264.0f);
    destroy(&track, times);

    SUITE("splits: the optimum window starts on a fix and ends between two");
    // The case that needs the mirrored pass. Elapsed time as a function of
    // where the window sits is piecewise linear, with a corner wherever either
    // end crosses a fix; the pass that only ever ends a window on a fix sees
    // half of those corners, and the minimum here is at one of the others.
    //
    // The runner speeds up from 6:40/km to 3:20/km at exactly the 1000 m fix,
    // and the far end of the window is in a 5:00/km stretch sampled so sparsely
    // that no fix falls near 6000 m. So the best window starts at 1000 m and
    // ends mid-segment at 6000 m, in 1050 s. Every window that ends on a fix is
    // 1100 s or worse, which is what this reports if pass B is dropped.
    {
        const Sample samples[] = {
            {0.0f, 0.0},
            {500.0f, 200.0},  // 6:40/km
            {1000.0f, 400.0}, // the pace changes here
            {1500.0f, 500.0},
            {2000.0f, 600.0},
            {2500.0f, 700.0},
            {3000.0f, 800.0},
            {3500.0f, 900.0},
            {4000.0f, 1000.0},
            {4500.0f, 1100.0},
            {5000.0f, 1200.0},
            {5500.0f, 1300.0}, // 3:20/km
            {6500.0f, 1600.0}, // one long 5:00/km segment
            {7000.0f, 1750.0},
            {7500.0f, 1900.0},
            {8000.0f, 2050.0},
        };
        track = build(samples, (int)(sizeof(samples) / sizeof(samples[0])), &times);
        track_splits_compute(&track, times);
        CHECK_NEAR(track.splits[SPLIT_5K], 1050.0, 0.01);
        CHECK(track.splits[SPLIT_5K] < 1099.0f);
        destroy(&track, times);
    }

    SUITE("splits: a track too short for any of them");
    track = constant_pace(3000.0f, 10.0f, 0.3, NULL, &times);
    track_splits_compute(&track, times);
    for (int s = 0; s < SPLIT_COUNT; s++)
        CHECK_NEAR(track.splits[s], 0.0, 1e-9);
    destroy(&track, times);

    SUITE("splits: a track of exactly the target distance");
    // The one window there is, which is the whole track. Off by one either way
    // and this is either missed entirely or read past the end of the array.
    track = constant_pace(5000.0f, 10.0f, 0.3, NULL, &times);
    track_splits_compute(&track, times);
    CHECK_NEAR(track.splits[SPLIT_5K], 1500.0, 0.01);
    CHECK_NEAR(track.splits[SPLIT_10K], 0.0, 1e-9);
    destroy(&track, times);

    SUITE("splits: a gap in the timestamps splits the track");
    // Twelve kilometres with the watch dropping out at the halfway mark. There
    // is a clean five kilometres either side of the gap, but no ten kilometres
    // that does not span it -- and a window that spanned it would be timed
    // across a hole.
    {
        int count = 12000 / 10 + 1;
        Sample *samples = calloc((size_t)count, sizeof(Sample));
        for (int i = 0; i < count; i++) {
            samples[i].metres = (float)(i * 10);
            samples[i].seconds = samples[i].metres * 0.3;
        }
        track = build(samples, count, &times);
        free(samples);
        // A single missing fix at 6 km is enough to end the run there.
        times[600] = (time_t)-1;
        track_splits_compute(&track, times);

        CHECK_NEAR(track.splits[SPLIT_5K], 1500.0, 0.01);
        CHECK_NEAR(track.splits[SPLIT_10K], 0.0, 1e-9);
        destroy(&track, times);
    }

    SUITE("splits: a clock that goes backwards");
    // Neither a negative time nor an absurd one: the run ends at the bad fix
    // and the stretch after it is measured on its own.
    {
        int count = 12000 / 10 + 1;
        Sample *samples = calloc((size_t)count, sizeof(Sample));
        for (int i = 0; i < count; i++) {
            samples[i].metres = (float)(i * 10);
            samples[i].seconds = samples[i].metres * 0.3;
        }
        track = build(samples, count, &times);
        free(samples);
        times[600] -= 2000; // two thousand seconds into the past
        track_splits_compute(&track, times);

        for (int s = 0; s < SPLIT_COUNT; s++)
            CHECK(track.splits[s] >= 0.0f);
        CHECK(isfinite(track.splits[SPLIT_5K]));
        // Five kilometres of clean data survive on the far side of the fault.
        CHECK_NEAR(track.splits[SPLIT_5K], 1500.0, 0.01);
        destroy(&track, times);
    }

    SUITE("splits: a stretch nobody could run is not a record");
    // A file labelled "Running" whose middle is a drive home, which is what
    // half the libraries exported from a phone app look like. A search for a
    // minimum lands on exactly that stretch, so it has to end the run rather
    // than hold the record: sixteen kilometres at 5:00/km with 4 km to 9 km
    // covered at 15 m/s.
    {
        int count = 16000 / 10 + 1;
        Sample *samples = calloc((size_t)count, sizeof(Sample));
        double seconds = 0.0;
        for (int i = 0; i < count; i++) {
            samples[i].metres = (float)(i * 10);
            samples[i].seconds = seconds;
            bool driving = samples[i].metres >= 4000.0f && samples[i].metres < 9000.0f;
            seconds += driving ? 10.0 / 15.0 : 3.0; // 15 m/s, then 3.33 m/s
        }
        track = build(samples, count, &times);
        free(samples);
        track_splits_compute(&track, times);

        // The clean seven kilometres after the drive hold the record: 5 km at
        // 3.33 m/s is 1500 s. The drive itself would have been about 333 s.
        CHECK_NEAR(track.splits[SPLIT_5K], 1500.0, 1.0);
        // And no window may span the drive, so the ten kilometres that the
        // whole track covers is not a record at all -- neither side of the
        // break is long enough on its own.
        CHECK_NEAR(track.splits[SPLIT_10K], 0.0, 1e-9);
        destroy(&track, times);
    }

    SUITE("splits: a coarse clock standing still is still believed");
    // Two fixes sharing a second is how a one-second clock records a pause, and
    // must not be mistaken for infinite speed -- but any distance covered in no
    // time at all is not something to believe.
    {
        const Sample paused[] = {
            {0.0f, 0.0},
            {100.0f, 30.0},
            {100.0f, 30.0}, // a pause, no distance
            {200.0f, 60.0},
            {5200.0f, 1560.0},
        };
        track = build(paused, (int)(sizeof(paused) / sizeof(paused[0])), &times);
        track_splits_compute(&track, times);
        // 5 km covered at 3.33 m/s throughout, so the pause changed nothing.
        CHECK(track.splits[SPLIT_5K] > 0.0f);
        CHECK(isfinite(track.splits[SPLIT_5K]));
        destroy(&track, times);
    }

    SUITE("splits: standing still does not divide by zero");
    // A minute of fixes at the same spot on either side of the five kilometre
    // mark, which is what a paused watch or a wait at a crossing looks like.
    {
        const int count = 700;
        Sample *samples = calloc((size_t)count, sizeof(Sample));
        double metres = 0.0;
        double seconds = 0.0;
        for (int i = 0; i < count; i++) {
            samples[i].metres = (float)metres;
            samples[i].seconds = seconds;
            // Two waits of half a minute, where the clock runs on and the
            // odometer does not. Distance has to resume from where it stopped:
            // a pause that ends by teleporting forward is a different fault.
            bool waiting = (i >= 250 && i < 260) || (i >= 499 && i < 509);
            metres += waiting ? 0.0 : 10.0;
            seconds += 3.0;
        }

        track = build(samples, count, &times);
        free(samples);
        track_splits_compute(&track, times);
        CHECK(isfinite(track.splits[SPLIT_5K]));
        // Five hundred moving steps of three seconds, plus whichever wait the
        // window cannot avoid.
        CHECK(track.splits[SPLIT_5K] >= 1500.0f);
        CHECK(track.splits[SPLIT_5K] < 1600.0f);
        destroy(&track, times);
    }

    SUITE("splits: degenerate input");
    // Nothing here may read off the end, which is what the sanitizers are for.
    track_splits_compute(NULL, NULL);

    {
        GpxTrack empty = {0};
        empty.splits[SPLIT_5K] = 99.0f; // must be cleared, not left stale
        track_splits_compute(&empty, NULL);
        CHECK_NEAR(empty.splits[SPLIT_5K], 0.0, 1e-9);
    }

    {
        const Sample one[] = {{0.0f, 0.0}};
        track = build(one, 1, &times);
        track_splits_compute(&track, times);
        for (int s = 0; s < SPLIT_COUNT; s++)
            CHECK_NEAR(track.splits[s], 0.0, 1e-9);
        destroy(&track, times);
    }

    {
        // Every fix missing its time: a planned route rather than a recording.
        int count = 12000 / 10 + 1;
        track = constant_pace(12000.0f, 10.0f, 0.3, &count, &times);
        for (int i = 0; i < track.total_points; i++)
            times[i] = (time_t)-1;
        track_splits_compute(&track, times);
        for (int s = 0; s < SPLIT_COUNT; s++)
            CHECK_NEAR(track.splits[s], 0.0, 1e-9);
        destroy(&track, times);
    }

    SUITE("splits: a longer distance is never faster than a shorter one");
    // The property an off-by-one in either pointer advance breaks, and which no
    // single hand-picked case would catch. A marathon with the pace wandering
    // about, so the window has real work to do wherever it sits.
    {
        int count = 43000 / 10 + 1;
        Sample *samples = calloc((size_t)count, sizeof(Sample));
        unsigned seed = 12345;
        double seconds = 0.0;
        for (int i = 0; i < count; i++) {
            samples[i].metres = (float)(i * 10);
            samples[i].seconds = seconds;
            seed = seed * 1103515245u + 12345u;
            // Between 3:20/km and 6:40/km, changing every ten metres.
            double secs_per_metre = 0.2 + 0.2 * (double)((seed >> 16) & 0xFF) / 255.0;
            seconds += 10.0 * secs_per_metre;
        }
        track = build(samples, count, &times);
        free(samples);
        track_splits_compute(&track, times);

        for (int s = 0; s < SPLIT_COUNT; s++)
            CHECK(track.splits[s] > 0.0f);
        CHECK(track.splits[SPLIT_5K] <= track.splits[SPLIT_10K]);
        CHECK(track.splits[SPLIT_10K] <= track.splits[SPLIT_HALF_MARATHON]);
        CHECK(track.splits[SPLIT_HALF_MARATHON] <= track.splits[SPLIT_MARATHON]);
        // And none of them can beat the pace the whole track was run at.
        CHECK(track.splits[SPLIT_MARATHON] <= (float)times[count - 1] -
                                                  (float)times[0] + 1.0f);
        destroy(&track, times);
    }
}
