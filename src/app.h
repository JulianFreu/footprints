#ifndef APP_H
#define APP_H

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>

#include "config.h"
#include "background.h"
#include "import_job.h"
#include "map_types.h"
#include "profiler.h"
#include "track_series.h"
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
    // Indexed by UiFont. Clay's fontId selects into this, which is how a text
    // element asks for the font already at the size it wants.
    SDL2_Font fonts[UI_FONT_COUNT];
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
    // Which track the graphs were last built for, and the pixel size they were
    // built at, so they are not redone on a frame where neither has moved. The
    // size is part of it because the graphs fill the sidebar, and so change
    // shape with the window rather than only with the selection.
    int rendered_overlay_track;
    int rendered_graph_width;
    int rendered_graph_height;
    // The numbers behind those graphs, indexed by TrackSeriesKind. Kept as
    // well as the pictures, because the readout has to answer what the values
    // are at the point being hovered over.
    TrackSeries track_series[TRACK_SERIES_COUNT];
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
    // Where each of those iterations went, phase by phase. Sampled whether or
    // not the overlay is being drawn, so the graph is already full the moment
    // it is switched on -- including the frames spent switching it on.
    Profiler profiler;
    bool mouse_over_ui;
    bool show_heat;
    // Raised by anything that changes what the window should show. The frame
    // loop clears it before it draws, so a request made while drawing survives
    // to the frame after.
    bool redraw_requested;
    // The long-running work. While this is busy the collection belongs to it.
    BackgroundJob background;
    // The two activity imports, one per provider. They write files rather than
    // touching the collection, so they run alongside the panels instead of
    // replacing them, and the library load that reads what they fetched is an
    // ordinary background job.
    ImportJob garmin;
    ImportJob strava;
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
