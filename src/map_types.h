#ifndef MAP_TYPES_H
#define MAP_TYPES_H

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "config.h"

typedef struct MapTile {
    int tile_x;
    int tile_y;
    int zoom;
} MapTile;

typedef struct
{
    SDL_Point pos;
    int heat;
} HeatPoint;

typedef struct
{
    MapTile key;
    HeatPoint *points;
    int point_count;
    int capacity;
} CombinedTilePoints;

typedef struct
{
    MapTile key;
    SDL_Texture *texture;
} TrackTileTexture;

typedef struct
{
    TrackTileTexture *entries;
    int size;
    int capacity;
} TrackTileTextureCache;

// Both texture caches are a plain grow-only array of fixed-size entries, so
// they share one growth routine; see tile_cache_reserve in map.c.

typedef struct TileTexture {
    MapTile key;
    SDL_Texture *texture;
} TileTexture;

typedef struct
{
    TileTexture *entries;
    int size;
    int capacity;
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
