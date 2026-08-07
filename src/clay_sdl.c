// The single translation unit that carries Clay's implementation and the
// vendored SDL2 renderer. Everything else includes clay.h for declarations
// only.
#include "ui_types.h" // SDL2_Font, which the vendored renderer expects

// Ahead of the macros below, so that none of them renames a declaration. The
// vendored renderer includes these three itself, and the guards make that a
// no-op by the time it does.
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

// The renderer rasterises every string and uploads every icon on every frame,
// then throws both away. It is vendored and not edited, so the five calls it
// does that with are pointed at the cache instead -- which is the whole of how
// that becomes once per string rather than once per frame.
#include "render_cache.h"
#define TTF_RenderUTF8_Blended render_cache_text_surface
#define TTF_SetFontSize render_cache_set_font_size
#define SDL_CreateTextureFromSurface render_cache_texture
#define SDL_DestroyTexture render_cache_release_texture
#define SDL_FreeSurface render_cache_release_surface

#include "clay_renderer_sdl.c"

#undef TTF_RenderUTF8_Blended
#undef TTF_SetFontSize
#undef SDL_CreateTextureFromSurface
#undef SDL_DestroyTexture
#undef SDL_FreeSurface

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
