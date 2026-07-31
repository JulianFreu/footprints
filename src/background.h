#ifndef BACKGROUND_H
#define BACKGROUND_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "gpx_types.h"
#include "progress.h"

// Runs the two operations that take long enough to freeze the window: reading
// the GPX library at startup, and recalculating the heat.
//
// Both used to run on the main thread -- the heat one straight out of a Clay
// click handler -- so the application stopped answering the window manager for
// as long as they took, and reported progress to stdout where nobody watching
// the window could see it.
//
// Ownership rule: while a job is in flight the worker has the collection to
// itself. The main thread must not read or draw it until background_collect
// says the results have landed. background_busy is what the frame loop asks.

typedef enum {
    BG_STAGE_IDLE,
    BG_STAGE_PARSING,
    BG_STAGE_HEAT,
} BgStage;

typedef struct BackgroundJob {
    pthread_t thread;
    bool thread_started; // main thread only

    _Atomic int stage;     // BgStage
    _Atomic int completed; // units done in the current stage
    _Atomic int total;     // units in the current stage, 0 while unknown
    _Atomic bool cancel;
    _Atomic bool finished; // worker is done; main thread has yet to join

    // Owned by the worker for the duration of the job.
    GpxCollection *collection;
    bool parse_first;
} BackgroundJob;

// Reads the library and then calculates the heat over it.
bool background_start_load(BackgroundJob *job, GpxCollection *collection);
// Recalculates the heat over the tracks already parsed.
bool background_start_heat(BackgroundJob *job, GpxCollection *collection);

// True while the collection belongs to the worker.
bool background_busy(const BackgroundJob *job);

// What to show while it is busy: the stage, and 0..1 through it. A total of
// zero means the stage has not worked out how much there is to do yet.
BgStage background_stage(const BackgroundJob *job);
float background_fraction(const BackgroundJob *job);
const char *background_stage_label(BgStage stage);

// Main thread: if the worker has finished, joins it and hands the collection
// back. Returns true exactly once per job, on the frame the results land.
bool background_collect(BackgroundJob *job);

// Asks any running job to stop and waits for it. Safe to call when idle.
void background_stop(BackgroundJob *job);

#endif
