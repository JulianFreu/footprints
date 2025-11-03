#ifndef ui_h
#define ui_h

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>


#include "structs.h"
#include "tracks.h"
#include "heat.h"
#include "filters.h"

float get_delta_time(Uint32 lastFrameTime);
void clay_init(struct application *appl);
void clay_draw_UI(struct application *appl, GpxCollection* collection);
void clay_free_memory();

#endif