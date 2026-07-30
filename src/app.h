#ifndef APP_H
#define APP_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "config.h"
#include "map_types.h"
#include "ui_types.h"

// Composition root: the one object that owns the window, renderer, caches and
// input state for the running application.
struct application {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *map;
    SDL_Texture *tex_tracks;
    int window_width;
    int window_height;
    int zoom;
    int mouse_x;
    int mouse_y;
    int wheel_y;
    int world_x;
    int world_y;
    double center_coord_x;
    double center_coord_y;
    int running;
    int dragging;
    bool leftMouseButtonPressed;
    int selected_track;
    struct fifo download_queue;
    SDL2_Font fonts[1];
    UiIcons icons;
    TileTextureCache tile_cache;
    SDL_Texture *selected_track_overlay[MAX_ZOOM + 1]; // +1 for zoom level 0 to 20
    int currentFPS;
    SDL_Event event;
    Uint32 lastFrameTime;
    bool mouseOverUI;
    bool show_heat;
    bool update_window;
};

#endif
