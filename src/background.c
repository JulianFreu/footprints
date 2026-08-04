#include "background.h"

#include <stdio.h>

#include "filters.h"
#include "gpx_parser.h"
#include "heat.h"
#include "log.h"
#include "point_index.h"
#include "settings.h"

static Progress job_progress(BackgroundJob *job) {
    return (Progress){
        .completed = &job->completed,
        .total = &job->total,
        .cancel = &job->cancel};
}

static void begin_stage(BackgroundJob *job, BgStage stage) {
    // Zero the counters before announcing the stage, so a reader that sees the
    // new stage never sees the previous stage's totals alongside it.
    atomic_store(&job->completed, 0);
    atomic_store(&job->total, 0);
    atomic_store(&job->stage, stage);
}

static void *background_worker(void *arg) {
    BackgroundJob *job = (BackgroundJob *)arg;
    Progress progress = job_progress(job);

    if (job->parse_first) {
        begin_stage(job, BG_STAGE_PARSING);
        // The parse grows the tracks array, so anything pointing into it is
        // stale before the parse rather than after it.
        point_index_invalidate(&job->collection->point_index);
        if (!gpx_parse_all_files(job->collection, &progress))
            fprintf(stderr, "No tracks were loaded from %s\n", settings.gpx_dir);

        // Freshly parsed tracks have not been through the filters, and the
        // heat is only calculated over the visible ones -- so this has to
        // happen between the two stages, not after both.
        apply_filter_values(job->collection);
    }

    if (!atomic_load(&job->cancel)) {
        begin_stage(job, BG_STAGE_HEAT);
        if (!calculate_heatmap(job->collection, &progress))
            fprintf(stderr, "Heat calculation failed; the map will render unshaded\n");
    }

    // Built here rather than on the first tile that needs it: it is a sort of
    // every point in the library, and the main thread would otherwise spend a
    // frame on it. It is pure arithmetic over memory this thread still owns,
    // and it outlives every change to the filters, so this is the only place
    // that pays for it.
    if (!atomic_load(&job->cancel))
        point_index_ensure(job->collection);

    atomic_store(&job->stage, BG_STAGE_IDLE);
    // Published last, and it is what the main thread waits on: everything the
    // worker wrote happens before this store and is visible after the load.
    atomic_store(&job->finished, true);
    return NULL;
}

static bool start(BackgroundJob *job, GpxCollection *collection, bool parse_first) {
    if (job->thread_started)
        return false; // one job at a time; the caller's request is dropped

    job->collection = collection;
    job->parse_first = parse_first;
    atomic_store(&job->cancel, false);
    atomic_store(&job->finished, false);
    begin_stage(job, parse_first ? BG_STAGE_PARSING : BG_STAGE_HEAT);

    if (pthread_create(&job->thread, NULL, background_worker, job) != 0) {
        perror("pthread_create failed");
        atomic_store(&job->stage, BG_STAGE_IDLE);
        return false;
    }
    job->thread_started = true;
    return true;
}

bool background_start_load(BackgroundJob *job, GpxCollection *collection) {
    return start(job, collection, true);
}

bool background_start_heat(BackgroundJob *job, GpxCollection *collection) {
    return start(job, collection, false);
}

bool background_busy(const BackgroundJob *job) {
    return job->thread_started;
}

BgStage background_stage(const BackgroundJob *job) {
    return (BgStage)atomic_load(&job->stage);
}

float background_fraction(const BackgroundJob *job) {
    int total = atomic_load(&job->total);
    if (total <= 0)
        return 0.0f;

    int completed = atomic_load(&job->completed);
    if (completed >= total)
        return 1.0f;
    return (float)completed / (float)total;
}

const char *background_stage_label(BgStage stage) {
    switch (stage) {
    case BG_STAGE_PARSING:
        return "Reading tracks";
    case BG_STAGE_HEAT:
        return "Calculating heat";
    case BG_STAGE_IDLE:
        break;
    }
    return "Working";
}

bool background_collect(BackgroundJob *job) {
    if (!job->thread_started || !atomic_load(&job->finished))
        return false;

    pthread_join(job->thread, NULL);
    job->thread_started = false;
    atomic_store(&job->finished, false);
    return true;
}

void background_stop(BackgroundJob *job) {
    if (!job->thread_started)
        return;

    atomic_store(&job->cancel, true);
    pthread_join(job->thread, NULL);
    job->thread_started = false;
    atomic_store(&job->stage, BG_STAGE_IDLE);
}
