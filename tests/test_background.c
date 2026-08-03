#include "../src/background.c"

#include "harness.h"

#include <stdlib.h>
#include <time.h>

// The contract the frame loop depends on: while a job is in flight the
// collection belongs to the worker, exactly one collect reports the results,
// and a stop always joins. Getting any of these wrong is a use-after-free or a
// silently dropped result rather than a visible bug, so they are pinned here.

static void sleep_briefly(void) {
    struct timespec ts = {0, 5 * 1000 * 1000}; // 5ms
    nanosleep(&ts, NULL);
}

// A collection big enough that the heat calculation takes long enough to
// observe in flight, but small enough not to slow the suite down.
static void build_collection(GpxCollection *collection, GpxTrack *tracks,
                             GpxPoint *storage, int track_count, int per_track) {
    // A finished job leaves a point index behind it, and this is about to
    // forget the collection that owns it.
    point_index_free(&collection->point_index);

    *collection = (GpxCollection){0};
    collection->tracks = tracks;
    collection->total_tracks = track_count;

    for (int t = 0; t < track_count; t++) {
        tracks[t] = (GpxTrack){0};
        tracks[t].points = &storage[t * per_track];
        tracks[t].total_points = per_track;
        tracks[t].track_id = t;
        tracks[t].visible_in_list = true;
        for (int i = 0; i < per_track; i++) {
            tracks[t].points[i] = (GpxPoint){0};
            tracks[t].points[i].world_x = 142800000 + (i * 37) % 4000;
            tracks[t].points[i].world_y = 93100000 + (i * 53) % 4000;
            tracks[t].points[i].track_id = t;
        }
    }
    reset_filters(&collection->filters);
}

// Spins until the job reports itself finished, so the suite never hangs on a
// worker that failed to start.
static bool wait_for_finish(BackgroundJob *job, int max_spins) {
    for (int i = 0; i < max_spins; i++) {
        if (background_collect(job))
            return true;
        sleep_briefly();
    }
    return false;
}

void run_background_tests(void) {
    enum {
        TRACKS = 24,
        PER_TRACK = 500
    };
    static GpxTrack tracks[TRACKS];
    static GpxPoint storage[TRACKS * PER_TRACK];
    GpxCollection collection = {0};

    SUITE("background: an idle job is not busy and collects nothing");
    BackgroundJob job = {0};
    CHECK(!background_busy(&job));
    CHECK(!background_collect(&job));
    CHECK_INT(background_stage(&job), BG_STAGE_IDLE);
    // Stopping an idle job is allowed and does nothing.
    background_stop(&job);
    CHECK(!background_busy(&job));

    SUITE("background: a heat job runs, reports, and collects exactly once");
    build_collection(&collection, tracks, storage, TRACKS, PER_TRACK);
    CHECK(background_start_heat(&job, &collection));
    CHECK(background_busy(&job));

    CHECK(wait_for_finish(&job, 4000));
    // Collecting is what hands the collection back, and only the first one
    // reports results -- a second would run the adopt step twice.
    CHECK(!background_busy(&job));
    CHECK(!background_collect(&job));
    // The work actually happened.
    CHECK(collection.max_heat > 0);
    CHECK_INT(background_stage(&job), BG_STAGE_IDLE);
    // The fraction is complete once the job is.
    CHECK_NEAR(background_fraction(&job), 1.0, 0.001);

    SUITE("background: a second job is refused while one is running");
    build_collection(&collection, tracks, storage, TRACKS, PER_TRACK);
    CHECK(background_start_heat(&job, &collection));
    // This is what stops a second click on Calculate Heat stacking a job.
    CHECK(!background_start_heat(&job, &collection));
    CHECK(!background_start_load(&job, &collection));
    CHECK(wait_for_finish(&job, 4000));

    SUITE("background: stop cancels and joins");
    build_collection(&collection, tracks, storage, TRACKS, PER_TRACK);
    CHECK(background_start_heat(&job, &collection));
    // Returns only once the worker is gone, which is what makes it safe for
    // teardown to free the collection afterwards.
    background_stop(&job);
    CHECK(!background_busy(&job));
    CHECK_INT(background_stage(&job), BG_STAGE_IDLE);
    // A stopped job has nothing left to collect.
    CHECK(!background_collect(&job));
    // And the next one still starts cleanly.
    CHECK(background_start_heat(&job, &collection));
    CHECK(wait_for_finish(&job, 4000));

    SUITE("background: progress stays in range through a job");
    build_collection(&collection, tracks, storage, TRACKS, PER_TRACK);
    CHECK(background_start_heat(&job, &collection));
    int out_of_range = 0;
    for (int i = 0; i < 400 && background_busy(&job); i++) {
        float fraction = background_fraction(&job);
        if (fraction < 0.0f || fraction > 1.0f)
            out_of_range++;
        if (background_collect(&job))
            break;
        sleep_briefly();
    }
    CHECK_INT(out_of_range, 0);
    if (background_busy(&job))
        CHECK(wait_for_finish(&job, 4000));

    SUITE("background: stage labels are never null or empty");
    CHECK(background_stage_label(BG_STAGE_IDLE)[0] != '\0');
    CHECK(background_stage_label(BG_STAGE_PARSING)[0] != '\0');
    CHECK(background_stage_label(BG_STAGE_HEAT)[0] != '\0');

    point_index_free(&collection.point_index);
}
