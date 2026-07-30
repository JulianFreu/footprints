// The single translation unit that carries Clay's implementation and the
// vendored SDL2 renderer. Everything else includes clay.h for declarations
// only.
#include "ui_types.h" // SDL2_Font, which the vendored renderer expects

#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "clay_renderer_sdl.c"

#include "clay_sdl.h"

Clay_Dimensions clay_sdl_measure_text(Clay_StringSlice text,
                                      Clay_TextElementConfig *config,
                                      void *user_data) {
    return SDL2_MeasureText(text, config, user_data);
}

void clay_sdl_render(SDL_Renderer *renderer, Clay_RenderCommandArray commands,
                     SDL2_Font *fonts) {
    Clay_SDL2_Render(renderer, commands, fonts);
}
