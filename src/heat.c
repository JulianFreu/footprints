#include "heat.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "heat_types.h"
#include "progress.h"
#include "settings.h"

#include "log.h"

// The tree is implicit in the array: the node for a range is its midpoint, its
// children are the halves either side. Build and search both derive the split
// the same way, so nothing has to be stored.
static int subtree_median(int lo, int hi) {
    return lo + (hi - lo) / 2;
}

static int axis_value(const GpxPoint *point, int axis) {
    return (axis == 0) ? point->world_x : point->world_y;
}

static void swap_points(GpxPoint **a, GpxPoint **b) {
    GpxPoint *tmp = *a;
    *a = *b;
    *b = tmp;
}

// Reorders points[lo, hi) so that index n holds the value it would hold if the
// range were sorted on `axis`, with everything below it no greater and
// everything above it no smaller. That is the whole of what the build needs --
// fully sorting each level, as this did, costs a log factor for an ordering
// that is thrown away immediately.
static void select_nth(GpxPoint **points, int lo, int hi, int n, int axis) {
    while (hi - lo > 1) {
        // Median of three. Track points arrive in the order they were
        // recorded, so a range is very often already close to sorted on one
        // axis, which is precisely where a first-element pivot degrades to
        // quadratic.
        int mid = subtree_median(lo, hi);
        int a = axis_value(points[lo], axis);
        int b = axis_value(points[mid], axis);
        int c = axis_value(points[hi - 1], axis);
        int pivot;
        if (a < b)
            pivot = (b < c) ? b : ((a < c) ? c : a);
        else
            pivot = (a < c) ? a : ((b < c) ? c : b);

        // Three-way partition, because a stationary GPS emits long runs of
        // identical coordinates and a two-way split would put all of them on
        // one side and make no progress.
        int lt = lo, i = lo, gt = hi;
        while (i < gt) {
            int value = axis_value(points[i], axis);
            if (value < pivot)
                swap_points(&points[lt++], &points[i++]);
            else if (value > pivot)
                swap_points(&points[i], &points[--gt]);
            else
                i++;
        }

        // [lo, lt) < pivot, [lt, gt) == pivot, [gt, hi) > pivot
        if (n < lt)
            hi = lt;
        else if (n < gt)
            return; // n landed inside the run of pivots; it is in place
        else
            lo = gt;
    }
}

// Permutes points[lo, hi) into the implicit k-d tree layout.
static void build_kdtree(GpxPoint **points, int lo, int hi, int depth) {
    if (hi - lo <= 1)
        return;
    int mid = subtree_median(lo, hi);
    select_nth(points, lo, hi, mid, depth % 2);
    build_kdtree(points, lo, mid, depth + 1);
    build_kdtree(points, mid + 1, hi, depth + 1);
}

static double squared_distance(GpxPoint p1, GpxPoint p2, float mercator_x_correction) {
    // Flat projection; good enough at the radii we search over.
    long int dx = (p2.world_x - p1.world_x) * mercator_x_correction;
    long int dy = (p2.world_y - p1.world_y);
    return dx * dx + dy * dy;
}

// Mercator stretches the x axis by 1/cos(latitude), so a world pixel east-west
// covers less ground the further from the equator it is. Scaling dx by
// cos(latitude) is what makes the search radius mean the same distance
// everywhere. The bands step through that cosine in eight pieces of world_y,
// which is close enough over a radius of a couple of hundred pixels and keeps
// the inner loop off a trig call. The equator is the middle of the world
// square, hence the symmetry either side of it.
static float get_x_correction_factor(int world_y) {
    if (world_y < 33000000)
        return 0.09;
    else if (world_y < 67000000)
        return 0.41;
    else if (world_y < 100000000)
        return 0.71;
    else if (world_y < 134000000)
        return 0.92;
    else if (world_y < 167000000)
        return 1.00;
    else if (world_y < 201000000)
        return 0.92;
    else if (world_y < 234000000)
        return 0.71;
    else if (world_y < 268000000)
        return 0.41;
    else
        return 0.09;
}

// Counts the distinct tracks with a point within radius2 of target.
//
// `seen` holds, per track id, the stamp of the target it was last counted for.
// Comparing against the current stamp makes the duplicate check a single array
// read: the previous version scanned the ids collected so far on every hit and
// cleared the whole array once per target, which on a large library was more
// memset traffic than actual searching.
static void radius_search(GpxPoint **points, int lo, int hi, int depth,
                          const GpxPoint *target, double radius2, int *count,
                          int *seen, int stamp, float x_correction) {
    if (hi - lo <= 0)
        return;

    int mid = subtree_median(lo, hi);
    const GpxPoint *node = points[mid];
    int axis = depth % 2;

    if (node->track_id != target->track_id &&
        squared_distance(*node, *target, x_correction) <= radius2) {
        if (seen[node->track_id] != stamp) {
            seen[node->track_id] = stamp;
            (*count)++;
        }
    }

    double diff = axis_value(target, axis) - axis_value(node, axis);

    // The near side always has to be walked; the far side only if the splitting
    // plane itself is within the radius. squared_distance scales the x axis by
    // x_correction, so the test for that axis has to scale the same way --
    // comparing the raw pixel gap against the radius pruned branches that the
    // distance function would have accepted, and undercounted the heat.
    double plane = (axis == 0) ? diff * x_correction : diff;
    bool plane_in_range = plane * plane <= radius2;

    int near_lo = lo, near_hi = mid, far_lo = mid + 1, far_hi = hi;
    if (diff > 0) {
        near_lo = mid + 1;
        near_hi = hi;
        far_lo = lo;
        far_hi = mid;
    }

    radius_search(points, near_lo, near_hi, depth + 1, target, radius2, count,
                  seen, stamp, x_correction);
    if (plane_in_range)
        radius_search(points, far_lo, far_hi, depth + 1, target, radius2, count,
                      seen, stamp, x_correction);
}

