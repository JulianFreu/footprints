#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdatomic.h>
#include <stdbool.h>

// How a long operation reports back to whoever started it, and how it is asked
// to stop. Both fields are optional: passing NULL for the whole thing runs the
// operation silently and uninterruptibly, which is what the tests want.
//
// The counters are plain atomics rather than mutex-guarded, because the reader
// only ever draws them -- a value one frame out of date is not worth a lock.
typedef struct Progress {
    _Atomic int *completed;
    _Atomic int *total;
    // Set by the main thread to ask the operation to give up early, checked
    // often enough that quitting stays responsive.
    _Atomic bool *cancel;
} Progress;

static inline void progress_set_total(const Progress *progress, int total) {
    if (progress && progress->total)
        atomic_store(progress->total, total);
}

static inline void progress_set_completed(const Progress *progress, int completed) {
    if (progress && progress->completed)
        atomic_store(progress->completed, completed);
}

static inline void progress_add(const Progress *progress, int delta) {
    if (progress && progress->completed)
        atomic_fetch_add(progress->completed, delta);
}

static inline bool progress_cancelled(const Progress *progress) {
    return progress && progress->cancel && atomic_load(progress->cancel);
}

#endif
