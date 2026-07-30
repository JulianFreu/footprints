#ifndef HEAT_TYPES_H
#define HEAT_TYPES_H

#include <pthread.h>

#include "gpx_types.h"

// 2-d k-d tree over every visible track point, used for the radius search that
// assigns each point its heat value.
typedef struct KDNode {
    GpxPoint *point;
    int axis;
    struct KDNode *left, *right;
} KDNode;

// One worker's slice of the heat calculation. start/end index into the shared
// points array; the mutex-guarded fields are shared across all workers.
typedef struct
{
    GpxPoint **points;
    int start;
    int end;
    KDNode *tree;
    float radius2;
    int total_tracks;
    int *thread_max_heat;
    pthread_mutex_t *max_mutex;
    pthread_mutex_t *progress_mutex;
    int thread_progress;
    int *total_progress;
} HeatmapTask;

#endif
