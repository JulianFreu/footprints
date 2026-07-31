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

// Fills `out` with the tiles covering the window and returns how many were
// written, never more than max_tiles.
int map_visible_tiles(const struct application *appl, VisibleTile *out, int max_tiles);

// Draws the map background for those tiles, fetching or queueing what is
// missing. The overlays that go on top are drawn by their own modules.
void map_draw_tiles(struct application *appl, const VisibleTile *tiles, int count);
void *download_tiles(void *arg);
void download_thread_stop(struct fifo *download_queue);
void conv_pixel_to_tile_and_offset(int pixel_x, int pixel_y, int source_zoom, int target_zoom,
                                   int *tile_x, int *tile_y,
                                   int *pixel_in_tile_x, int *pixel_in_tile_y);
// Whether a Stadia Maps key was compiled in. -stadiamaps needs one; the
// default OpenStreetMap tiles do not.
bool map_has_api_key(void);

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
