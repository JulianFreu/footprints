#ifndef MAP_TYPES_H
#define MAP_TYPES_H

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anim.h"
#include "config.h"

// gpx_types.h includes this header for the caches below, so the point type can
// only be named, not defined, here.
struct GpxPoint;

typedef struct MapTile {
    int tile_x;
    int tile_y;
    int zoom;
} MapTile;

// One visible track point, tagged with the Morton code of the MAX_ZOOM tile it
// falls in. Interleaving the tile's x and y bits is what makes a tile's points
// contiguous once the array is sorted: a tile at zoom z is a prefix of that
// code, so its points are one range rather than a scan of the whole library.
typedef struct
{
    uint64_t tile_key;
    const struct GpxPoint *point;
} IndexedPoint;

// Sorted index over every visible point, shared by all zoom levels. Rebuilt
// only when the visible set changes, which is also when the rendered tiles are
// dropped.
typedef struct
{
    IndexedPoint *entries;
    int count;
    int capacity;
    bool valid;
} TrackPointIndex;

// Everything the selected-track overlay is drawn from. It is a screen-space
// picture of a world-space thing, so it is only valid for the camera it was
// built at -- which is why it is compared rather than assumed, and why a
// texture per zoom level never worked.
typedef struct OverlayKey {
    int track;
    int world_x;
    int world_y;
    int zoom;
    int window_width;
    int window_height;
} OverlayKey;

// How the map layers are drawn relative to where the model says they are.
//
// Derived once a frame from the ZoomTransition in zoom.h, and the only thing
// that knows a zoom is part-way done. The model snaps to a whole zoom level
// while the picture catches up, so tile selection, the on-disk cache, the point
// index and the heat rasterisation go on seeing whole zooms and only the blit
// knows about the fraction. Scaling is about an anchor in screen pixels so the
// point under the cursor stays under it.
typedef struct MapTransform {
    float scale;
    float anchor_x;
    float anchor_y;
} MapTransform;

// A tile that is on screen this frame, and where it lands. Produced once per
// frame and handed to each layer in turn.
//
// Float, and carrying its own size rather than assuming TILE_SIZE, because a
// scaled transform stretches a tile to cover more or less than one.
typedef struct VisibleTile {
    MapTile tile;
    float screen_x;
    float screen_y;
    float size;
} VisibleTile;

// One cached tile texture. The map background and the track heat overlay hold
// structurally identical caches, so they share one type and one set of
// operations rather than two copies that drifted apart.
typedef struct TileTexture {
    MapTile key;
    SDL_Texture *texture;
    // Value of the owning cache's clock when this entry was last handed out.
    // The oldest is what gets evicted once the cache is full.
    uint64_t last_used;
    // How much of the tile is showing, 0..1. A zeroed Anim reads 0, which is
    // invisible rather than opaque, so the insert settles this at 1 and only a
    // caller that wants a fade winds it back to 0 -- which is what leaves the
    // heat cache, which shares this type, drawing at full strength without
    // knowing the field is here.
    Anim fade;
} TileTexture;

typedef struct
{
    TileTexture *entries;
    int size;
    int capacity;
    uint64_t clock;
    // Which entry sits in each hash slot, as its index in `entries` plus one so
    // that zero reads as an empty slot -- which is what lets a zeroed cache be a
    // valid empty one, the way both of these are created.
    //
    // An index rather than a pointer: growing the cache reallocs `entries`, and
    // pointers into it would all have to be found and rewritten.
    int32_t lookup[TILE_LOOKUP_SLOTS];
} TileTextureCache;

// Growable buffer for a curl response body.
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Bounded queue of tiles awaiting download. One slot is always left unused so
// a full queue stays distinguishable from an empty one; see fifo.c.
struct fifo {
    int read_p;
    int write_p;
    // Set by the main thread to ask the download worker to return. Checked
    // under `lock`, alongside the condition the worker waits on.
    bool stop;
    MapTile tile[FIFO_DEPTH];
    MapTile tile_in_dl;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

#endif
