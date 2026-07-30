#include "../src/point_index.c"

#include "harness.h"

#include <string.h>

// The predicate the index replaced: a point belongs to a tile when shifting its
// world coordinates down to that zoom and dividing by the tile size lands on
// the tile. This is what get_or_render_track_tile used to test for every point
// of every track, and what the index now has to agree with exactly.
static bool point_is_in_tile(const GpxPoint *point, MapTile tile) {
    int zoom_diff = MAX_ZOOM - tile.zoom;
    return (point->world_x >> zoom_diff) / TILE_SIZE == tile.tile_x &&
           (point->world_y >> zoom_diff) / TILE_SIZE == tile.tile_y;
}

static int brute_force_count(const GpxCollection *collection, MapTile tile) {
    int count = 0;
    for (int t = 0; t < collection->total_tracks; t++) {
        const GpxTrack *track = &collection->tracks[t];
        if (!track->visible_in_list)
            continue;
        for (int i = 0; i < track->total_points; i++)
            if (point_is_in_tile(&track->points[i], tile))
                count++;
    }
    return count;
}

static unsigned long rng_state = 99;
static int next_random(int bound) {
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (int)((rng_state >> 33) % (unsigned long)bound);
}

void run_point_index_tests(void) {
    SUITE("point_index: Morton codes interleave and preserve prefixes");
    // Tile (0,0) is the origin of every quad, so its code is zero at any depth.
    CHECK_INT(point_index_tile_key(0, 0), 0);
    // x contributes the even bits, y the odd ones.
    CHECK_INT(point_index_tile_key(1, 0), 1);
    CHECK_INT(point_index_tile_key(0, 1), 2);
    CHECK_INT(point_index_tile_key(1, 1), 3);
    CHECK_INT(point_index_tile_key(2, 0), 4);
    CHECK_INT(point_index_tile_key(3, 3), 15);
    // The prefix property the whole scheme rests on: the code of a tile at
    // zoom z, shifted up two bits per extra zoom level, bounds the codes of the
    // four tiles it splits into.
    for (int trial = 0; trial < 200; trial++) {
        uint32_t tx = (uint32_t)next_random(1 << 10);
        uint32_t ty = (uint32_t)next_random(1 << 10);
        uint64_t parent = point_index_tile_key(tx, ty);
        for (int child = 0; child < 4; child++) {
            uint64_t kid = point_index_tile_key(tx * 2 + (child & 1),
                                                ty * 2 + ((child >> 1) & 1));
            CHECK(kid >= (parent << 2));
            CHECK(kid < ((parent + 1) << 2));
        }
    }

    SUITE("point_index: tile ranges match a full scan at every zoom");
    {
        enum {
            TRACKS = 12,
            PER_TRACK = 220
        };
        static GpxTrack tracks[TRACKS];
        static GpxPoint storage[TRACKS][PER_TRACK];
        GpxCollection collection = {0};
        collection.tracks = tracks;
        collection.total_tracks = TRACKS;

        // Spread over a few zoom-12 tiles around Munich, so tiles hold varying
        // numbers of points and some hold none.
        for (int t = 0; t < TRACKS; t++) {
            tracks[t] = (GpxTrack){0};
            tracks[t].points = storage[t];
            tracks[t].total_points = PER_TRACK;
            tracks[t].track_id = t;
            // Two tracks are filtered out, to pin down that the index only
            // holds visible points.
            tracks[t].visible_in_list = (t % 6 != 0);
            for (int i = 0; i < PER_TRACK; i++) {
                storage[t][i] = (GpxPoint){0};
                storage[t][i].world_x = 142800000 + next_random(300000);
                storage[t][i].world_y = 93100000 + next_random(300000);
                storage[t][i].track_id = t;
            }
        }

        CHECK(point_index_ensure(&collection));

        int visible_points = 0;
        for (int t = 0; t < TRACKS; t++)
            if (tracks[t].visible_in_list)
                visible_points += PER_TRACK;
        CHECK_INT(collection.point_index.count, visible_points);

        // Sorted, which is what the binary search assumes.
        int unsorted = 0;
        for (int i = 1; i < collection.point_index.count; i++)
            if (collection.point_index.entries[i - 1].tile_key >
                collection.point_index.entries[i].tile_key)
                unsorted++;
        CHECK_INT(unsorted, 0);

        // Walk the tiles covering the data at several zooms and compare the
        // index's answer against the scan it replaced.
        int mismatches = 0;
        int tiles_with_points = 0;
        for (int zoom = 8; zoom <= MAX_ZOOM; zoom += 3) {
            int shift = MAX_ZOOM - zoom;
            int base_x = (142800000 >> shift) / TILE_SIZE;
            int base_y = (93100000 >> shift) / TILE_SIZE;
            int span = (zoom >= 16) ? 24 : 4;
            for (int dx = -1; dx <= span; dx++) {
                for (int dy = -1; dy <= span; dy++) {
                    MapTile tile = {base_x + dx, base_y + dy, zoom};
                    int from, to;
                    point_index_tile_range(&collection.point_index, tile, &from, &to);
                    int expected = brute_force_count(&collection, tile);
                    if (to - from != expected)
                        mismatches++;
                    if (expected > 0)
                        tiles_with_points++;

                    // Every entry actually returned must belong to the tile.
                    for (int i = from; i < to; i++)
                        if (!point_is_in_tile(collection.point_index.entries[i].point, tile))
                            mismatches++;
                }
            }
        }
        CHECK_INT(mismatches, 0);
        // Guard against the comparison passing because every tile was empty.
        CHECK(tiles_with_points > 20);

        // Every visible point is reachable through the zoom-0 root tile.
        MapTile root = {0, 0, 0};
        int from, to;
        point_index_tile_range(&collection.point_index, root, &from, &to);
        CHECK_INT(to - from, visible_points);

        point_index_free(&collection.point_index);
    }

    SUITE("point_index: invalidate forces a rebuild against the new visible set");
    {
        enum {
            PER_TRACK = 40
        };
        static GpxTrack tracks[2];
        static GpxPoint storage[2][PER_TRACK];
        GpxCollection collection = {0};
        collection.tracks = tracks;
        collection.total_tracks = 2;
        for (int t = 0; t < 2; t++) {
            tracks[t] = (GpxTrack){0};
            tracks[t].points = storage[t];
            tracks[t].total_points = PER_TRACK;
            tracks[t].visible_in_list = true;
            for (int i = 0; i < PER_TRACK; i++) {
                storage[t][i] = (GpxPoint){0};
                storage[t][i].world_x = 142800000 + i * 500;
                storage[t][i].world_y = 93100000 + i * 500;
            }
        }

        CHECK(point_index_ensure(&collection));
        CHECK_INT(collection.point_index.count, 2 * PER_TRACK);

        // A second call while valid must not double the contents.
        CHECK(point_index_ensure(&collection));
        CHECK_INT(collection.point_index.count, 2 * PER_TRACK);

        tracks[1].visible_in_list = false;
        point_index_invalidate(&collection.point_index);
        CHECK(point_index_ensure(&collection));
        CHECK_INT(collection.point_index.count, PER_TRACK);

        point_index_free(&collection.point_index);
    }

    SUITE("point_index: an empty collection yields empty ranges");
    {
        GpxCollection collection = {0};
        CHECK(point_index_ensure(&collection));
        CHECK_INT(collection.point_index.count, 0);
        MapTile tile = {100, 100, 12};
        int from, to;
        point_index_tile_range(&collection.point_index, tile, &from, &to);
        CHECK_INT(from, 0);
        CHECK_INT(to, 0);
        point_index_free(&collection.point_index);
    }
}
