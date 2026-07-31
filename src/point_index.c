#include "point_index.h"

#include <stdio.h>
#include <stdlib.h>

// Spreads the low 20 bits of `value` out into every other bit, so two of these
// can be woven together into one Morton code. 20 bits is MAX_ZOOM, i.e. the
// widest tile coordinate the map ever addresses.
static uint64_t interleave_bits(uint32_t value) {
    uint64_t x = value & 0xFFFFFu;
    x = (x | (x << 16)) & 0x0000FFFF0000FFFFULL;
    x = (x | (x << 8)) & 0x00FF00FF00FF00FFULL;
    x = (x | (x << 4)) & 0x0F0F0F0F0F0F0F0FULL;
    x = (x | (x << 2)) & 0x3333333333333333ULL;
    x = (x | (x << 1)) & 0x5555555555555555ULL;
    return x;
}

uint64_t point_index_tile_key(uint32_t tile_x, uint32_t tile_y) {
    return (interleave_bits(tile_y) << 1) | interleave_bits(tile_x);
}

static int compare_indexed_points(const void *a, const void *b) {
    uint64_t ka = ((const IndexedPoint *)a)->tile_key;
    uint64_t kb = ((const IndexedPoint *)b)->tile_key;
    return (ka > kb) - (ka < kb);
}

// Index of the first entry whose key is >= `key`.
static int lower_bound(const TrackPointIndex *index, uint64_t key) {
    int lo = 0, hi = index->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (index->entries[mid].tile_key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

bool point_index_ensure(GpxCollection *collection) {
    TrackPointIndex *index = &collection->point_index;
    if (index->valid)
        return true;

    index->count = 0;

    int needed = 0;
    for (int t = 0; t < collection->total_tracks; t++)
        if (collection->tracks[t].visible_in_list)
            needed += collection->tracks[t].total_points;

    if (needed > index->capacity) {
        IndexedPoint *grown = realloc(index->entries, (size_t)needed * sizeof(IndexedPoint));
        if (!grown) {
            fprintf(stderr, "Could not grow the track point index\n");
            return false;
        }
        index->entries = grown;
        index->capacity = needed;
    }

    for (int t = 0; t < collection->total_tracks; t++) {
        const GpxTrack *track = &collection->tracks[t];
        if (!track->visible_in_list)
            continue;
        for (int i = 0; i < track->total_points; i++) {
            const GpxPoint *point = &track->points[i];
            index->entries[index->count++] = (IndexedPoint){
                .tile_key = point_index_tile_key((uint32_t)(point->world_x / TILE_SIZE),
                                                 (uint32_t)(point->world_y / TILE_SIZE)),
                .point = point};
        }
    }

    qsort(index->entries, (size_t)index->count, sizeof(IndexedPoint),
          compare_indexed_points);
    index->valid = true;
    return true;
}

void point_index_invalidate(TrackPointIndex *index) {
    index->valid = false;
    index->count = 0;
}

void point_index_free(TrackPointIndex *index) {
    free(index->entries);
    index->entries = NULL;
    index->count = 0;
    index->capacity = 0;
    index->valid = false;
}

void point_index_tile_range(const TrackPointIndex *index, MapTile tile,
                            int *from, int *to) {
    // Every MAX_ZOOM tile inside this one shares its Morton prefix, so the
    // points wanted are the half-open range between that prefix and the next
    // one along.
    int zoom_shift = 2 * (MAX_ZOOM - tile.zoom);
    uint64_t first = point_index_tile_key((uint32_t)tile.tile_x, (uint32_t)tile.tile_y) << zoom_shift;
    uint64_t last = first + (1ULL << zoom_shift);

    *from = lower_bound(index, first);
    *to = lower_bound(index, last);
}
