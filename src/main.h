#ifndef MAIN_H
#define MAIN_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include "structs.h"
#include "ui.h"
#include "map.h"
#include "gpxParser.h"
#include "tracks.h"
#include "filters.h"
#include "heat.h"


bool sdl_initialize(struct application *appl);
bool appl_cleanup(struct application *appl, GpxCollection * collection, int exit_status);
bool handle_events(struct application *appl, GpxCollection *collection);

#endif