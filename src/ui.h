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

float GetDeltaTime(Uint32 lastFrameTime);
void clay_Init(struct application *appl);
void clay_DrawUI(struct application *appl, GpxCollection* collection);
void clay_freeMemory();

#endif