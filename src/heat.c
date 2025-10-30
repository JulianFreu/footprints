#include "heat.h"

// Globale Variable für Sortierachse
int current_axis;

// Vergleichsfunktion für qsort
int compare_points(const void *a, const void *b)
{
    GpxPoint *p1 = *(GpxPoint **)a;
    GpxPoint *p2 = *(GpxPoint **)b;
    double diff = (current_axis == 0) ? (p1->world_x - p2->world_x) : (p1->world_y - p2->world_y);
    return (diff > 0) - (diff < 0);
}

// Erstelle einen neuen Knoten
KDNode *create_node(GpxPoint *point, int axis)
{
    KDNode *node = (KDNode *)malloc(sizeof(KDNode));
    if (!node)
    {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }
    node->point = point;
    node->axis = axis;
    node->left = node->right = NULL;
    return node;
}

// Baue den k-d-Tree rekursiv
KDNode *build_kdtree(GpxPoint **points, int n, int depth)
{
    if (n <= 0)
        return NULL;
    int axis = depth % 2;
    current_axis = axis;
    qsort(points, n, sizeof(GpxPoint *), compare_points);
    int median = n / 2;
    KDNode *node = create_node(points[median], axis);
    node->left = build_kdtree(points, median, depth + 1);
    node->right = build_kdtree(points + median + 1, n - median - 1, depth + 1);
    return node;
}

void free_kdtree(KDNode *node)
{
    if (node == NULL)
        return;

    free_kdtree(node->left);
    free_kdtree(node->right);
    free(node); // Nur der KDNode selbst, nicht node->point!
}

double squared_distance(GpxPoint p1, GpxPoint p2, float mercator_x_correction)
{
    // einfache flache projektion
    long int dx = (p2.world_x - p1.world_x) * mercator_x_correction;
    long int dy = (p2.world_y - p1.world_y);
    return dx * dx + dy * dy;
}

