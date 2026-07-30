#ifndef APP_H
#define APP_H

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>

#include "config.h"
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
    int current_fps;
    Uint32 last_frame_time;
    bool mouse_over_ui;
    bool show_heat;
    bool update_window;
};

#endif
