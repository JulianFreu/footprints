#ifndef UI_TYPES_H
#define UI_TYPES_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anim.h"
#include "config.h"
#include "track_series.h"

// The panels the menu bar toggles, in the order their buttons are stacked.
// One button, one panel, one anim: the enum is what ties the three together,
// and PANEL_COUNT is how many buttons the bar draws.
typedef enum MenuPanel {
    PANEL_NONE = -1,
    PANEL_RUN_LIST = 0,
    PANEL_STATISTICS,
    PANEL_RECORDS,
    PANEL_IMPORT,
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
    // Which panel the filters are a wing of. Not derived from the anims: a
    // panel on its way shut is still the one they are sliding away from.
    MenuPanel filters_host;
} UIState;

typedef struct
{
    uint32_t font_id;
    TTF_Font *font;
} SDL2_Font;

// One font per size anything is drawn at. SDL2_ttf drops a font's whole glyph
// cache whenever its size changes, and a single sidebar row draws a value at one
// size next to its unit at another -- so one font resized per element rebuilt
// every glyph from its outline on every draw. A font apiece means the size is
// never changed and the glyphs are kept.
//
// The sizes themselves live in ui.c, next to the layout constants they come
// from. This is only how many there are and what each is for -- which is what
// sizes struct application's font array, and what lets tracks.c ask for the one
// it rasterises the sidebar's graph labels with rather than borrowing a font the
// UI is drawing text at.
typedef enum UiFont {
    UI_FONT_SMALL = 0,
    UI_FONT_LABEL,
    UI_FONT_HEADING,
    UI_FONT_GRAPH_LABEL,
    UI_FONT_COUNT
} UiFont;

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
    // One picture per graph in the sidebar, indexed by TrackSeriesKind and
    // NULL where the selected track carries nothing of that kind. Regenerated
    // only when the selection changes, not per frame.
    SDL_Surface *graphs[TRACK_SERIES_COUNT];
} UiIcons;

#endif
