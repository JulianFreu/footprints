#ifndef TRACKS_H
#define TRACKS_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "app.h"
#include "gpx_types.h"
#include "map_types.h"

// Drops the rendered heat tiles and the spatial index behind them. Call
// whenever the set of visible tracks, or their heat, changes.
void tracks_invalidate_cache(GpxCollection *collection);
// Teardown counterpart: also releases the index's allocation.
void tracks_free_collection_cache(GpxCollection *collection);
void tracks_free_scratch(void);
void update_track_info_graphs(struct application *appl, const GpxCollection *collection);
SDL_Texture *get_or_render_track_tile(struct application *appl, GpxCollection *collection, MapTile key);
int find_track_near_click(GpxCollection *collection, int click_x, int click_y, int current_zoom, int max_pixel_distance);
void update_selected_track_overlay(struct application *appl, GpxCollection *collection);

#endif
