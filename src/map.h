#ifndef MAP_H
#define MAP_H

#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h> // mkdir + stat
#include <unistd.h>

#include "app.h"
#include "fifo.h"
#include "gpx_types.h"
#include "map_types.h"

bool get_map_background(struct application *appl, GpxCollection *collection);
void *download_tiles(void *arg);
void conv_pixel_to_tile_and_offset(int pixel_x, int pixel_y, int source_zoom, int target_zoom,
                                   int *tile_x, int *tile_y,
                                   int *pixel_in_tile_x, int *pixel_in_tile_y);
void free_tile_cache(TileTextureCache *cache);
bool tile_key_equal(MapTile a, MapTile b);

#endif
