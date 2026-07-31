#ifndef HEAT_TYPES_H
#define HEAT_TYPES_H

#include <pthread.h>

#include "gpx_types.h"

// One worker's slice of the heat calculation.
//
// The k-d tree is implicit: `points` is permuted so the median of any range
// sits at that range's midpoint, which makes the array itself the tree and
// needs no nodes. Every worker searches the whole of it, over
// [0, total_points), while start/end bound only the points that worker is
// responsible for assigning heat to. The mutex-guarded fields are shared
// across all workers.
typedef struct
{
    GpxPoint **points;
    int total_points;
    int start;
    int end;
    float radius2;
    int total_tracks;
    int *thread_max_heat;
    pthread_mutex_t *max_mutex;
    pthread_mutex_t *progress_mutex;
    int thread_progress;
    int *total_progress;
} HeatmapTask;

#endif
