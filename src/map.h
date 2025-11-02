#ifndef MAP_H
#define MAP_H

#include <unistd.h>
#include <math.h>
#include <curl/curl.h>
#include <sys/stat.h> // für mkdir + stat
#include <pthread.h>
#include "structs.h"
#include "fifo.h"
#include "tracks.h"

bool get_map_background(struct application *appl, GpxCollection* collection);
void *download_tiles(void *arg);
void pixelToTileAndOffset(int pixel_x, int pixel_y, int source_zoom, int target_zoom,
                          int *tile_x, int *tile_y,
                          int *pixel_in_tile_x, int *pixel_in_tile_y);
void free_tile_cache(TileTextureCache *cache);
bool tile_key_equal(MapTile a, MapTile b);


#endif