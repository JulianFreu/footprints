#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "app.h"
#include "clay.h"
#include "filters.h"
#include "gpx_types.h"
#include "ui.h"

// Shared between the UI translation units. Nothing outside src/ui*.c should
// include this: it is the vocabulary the panels are laid out in, not part of
// the module's interface.

// --- Layout ---
#define SCREEN_BORDER_PADDING 5
#define CORNER_RADIUS 8
#define GAPS 5
#define SIDEBAR_WIDTH 250
#define MENU_BAR_WIDTH 250
#define MENU_ICON_SIZE 32 + 2 * GAPS

#define ELEMENTS_HEIGHT 30
#define ELEMENTS_WIDTH 180
#define LIST_ENTRY_HEIGHT 30
#define HEADER_HEIGHT 50
#define FILTERS_WIDTH 300
#define FILTERS_MINMAX_WIDTH 80

// Every run-list column is the same width; the list is as wide as the sum.
#define RUN_LIST_COLUMN_WIDTH 100
#define RUN_LIST_COLUMN_COUNT 8
#define RUN_LIST_WIDTH (RUN_LIST_COLUMN_COUNT * RUN_LIST_COLUMN_WIDTH + 2 * GAPS)

// --- Text ---
#define FILTER_TEXT_FONT_SIZE 12
#define LABEL_FONT_SIZE 16
#define HEADING_FONT_SIZE 20

// Animation and text-input state for the whole UI. Defined in ui.c.
extern UIState ui;

// Emits a Clay text element for a null-terminated string.
void ui_draw_text(const char *string, uint16_t font_size, Clay_Color color,
                  Clay_TextAlignment align);

// Starts a panel opening or closing, whichever reverses what it is doing now.
void ui_animation_toggle(struct AnimationState *anim_obj);

// The two side panels, each drawn by its own translation unit.
void ui_draw_filter_panel(struct application *appl, GpxCollection *collection,
                          int list_offset_y);
void ui_draw_run_list(struct application *appl, GpxCollection *collection,
                      int list_offset_y);
// Releases the run list's between-frames row buffer.
void ui_runlist_free_scratch(void);
// A row click, consumed once by the next layout pass.
bool ui_runlist_take_click(int *track_id);
// The row drawn with a selection border, or -1.
int ui_runlist_selected_row(void);

#endif
