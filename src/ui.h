#ifndef ui_h
#define ui_h

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "app.h"
#include "gpx_types.h"
#include "ui_types.h"
#include "tracks.h"
#include "heat.h"
#include "filters.h"

float get_delta_time(Uint32 lastFrameTime);
void clay_init(struct application *appl);
void clay_draw_ui(struct application *appl, GpxCollection *collection);
void clay_free_memory();
void ui_load_icons(struct application *appl);
void ui_free_icons(struct application *appl);

#endif