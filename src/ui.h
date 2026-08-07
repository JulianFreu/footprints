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

// And again for the import panel, which is routed after the settings one.
// Every box in it takes whole words -- an address, a password, a code, an
// application's id and secret -- so it too works from the characters
// SDL_TEXTINPUT carries rather than the keycodes.
bool ui_import_input_active(void);
bool ui_import_handle_key(SDL_Keycode key, bool shift_held);
void ui_import_handle_text(const char *text);
void ui_import_blur(void);

// Marks everything derived from the tracks stale -- the statistics series and
// the records table. Each panel rebuilds on its next update rather than in the
// middle of whatever changed the tracks, which may well be a layout pass or a
// worker's results landing. One call rather than one per panel, so a fifth
// panel does not mean a fifth line at each of these sites.
void ui_invalidate_derived(void);

// Which point of the selected track the sidebar's graphs are being hovered
// over, if any. The frame loop asks so it can put a dot there on the map; where
// the pointer is belongs to the panel that was hit-tested, not to the map.
bool ui_graph_hover_point(int *point_index);

// The pixel size the sidebar draws each of its graphs at, for the window as it
// is now. The height depends on how much room the attribute rows leave, so it
// moves with the window; the pictures are rasterised at exactly this so they
// land on the screen at their own scale rather than being scaled into their
// box. The arithmetic behind it is the layout's, which is why the code that
// draws the graphs asks rather than working it out a second time. Either
// pointer may be null, for a caller that only wants the other.
void ui_sidebar_graph_size(const struct application *appl, int *width, int *height);

void ui_load_icons(struct application *appl);
void ui_free_icons(struct application *appl);

// The frame-time graph, drawn straight to the renderer after Clay's own pass:
// it is an overlay rather than a panel, so it sits on top of whatever is open
// instead of competing with it for room. Draws nothing when the setting is off.
void ui_profiler_draw(struct application *appl);
// Drops the legend texture the overlay caches. Safe to call having never drawn.
void ui_profiler_free(struct application *appl);

#endif