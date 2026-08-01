#ifndef UI_H
#define UI_H

#include <SDL2/SDL.h>

#include "app.h"
#include "gpx_types.h"
#include "ui_types.h"

// Animation and text-input state for the whole UI. Defined in ui.c; the event
// loop in main.c both reads and drives it.
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
// Text input for the filter fields. The event loop routes keystrokes here
// rather than reaching into UIState itself.
bool ui_text_input_active(void);
void ui_text_input_digit(GpxCollection *collection, char digit);
void ui_text_input_finish(GpxCollection *collection);

// Marks the statistics series stale. The panel rebuilds it on the next update
// rather than in the middle of whatever changed the tracks, which may well be a
// layout pass or a worker's results landing.
void ui_stats_invalidate(void);

void ui_load_icons(struct application *appl);
void ui_free_icons(struct application *appl);

#endif