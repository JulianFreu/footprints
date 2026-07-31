#ifndef APP_H
#define APP_H

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>

#include "config.h"
#include "background.h"
#include "map_types.h"
#include "ui_types.h"

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
    SDL_Texture *selected_track_overlay[MAX_ZOOM + 1]; // +1 for zoom level 0 to 20
    // Which track the overlay and the elevation profile were last built for,
    // so neither is redone on a frame where the selection has not moved.
    int rendered_overlay_track;
    // Screen-space scratch for the selected track's polyline, kept between
    // frames and grown only when a longer track is selected.
    SDL_Point *overlay_points;
    int overlay_points_capacity;
    int current_fps;
    Uint32 last_frame_time;
    bool mouse_over_ui;
    bool show_heat;
    bool update_window;
    // The long-running work. While this is busy the collection belongs to it.
    BackgroundJob background;
    // The collection this application is showing. Clay's hover callbacks are
    // handed one pointer, so anything a button needs has to hang off it.
    GpxCollection *collection;
};

#endif
