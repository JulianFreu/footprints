#ifndef UI_TYPES_H
#define UI_TYPES_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "filter_types.h"

typedef struct AnimationState {
    bool opening;
    bool closing;
    // Eased 0..1 position the layout offsets are scaled by.
    float animation;
    // Raw 0..90 position, read as degrees so sin() supplies the easing.
    float progress;
} AnimationState;

typedef struct UIState {
    AnimationState right_sidebar;
    AnimationState run_list;
    AnimationState filters_animation;
    bool text_input_mode;
    char text_input_buffer[INPUT_BUFFER_SIZE];
    size_t text_input_length;
    uint16_t active_filter_id;
} UIState;

typedef struct
{
    uint32_t font_id;
    TTF_Font *font;
} SDL2_Font;

// Icon surfaces are decoded once at startup and handed to Clay by pointer every
// frame. Clay does not take ownership, so these are freed in appl_cleanup.
typedef struct
{
    SDL_Surface *menu_burger;
    SDL_Surface *date;
    SDL_Surface *clock;
    SDL_Surface *duration;
    SDL_Surface *pace;
    SDL_Surface *distance;
    SDL_Surface *elev_up;
    SDL_Surface *elev_down;
    SDL_Surface *peak;
    // Regenerated only when the selected track changes, not per frame.
    SDL_Surface *elev_profile;
} UiIcons;

#endif
