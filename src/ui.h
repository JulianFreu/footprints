#ifndef UI_H
#define UI_H

#include <SDL2/SDL.h>

#include "app.h"
#include "gpx_types.h"
#include "ui_types.h"

// Animation state for the whole UI. Defined in ui.c; the event loop in main.c
// both reads and drives it.
extern UIState ui;

void clay_init(struct application *appl);
// Input and animation for this frame. Runs before the layout, and asks for
// another frame while anything is still moving.
void ui_update(struct application *appl, GpxCollection *collection);
void clay_draw_ui(struct application *appl, GpxCollection *collection);
void clay_free_memory(void);
// Shows one panel and shuts the rest; pressing the one already open shuts it
// too. The menu buttons and the TAB key both come through here.
void ui_toggle_panel(MenuPanel panel);
// The run list, which is what TAB has always toggled.
void ui_toggle_run_list(void);
// Text input for the filter fields. Which field is being edited, and what has
// been typed into it, belongs to the filter panel; the event loop routes keys
// here rather than holding any of it.
bool ui_filters_input_active(void);
// Handles one key while a field is focused. False for a key the panel has no
// use for, which the caller is then free to go on routing -- and which no
// longer throws the field out of edit mode the way anything but a digit did.
bool ui_filters_handle_key(GpxCollection *collection, SDL_Keycode key,
                           bool shift_held);
// Leaves the field. The click that caused it still counts for whatever it
// landed on, so moving between two fields is one click rather than two.
void ui_filters_blur(void);

// The same three for the settings panel, which is routed ahead of the filters
// so that only one caret is ever live. Its fields take whole words as well as
// digits -- the API key and the GPX folder are text -- so it needs the typed
// characters SDL_TEXTINPUT carries rather than only the keycodes.
bool ui_settings_input_active(void);
bool ui_settings_handle_key(SDL_Keycode key, bool shift_held);
void ui_settings_handle_text(const char *text);
// Commits whatever is in the focused field and leaves it. Committing rather
// than discarding is what makes clicking away from a field the same as
// pressing return on it.
void ui_settings_blur(void);

// Marks everything derived from the tracks stale -- the statistics series and
// the records table. Each panel rebuilds on its next update rather than in the
// middle of whatever changed the tracks, which may well be a layout pass or a
// worker's results landing. One call rather than one per panel, so a fifth
// panel does not mean a fifth line at each of these sites.
void ui_invalidate_derived(void);

void ui_load_icons(struct application *appl);
void ui_free_icons(struct application *appl);

#endif