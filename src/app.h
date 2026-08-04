#ifndef APP_H
#define APP_H

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>

#include "config.h"
#include "background.h"
#include "garmin.h"
#include "map_types.h"
#include "ui_types.h"
#include "zoom.h"

// Composition root: the one object that owns the window, renderer, caches and
// input state for the running application.
struct application {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int window_width;
    int window_height;
    int zoom;
    int mouse_x;
    int mouse_y;
    int wheel_y;
    int world_x;
    int world_y;
    int running;
    int dragging;
    bool left_mouse_button_pressed;
    int selected_track;
    struct fifo download_queue;
    pthread_t download_thread;
    bool download_thread_started;
    SDL2_Font fonts[1];
    UiIcons icons;
    TileTextureCache tile_cache;
    // Where the picture is between two whole zoom levels, and that gap written
    // in the form the drawing wants. The transform is derived from the
    // transition and never set on its own; see MapTransform. It must never be
    // left at a zero scale -- initialise from MAP_TRANSFORM_IDENTITY, not {0}.
    ZoomTransition zoom_transition;
    MapTransform map_transform;
    // The selected track's polyline, drawn in screen space over the whole
    // window. One texture: it is built for wherever the camera is, so a
    // per-zoom set of them was stale for every zoom but the current one.
    SDL_Texture *selected_track_overlay;
    OverlayKey overlay_key;
    // Which track the elevation profile was last built for, so it is not
    // redone on a frame where the selection has not moved.
    int rendered_overlay_track;
    // Screen-space scratch for the selected track's polyline, kept between
    // frames and grown only when a longer track is selected.
    SDL_Point *overlay_points;
    int overlay_points_capacity;
    // Frame timing. `last_counter` is a performance-counter reading taken once
    // per iteration, and `delta_time` the seconds since the previous one --
    // measured across the whole iteration, so it includes the time spent
    // waiting for the display and for events.
    Uint64 last_counter;
    float delta_time;
    bool mouse_over_ui;
    bool show_heat;
    // Raised by anything that changes what the window should show. The frame
    // loop clears it before it draws, so a request made while drawing survives
    // to the frame after.
    bool redraw_requested;
    // The long-running work. While this is busy the collection belongs to it.
    BackgroundJob background;
    // The Garmin import. It writes files rather than touching the collection,
    // so it runs alongside the panels instead of replacing them, and the
    // library load that reads what it fetched is an ordinary background job.
    GarminJob garmin;
    // The collection this application is showing. Clay's hover callbacks are
    // handed one pointer, so anything a button needs has to hang off it.
    GpxCollection *collection;
};

// Asks for the next frame to be drawn. This is how a module keeps an animation
// running without the frame loop having to know the animation exists.
static inline void app_request_redraw(struct application *appl) {
    appl->redraw_requested = true;
}

#endif
