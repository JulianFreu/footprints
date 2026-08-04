#ifndef UI_TYPES_H
#define UI_TYPES_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anim.h"
#include "config.h"

// The panels the menu bar toggles, in the order their buttons are stacked.
// One button, one panel, one anim: the enum is what ties the three together,
// and PANEL_COUNT is how many buttons the bar draws.
typedef enum MenuPanel {
    PANEL_NONE = -1,
    PANEL_RUN_LIST = 0,
    PANEL_STATISTICS,
    PANEL_RECORDS,
    PANEL_GARMIN,
    PANEL_SETTINGS,
    PANEL_COUNT
} MenuPanel;

typedef struct UIState {
    // Each runs 0 (shut) to 1 (open); the layout offsets are scaled by it.
    Anim right_sidebar;
    // Indexed by MenuPanel. Only one is ever headed for 1: opening a panel
    // sends the others back to 0.
    Anim panels[PANEL_COUNT];
    // Which panel the buttons show as active, or PANEL_NONE. Read from the
    // target of the anims would not do: a panel on its way shut is not the
    // open one.
    MenuPanel open_panel;
    Anim filters;
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
    SDL_Surface *statistics;
    SDL_Surface *records;
    SDL_Surface *settings;
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
