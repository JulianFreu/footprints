#ifndef CLAY_SDL_H
#define CLAY_SDL_H

#include <SDL2/SDL.h>

#include "clay.h"
#include "ui_types.h"

// Thin wrappers over the vendored Clay SDL2 renderer.
//
// Both of the renderer's entry points are static, and the file is third-party
// code we do not edit, so it is compiled once in clay_sdl.c along with Clay's
// own implementation and reached through these. That keeps the ~5,000 lines of
// vendored code out of every UI translation unit, which previously recompiled
// all of it on any edit.

Clay_Dimensions clay_sdl_measure_text(Clay_StringSlice text,
                                      Clay_TextElementConfig *config,
                                      void *user_data);

void clay_sdl_render(SDL_Renderer *renderer, Clay_RenderCommandArray commands,
                     SDL2_Font *fonts);

#endif