float get_x_correction_factor(int world_y)
{
    // Define band thresholds (in world_y) — precomputed
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

// Radius-Suche
void radius_search(KDNode *node, GpxPoint *target, double radius2, int *count, int *checked_ids, int total_tracks, float x_correction)
{
    if (!node)
        return;
    if (node->point->track_id != target->track_id && squared_distance(*node->point, *target, x_correction) <= radius2)
    {
        bool idAlreadyChecked = false;
        for (int i = 0; i < *count; i++)
        {
            if (node->point->track_id == checked_ids[i])
            {
                idAlreadyChecked = true;
                break;
            }
        }
        if (!idAlreadyChecked && *count < total_tracks)
        {
            checked_ids[*count] = node->point->track_id;
            (*count)++;
        }
    }
    int axis = node->axis;
    float diff = (axis == 0) ? target->world_x - node->point->world_x : target->world_y - node->point->world_y;
    if (diff <= 0)
    {
        radius_search(node->left, target, radius2, count, checked_ids, total_tracks, x_correction);
        if (diff * diff <= radius2)
            radius_search(node->right, target, radius2, count, checked_ids, total_tracks, x_correction);
    }
    else
    {
        radius_search(node->right, target, radius2, count, checked_ids, total_tracks, x_correction);
        if (diff * diff <= radius2)
            radius_search(node->left, target, radius2, count, checked_ids, total_tracks, x_correction);
    }
}

void print_progress_bar(int current, int total, int bar_width, struct timespec *start_time)
{
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    double elapsed = (current_time.tv_sec - start_time->tv_sec) +
                     (current_time.tv_nsec - start_time->tv_nsec) / 1e9;
    elapsed++;
    int points_per_second = current / (int)elapsed;
    if (points_per_second == 0)
        points_per_second = 1;
    int points_left = total - current;

    float progress = (float)current / total;
    int pos = (int)(bar_width * progress);
    printf("\r[");
    for (int i = 0; i < bar_width; ++i)
    {
        if (i < pos)
            printf("=");
        else if (i == pos)
            printf(">");
        else
            printf(" ");
    }
    printf("] %3d%% | pps: %4d | eta: %3dmin %2ds   ", (int)(progress * 100), points_per_second, (int)(points_left / points_per_second) / 60, (points_left / points_per_second) % 60);
    fflush(stdout);
}

void *heatmap_worker(void *arg)
{
    HeatmapTask *task = (HeatmapTask *)arg;

    int progress_update_increments = 100;

    int *checked_ids = (int *)malloc(task->total_tracks * sizeof(int));
    if (!checked_ids)
    {
        fprintf(stderr, "Thread malloc failed\n");
        return NULL;
    }

    for (int i = task->start; i < task->end; i++)
    {
        // precalculate some values of taget point to be more efficient:
        // double lat_rad = task->points[i]->lat * DEG_TO_RAD;
        // double cos_lat = cos(lat_rad);
        // double meters_per_deg_lon = cos_lat * 111320.0;

        float x_correction = get_x_correction_factor(task->points[i]->world_y);
        // search for points in range
        int count = 0;
        memset(checked_ids, -1, task->total_tracks * sizeof(int));
        radius_search(task->tree, task->points[i], task->radius2, &count, checked_ids, task->total_tracks, x_correction);
        task->points[i]->heat = count;

        pthread_mutex_lock(task->max_mutex);
        if (count > *(task->thread_max_heat))
        {
            *(task->thread_max_heat) = count;
        }
        pthread_mutex_unlock(task->max_mutex);

        task->thread_progress++;
        if (task->thread_progress >= progress_update_increments)
        {
            pthread_mutex_lock(task->progress_mutex);
            *(task->total_progress) += task->thread_progress;
            task->thread_progress = 0;
            pthread_mutex_unlock(task->progress_mutex);
        }
    }
    free(checked_ids);
    pthread_mutex_lock(task->progress_mutex);
    *(task->total_progress) += task->thread_progress;
    task->thread_progress = 0;
    pthread_mutex_unlock(task->progress_mutex);
    return NULL;
}

bool CalculateHeatmap(GpxCollection *collection)
{
    // convert gpx track collection a single big point collection
    int total_points = 0;
    for (int i = 0; i < collection->total_tracks; i++)
    {
        if (collection->tracks[i].visible_in_list == true)
        {
            total_points = total_points + collection->tracks[i].total_points;
            printf("%d points in track %d\n", collection->tracks[i].total_points, i);
        }
    }
    printf("There are %d data points in total\n", total_points);
    printf("Collecting all points in one array\n");
    GpxPoint **points = (GpxPoint **)malloc(total_points * sizeof(GpxPoint *));
    if (!points)
    {
        perror("malloc failed");
        return false;
    }
    int i = 0;
    for (int track_id = 0; track_id < collection->total_tracks; track_id++)
    {
        if (collection->tracks[track_id].visible_in_list == true)
        {
            for (int point_id = 0; point_id < collection->tracks[track_id].total_points; point_id++)
            {
                points[i] = &collection->tracks[track_id].points[point_id];
                i++;
            }
        }
    }

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time); // Startzeit messen
    printf("Building kdtree\n");
    float radius = 200.0f;
    float radius2 = radius * radius;
    KDNode *tree = build_kdtree(points, total_points, 0);

    printf("Calculating heat in %d threads\n", NUM_THREADS);

    pthread_t threads[NUM_THREADS];
    HeatmapTask tasks[NUM_THREADS];
    pthread_mutex_t max_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;
    int max_heat = 0;

    int total_progress = 0;
    int chunk_size = total_points / NUM_THREADS;

    for (int t = 0; t < NUM_THREADS; t++)
    {
        tasks[t].points = points;
        tasks[t].start = t * chunk_size;
        tasks[t].end = (t == NUM_THREADS - 1) ? total_points : (t + 1) * chunk_size;
        tasks[t].tree = tree;
        tasks[t].radius2 = radius2;
        tasks[t].total_tracks = collection->total_tracks;
        tasks[t].thread_max_heat = &max_heat;
        tasks[t].max_mutex = &max_mutex;
        tasks[t].progress_mutex = &progress_mutex;
        tasks[t].thread_progress = 0;
        tasks[t].total_progress = &total_progress;

        if (pthread_create(&threads[t], NULL, heatmap_worker, &tasks[t]) != 0)
        {
            perror("pthread_create failed");
            free_kdtree(tree);
            free(points);
            return false;
        }
    }
    while (true)
    {
        pthread_mutex_lock(&progress_mutex);
        print_progress_bar(total_progress, total_points, 30, &start_time);
        if (total_progress >= total_points)
        {
            pthread_mutex_unlock(&progress_mutex);
            break;
        }
        pthread_mutex_unlock(&progress_mutex);
        usleep(100000);
    }

    for (int t = 0; t < NUM_THREADS; t++)
    {
        pthread_join(threads[t], NULL);
    }

    collection->max_heat = max_heat;

    free_kdtree(tree);
    free(points);
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    printf("\nHeatmap calculation took %.3f seconds\n", elapsed);
    return true;
}