static void *heatmap_worker(void *arg) {
    HeatmapTask *task = (HeatmapTask *)arg;

    // One stamp slot per track, cleared once for the whole slice rather than
    // once per point. -1 is not a valid stamp, and each worker owns its own
    // array, so a stamp only has to be unique within this loop.
    int *seen = (int *)malloc((size_t)task->total_tracks * sizeof(int));
    if (!seen) {
        fprintf(stderr, "Thread malloc failed\n");
        return NULL;
    }
    memset(seen, -1, (size_t)task->total_tracks * sizeof(int));

    for (int i = task->start; i < task->end; i++) {
        // Checked per batch rather than per point: often enough that quitting
        // is responsive, rarely enough that it costs nothing.
        if ((i - task->start) % HEAT_PROGRESS_BATCH == 0 &&
            progress_cancelled(task->progress)) {
            break;
        }

        float x_correction = get_x_correction_factor(task->points[i]->world_y);
        int count = 0;
        radius_search(task->points, 0, task->total_points, 0, task->points[i],
                      task->radius2, &count, seen, i, x_correction);
        task->points[i]->heat = count;

        if (count > task->max_heat)
            task->max_heat = count;

        // Batched so the workers are not all hammering one cache line.
        task->batch_progress++;
        if (task->batch_progress >= HEAT_PROGRESS_BATCH) {
            progress_add(task->progress, task->batch_progress);
            task->batch_progress = 0;
        }
    }
    free(seen);
    progress_add(task->progress, task->batch_progress);
    task->batch_progress = 0;
    return NULL;
}

// Number of heat workers to run: one per online core, clamped to a sane range.
static int heat_worker_count(void) {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1)
        cores = 1;
    if (cores > 64)
        cores = 64;
    return (int)cores;
}

bool calculate_heatmap(GpxCollection *collection, const Progress *progress) {
    int total_points = 0;
    for (int i = 0; i < collection->total_tracks; i++) {
        if (collection->tracks[i].visible_in_list == true) {
            total_points = total_points + collection->tracks[i].total_points;
            LOG_DEBUG("%d points in track %d\n", collection->tracks[i].total_points, i);
        }
    }
    LOG_DEBUG("There are %d data points in total\n", total_points);
    progress_set_total(progress, total_points);
    progress_set_completed(progress, 0);
    LOG_DEBUG("Collecting all points in one array\n");
    GpxPoint **points = (GpxPoint **)malloc(total_points * sizeof(GpxPoint *));
    if (!points) {
        perror("malloc failed");
        return false;
    }
    int i = 0;
    for (int track_id = 0; track_id < collection->total_tracks; track_id++) {
        if (collection->tracks[track_id].visible_in_list == true) {
            for (int point_id = 0; point_id < collection->tracks[track_id].total_points; point_id++) {
                points[i] = &collection->tracks[track_id].points[point_id];
                i++;
            }
        }
    }

    if (total_points == 0) {
        free(points);
        collection->max_heat = 0;
        LOG_DEBUG("No visible points; nothing to calculate\n");
        return true;
    }

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    LOG_DEBUG("Building kdtree\n");
    // Read once here rather than in the workers: they all want the same number,
    // and a setting changed mid-calculation must not give one worker a
    // different radius from the next.
    float radius = settings.heat_radius_pixels;
    float radius2 = radius * radius;
    build_kdtree(points, 0, total_points, 0);

    // One worker per core, but never more workers than points -- a fixed count
    // left every thread but the last with an empty range on small datasets,
    // because total_points / NUM_THREADS truncated to 0.
    int thread_count = heat_worker_count();
    if (thread_count > total_points)
        thread_count = total_points;

    LOG_DEBUG("Calculating heat in %d threads\n", thread_count);

    pthread_t threads[thread_count];
    HeatmapTask tasks[thread_count];

    for (int t = 0; t < thread_count; t++) {
        tasks[t].points = points;
        // Spread the remainder over the first few workers instead of piling it
        // all onto the last one.
        tasks[t].start = (int)((int64_t)total_points * t / thread_count);
        tasks[t].end = (int)((int64_t)total_points * (t + 1) / thread_count);
        tasks[t].total_points = total_points;
        tasks[t].radius2 = radius2;
        tasks[t].total_tracks = collection->total_tracks;
        tasks[t].max_heat = 0;
        tasks[t].progress = progress;
        tasks[t].batch_progress = 0;

        if (pthread_create(&threads[t], NULL, heatmap_worker, &tasks[t]) != 0) {
            perror("pthread_create failed");
            free(points);
            return false;
        }
    }
    // The reduction the workers no longer do a point at a time. Reading a
    // task's maximum after its join is ordered by the join itself.
    int max_heat = 0;
    for (int t = 0; t < thread_count; t++) {
        pthread_join(threads[t], NULL);
        if (tasks[t].max_heat > max_heat)
            max_heat = tasks[t].max_heat;
    }

    collection->max_heat = max_heat;

    free(points);
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    LOG_DEBUG("\nHeatmap calculation took %.3f seconds\n",
              (end_time.tv_sec - start_time.tv_sec) +
                  (end_time.tv_nsec - start_time.tv_nsec) / 1e9);
    return true;
}