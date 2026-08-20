#include "../src/heat.c"

#include "harness.h"

#include <stdint.h>

// A tiny deterministic PRNG, so a failure is reproducible and the suite does
// not depend on the platform's rand().
static uint64_t rng_state = 12345;
static int next_random(int bound) {
    // Spelled in a fixed-width type: unsigned long is 64 bits on Linux and
    // 32 on Windows, where the multiply below would be truncated and the
    // shift would be undefined outright.
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((rng_state >> 33) % (uint64_t)bound);
}

// What the k-d tree is supposed to compute, written the obvious way: for each
// point, how many other tracks have any point within the radius.
static int brute_force_heat(GpxPoint **points, int total_points, int target,
                            float radius2, int total_tracks) {
    float correction = get_x_correction_factor(points[target]->world_y);
    bool *found = calloc((size_t)total_tracks, sizeof(bool));
    int count = 0;
    for (int i = 0; i < total_points; i++) {
        if (points[i]->track_id == points[target]->track_id)
            continue;
        if (found[points[i]->track_id])
            continue;
        if (squared_distance(*points[i], *points[target], correction) <= radius2) {
            found[points[i]->track_id] = true;
            count++;
        }
    }
    free(found);
    return count;
}

static int tree_heat(GpxPoint **points, int total_points, int target,
                     float radius2, int total_tracks) {
    float correction = get_x_correction_factor(points[target]->world_y);
    int *seen = malloc((size_t)total_tracks * sizeof(int));
    memset(seen, -1, (size_t)total_tracks * sizeof(int));
    int count = 0;
    radius_search(points, 0, total_points, 0, points[target], radius2, &count,
                  seen, target, correction);
    free(seen);
    return count;
}

// Builds the tree over a point set and checks every point against brute force.
static void check_against_brute_force(GpxPoint *storage, int total_points,
                                      int total_tracks, float radius2) {
    GpxPoint **points = malloc((size_t)total_points * sizeof(GpxPoint *));
    for (int i = 0; i < total_points; i++)
        points[i] = &storage[i];

    // Brute-force answers are taken before the build, because the build
    // permutes the array -- and are keyed by the point itself, not its index.
    int *expected = malloc((size_t)total_points * sizeof(int));
    for (int i = 0; i < total_points; i++)
        expected[i] = brute_force_heat(points, total_points, i, radius2, total_tracks);

    build_kdtree(points, 0, total_points, 0);

    int mismatches = 0;
    for (int i = 0; i < total_points; i++) {
        // points[] has been permuted, so map back through the storage index.
        int original = (int)(points[i] - storage);
        if (tree_heat(points, total_points, i, radius2, total_tracks) != expected[original])
            mismatches++;
    }
    CHECK_INT(mismatches, 0);

    free(points);
    free(expected);
}

