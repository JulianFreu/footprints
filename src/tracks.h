#ifndef TRACKS_H
#define TRACKS_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "app.h"
#include "gpx_types.h"
#include "map_types.h"

// Drops the rendered heat tiles, keeping the spatial index behind them. Call
// whenever the set of visible tracks, or their heat, changes. The index spans
// every point whatever the filters say, and is the background job's to build.
void tracks_invalidate_filtered_view(GpxCollection *collection);
// Teardown counterpart: also releases the index's allocation.
void tracks_free_collection_cache(GpxCollection *collection);
void tracks_free_scratch(struct application *appl);
// Builds the sidebar's graphs, and the series behind them, for whichever track
// is selected. A no-op on a frame where the selection has not changed.
void update_track_info_graphs(struct application *appl, const GpxCollection *collection);
// Releases both. Teardown counterpart to the above.
void tracks_free_graphs(struct application *appl);
// A dot on one track's route, at one of its points. Drawn in the map's layer,
// so a panel sliding over it covers it the way it covers the track itself.
void tracks_draw_point_marker(struct application *appl, const GpxCollection *collection,
                              int track_id, int point_index);
// Draws the heat overlay over the tiles map_visible_tiles produced.
void tracks_draw_heat_tiles(struct application *appl, GpxCollection *collection,
                            const VisibleTile *tiles, int count);
// Draws the selected track's polyline over the whole window.
void tracks_draw_selected_overlay(struct application *appl);
int find_track_near_click(GpxCollection *collection, int click_x, int click_y, int current_zoom, int max_pixel_distance);
void update_selected_track_overlay(struct application *appl, GpxCollection *collection);

// The colour at `normalized` (0 cold, 1 hot) along the heat ramp. The ramp
// itself stays private to tracks.c; this is here so the settings panel can
// preview the same colours the map is drawn with rather than keep a second
// copy that would drift.
SDL_Color heat_ramp_color(float normalized);

#endif
