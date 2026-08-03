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

// SDL_FRect and SDL_RenderCopyF, which the map layers are drawn with, arrived
// in 2.0.10. Every distribution still shipping SDL2 is well past it; this turns
// the one that is not into a sentence rather than a link error.
#if !SDL_VERSION_ATLEAST(2, 0, 10)
#error "footprints needs SDL 2.0.10 or newer for SDL_RenderCopyF"
#endif

// What a MapTransform is when nothing is easing. A zeroed one has a scale of
// zero and draws nothing, so this is not something to leave to a {0}.
#define MAP_TRANSFORM_IDENTITY \
    (MapTransform){            \
        .scale = 1.0f, .anchor_x = 0.0f, .anchor_y = 0.0f}

// Puts a rectangle given in unscaled screen pixels where the transform says it
// should be drawn.
SDL_FRect map_transform_rect(const struct application *appl,
                             float x, float y, float w, float h);

// The other direction for a point: what unscaled position a window pixel is
// showing. This is what anything reading the mouse has to go through while a
// zoom is easing, because the projection below is deliberately about the model
// and does not know the transform exists.
void map_screen_untransform(const struct application *appl, float screen_x, float screen_y,
                            float *unscaled_x, float *unscaled_y);

// Zooms by `steps` whole levels about the mouse, easing the picture into it.
void map_zoom_by_wheel(struct application *appl, int steps);

// Advances whatever the map still has moving, and asks for another frame if
// anything did. A no-op once the picture has settled.
void map_update(struct application *appl, float dt);

// Fills `out` with the tiles covering the window and returns how many were
// written, never more than max_tiles.
int map_visible_tiles(const struct application *appl, VisibleTile *out, int max_tiles);

// Draws the map background for those tiles, fetching or queueing what is
// missing. The overlays that go on top are drawn by their own modules.
void map_draw_tiles(struct application *appl, const VisibleTile *tiles, int count);
void *download_tiles(void *arg);
// Registers the event the download worker uses to report a tile has landed.
// Main thread, before the worker starts.
void map_init_events(void);
// Offered every SDL event; acts only on that one.
void map_handle_event(const SDL_Event *event);
void download_thread_stop(struct fifo *download_queue);
void conv_pixel_to_tile_and_offset(int pixel_x, int pixel_y, int source_zoom, int target_zoom,
                                   int *tile_x, int *tile_y,
                                   int *pixel_in_tile_x, int *pixel_in_tile_y);

// The projection between the world and the window, in one place.
//
// World coordinates are pixels at MAX_ZOOM; a screen pixel covers
// 1 << (MAX_ZOOM - zoom) of them. That shift used to be written out at each of
// the six sites that needed it, in four slightly different arrangements. The
// tile grid keeps its own integer arithmetic in
// conv_pixel_to_tile_and_offset: a tile lands on a whole pixel by
// construction, and it is the one thing here that is not a projection.

// World pixels to one screen pixel at `zoom`. map_world_per_pixel_at is
// declared in zoom.h, which this header pulls in through app.h.
int map_world_per_pixel(const struct application *appl);

// Where a world point lands in the window, and what world point is under a
// window position. Screen coordinates are float: the window is the only place
// a fraction of a world pixel means anything.
void map_world_to_screen(const struct application *appl, int world_x, int world_y,
                         float *screen_x, float *screen_y);
void map_screen_to_world(const struct application *appl, float screen_x, float screen_y,
                         int *world_x, int *world_y);
// Whether a Stadia Maps key was compiled in. -stadiamaps needs one; the
// default OpenStreetMap tiles do not.
bool map_has_api_key(void);

bool tile_key_equal(MapTile a, MapTile b);

// Bounded, least-recently-used texture cache, shared by the map background and
// the track heat overlay.
SDL_Texture *tile_cache_lookup(TileTextureCache *cache, MapTile key);
// The same lookup for a caller that needs the entry itself -- its fade, in
// practice. The pointer is good only until the next insert.
TileTexture *tile_cache_find(TileTextureCache *cache, MapTile key);
// Advances every entry's fade by `dt` seconds. Returns whether any of them
// moved, which is what asks for another frame.
bool tile_cache_tick_fades(TileTextureCache *cache, float dt);
// Takes ownership of `texture` on success and hands back the entry it went
// into, or NULL if it could not be stored -- the caller destroys it then. The
// pointer is good only until the next insert.
TileTexture *tile_cache_insert(TileTextureCache *cache, MapTile key, SDL_Texture *texture);
// Empties the cache but keeps its allocation, for a caller that is about to
// fill it again. tile_cache_free gives the allocation back as well.
void tile_cache_clear(TileTextureCache *cache);
void tile_cache_free(TileTextureCache *cache);

// Formats the on-disk path of a tile. The single place that layout is spelled.
void tile_cache_path(char *out, size_t size, MapTile tile);

#endif