void run_heat_tests(void) {
    SUITE("heat: select_nth places the median");
    // The property the implicit tree relies on: after the call, everything
    // below n is no greater and everything above is no smaller.
    for (int trial = 0; trial < 50; trial++) {
        enum {
            N = 101
        };
        GpxPoint storage[N];
        GpxPoint *points[N];
        for (int i = 0; i < N; i++) {
            storage[i] = (GpxPoint){0};
            storage[i].world_x = next_random(1000);
            storage[i].world_y = next_random(1000);
            points[i] = &storage[i];
        }
        int axis = trial % 2;
        int n = N / 2;
        select_nth(points, 0, N, n, axis);
        int pivot = axis_value(points[n], axis);
        int bad = 0;
        for (int i = 0; i < n; i++)
            if (axis_value(points[i], axis) > pivot)
                bad++;
        for (int i = n + 1; i < N; i++)
            if (axis_value(points[i], axis) < pivot)
                bad++;
        CHECK_INT(bad, 0);
    }

    SUITE("heat: select_nth survives a range of all-equal values");
    // A stationary GPS emits long runs of identical coordinates; a two-way
    // partition makes no progress on them.
    {
        enum {
            N = 64
        };
        GpxPoint storage[N];
        GpxPoint *points[N];
        for (int i = 0; i < N; i++) {
            storage[i] = (GpxPoint){0};
            storage[i].world_x = 500;
            storage[i].world_y = 500;
            points[i] = &storage[i];
        }
        select_nth(points, 0, N, N / 2, 0);
        CHECK_INT(axis_value(points[N / 2], 0), 500);
    }

    SUITE("heat: build permutes without losing or duplicating a point");
    {
        enum {
            N = 257
        };
        GpxPoint storage[N];
        GpxPoint *points[N];
        for (int i = 0; i < N; i++) {
            storage[i] = (GpxPoint){0};
            storage[i].world_x = next_random(100000);
            storage[i].world_y = next_random(100000);
            points[i] = &storage[i];
        }
        build_kdtree(points, 0, N, 0);
        int seen[N] = {0};
        for (int i = 0; i < N; i++)
            seen[points[i] - storage]++;
        int wrong = 0;
        for (int i = 0; i < N; i++)
            if (seen[i] != 1)
                wrong++;
        CHECK_INT(wrong, 0);
    }

    SUITE("heat: radius search matches brute force on clustered points");
    // Points drawn tightly enough that many fall inside the radius, which is
    // what exercises the pruning decisions.
    {
        enum {
            N = 400,
            TRACKS = 8
        };
        GpxPoint storage[N];
        for (int i = 0; i < N; i++) {
            storage[i] = (GpxPoint){0};
            storage[i].world_x = 93000000 + next_random(1200);
            storage[i].world_y = 93000000 + next_random(1200);
            storage[i].track_id = i % TRACKS;
        }
        check_against_brute_force(storage, N, TRACKS,
                                  HEAT_RADIUS_PIXELS * HEAT_RADIUS_PIXELS);
    }

    SUITE("heat: radius search matches brute force on sparse points");
    // Spread far enough apart that most branches prune, so a too-eager prune
    // shows up as a miss.
    {
        enum {
            N = 300,
            TRACKS = 5
        };
        GpxPoint storage[N];
        for (int i = 0; i < N; i++) {
            storage[i] = (GpxPoint){0};
            storage[i].world_x = 93000000 + next_random(50000);
            storage[i].world_y = 93000000 + next_random(50000);
            storage[i].track_id = i % TRACKS;
        }
        check_against_brute_force(storage, N, TRACKS,
                                  HEAT_RADIUS_PIXELS * HEAT_RADIUS_PIXELS);
    }

    SUITE("heat: radius search matches brute force on collinear points");
    // Every point on one line: the degenerate case for a 2-d tree, since one
    // axis never discriminates.
    {
        enum {
            N = 200,
            TRACKS = 4
        };
        GpxPoint storage[N];
        for (int i = 0; i < N; i++) {
            storage[i] = (GpxPoint){0};
            storage[i].world_x = 93000000;
            storage[i].world_y = 93000000 + i * 30;
            storage[i].track_id = i % TRACKS;
        }
        check_against_brute_force(storage, N, TRACKS,
                                  HEAT_RADIUS_PIXELS * HEAT_RADIUS_PIXELS);
    }

    SUITE("heat: a lone track has no heat");
    {
        enum {
            N = 50
        };
        GpxPoint storage[N];
        for (int i = 0; i < N; i++) {
            storage[i] = (GpxPoint){0};
            storage[i].world_x = 93000000 + i * 10;
            storage[i].world_y = 93000000;
            storage[i].track_id = 0;
        }
        check_against_brute_force(storage, N, 1,
                                  HEAT_RADIUS_PIXELS * HEAT_RADIUS_PIXELS);
    }

    SUITE("heat: single point and empty ranges");
    {
        GpxPoint storage[1] = {{0}};
        storage[0].world_x = 93000000;
        storage[0].world_y = 93000000;
        check_against_brute_force(storage, 1, 1, 100.0f);
    }

    SUITE("heat: the threaded maximum is the maximum over every point");
    {
        // The workers each keep their own maximum and the join reduces them,
        // rather than every point taking a shared lock to answer the same
        // question. Whatever the split, the answer has to be the one a single
        // pass would give.
        enum {
            TRACKS = 9,
            PER_TRACK = 120,
            N = TRACKS * PER_TRACK
        };
        static GpxTrack tracks[TRACKS];
        static GpxPoint storage[N];
        GpxCollection collection = {0};
        collection.tracks = tracks;
        collection.total_tracks = TRACKS;

        for (int t = 0; t < TRACKS; t++) {
            tracks[t] = (GpxTrack){0};
            tracks[t].points = &storage[t * PER_TRACK];
            tracks[t].total_points = PER_TRACK;
            tracks[t].has_path = true; // built with coordinates, as the parser would
            tracks[t].track_id = t;
            tracks[t].visible_in_list = true;
            for (int i = 0; i < PER_TRACK; i++) {
                // Deliberately overlapping, so the tracks see each other and
                // the heat is not uniformly one.
                tracks[t].points[i] = (GpxPoint){0};
                tracks[t].points[i].world_x = 93000000 + next_random(1200);
                tracks[t].points[i].world_y = 93000000 + next_random(1200);
                tracks[t].points[i].track_id = t;
            }
        }

        CHECK(calculate_heatmap(&collection, NULL));

        int highest = 0;
        for (int i = 0; i < N; i++)
            if (storage[i].heat > highest)
                highest = storage[i].heat;
        CHECK_INT(collection.max_heat, highest);
        // Overlapping tracks, so this is a real maximum rather than the
        // no-overlap floor.
        CHECK(collection.max_heat > 1);
    }
}
