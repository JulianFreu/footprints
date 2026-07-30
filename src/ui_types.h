#ifndef UI_TYPES_H
#define UI_TYPES_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Filter identifiers. The low bits select which attribute a filter edits; the
// two high bits select which end of its range, so a field is addressed as
// e.g. FILTER_DISTANCE | HIGH_LIMIT.
#define FILTER_DISTANCE 0b0000000000000001
#define FILTER_DATE 0b0000000000000010
#define FILTER_DURATION 0b0000000000000100
#define FILTER_UPHILL 0b0000000000001000
#define FILTER_DOWNHILL 0b0000000000010000
#define FILTER_PEAK 0b0000000000100000
#define FILTER_PACE 0b0000000001000000
#define HIGH_LIMIT 0b1000000000000000
#define LOW_LIMIT 0b0100000000000000

typedef struct AnimationState {
    bool opening;
    bool closing;
    float animation;
    int ticks;
} AnimationState;

typedef struct UIState {
    AnimationState right_sidebar;
    AnimationState run_list;
    AnimationState filters_animation;
    bool text_input_mode;
    char text_input_buffer[INPUT_BUFFER_SIZE];
    size_t text_input_length;
    uint16_t activeFilterID;
} UIState;

typedef struct
{
    uint32_t fontId;
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
