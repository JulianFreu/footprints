#ifndef TRACKS_H
#define TRACKS_H

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "structs.h"
#include "map.h"

void free_track_tile_cache(TrackTileTextureCache *cache);
void update_track_info_graphs(struct application *appl, GpxCollection collection);
SDL_Texture *get_or_render_track_tile(struct application *appl, GpxCollection *collection, MapTile key);
int find_track_near_click(GpxCollection *collection, int click_x, int click_y, int current_zoom, int max_pixel_distance);
void update_selected_track_overlay(struct application *appl, GpxCollection *collection);

#endif