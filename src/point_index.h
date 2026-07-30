#ifndef POINT_INDEX_H
#define POINT_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#include "gpx_types.h"
#include "map_types.h"

// A sorted index over every visible track point, keyed by the Morton code of
// the MAX_ZOOM tile the point falls in.
//
// Interleaving the tile's x and y bits means the code of a tile at any zoom is
// a prefix of the codes of every MAX_ZOOM tile inside it. One sorted array
// therefore answers "which points are in this tile" at every zoom level, with
// a binary search instead of a pass over the whole library.

// (Re)builds the index from the collection's visible tracks. Cheap to call
// when already valid: it returns immediately.
bool point_index_ensure(GpxCollection *collection);

// Marks the index stale without releasing its memory, so the next build can
// reuse the allocation.
void point_index_invalidate(TrackPointIndex *index);

void point_index_free(TrackPointIndex *index);

// The half-open range [*from, *to) of index entries falling inside `tile`.
// Both are zero when the index is empty or the tile holds no points.
void point_index_tile_range(const TrackPointIndex *index, MapTile tile,
                            int *from, int *to);

// Morton code of a tile coordinate pair. Exposed for the tests.
uint64_t point_index_tile_key(uint32_t tile_x, uint32_t tile_y);

#endif
