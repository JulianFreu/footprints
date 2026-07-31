#ifndef MAP_H
#define MAP_H

#include <stdbool.h>
#include <stddef.h>

#include <SDL2/SDL.h>

#include "app.h"
#include "gpx_types.h"
#include "map_types.h"

// Set once from the command line before the download thread starts; read by it
// from then on.
extern bool use_osm_tiles;

// Written by the tile download thread, read by the main loop each frame to
// decide whether the map still needs redrawing.
extern _Atomic bool download_in_progress;

bool get_map_background(struct application *appl, GpxCollection *collection);
void *download_tiles(void *arg);
void download_thread_stop(struct fifo *download_queue);
void conv_pixel_to_tile_and_offset(int pixel_x, int pixel_y, int source_zoom, int target_zoom,
                                   int *tile_x, int *tile_y,
                                   int *pixel_in_tile_x, int *pixel_in_tile_y);
bool tile_key_equal(MapTile a, MapTile b);

// Bounded, least-recently-used texture cache, shared by the map background and
// the track heat overlay.
SDL_Texture *tile_cache_lookup(TileTextureCache *cache, MapTile key);
// Takes ownership of `texture` on success; the caller destroys it on failure.
bool tile_cache_insert(TileTextureCache *cache, MapTile key, SDL_Texture *texture);
void tile_cache_free(TileTextureCache *cache);

// Formats the on-disk path of a tile. The single place that layout is spelled.
void tile_cache_path(char *out, size_t size, MapTile tile);

#endif
