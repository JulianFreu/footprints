#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "app.h"
#include "clay.h"
#include "filters.h"
#include "gpx_types.h"
#include "track_format.h"
#include "ui.h"

// Shared between the UI translation units. Nothing outside src/ui*.c should
// include this: it is the vocabulary the panels are laid out in, not part of
// the module's interface.

// --- Layout ---
#define SCREEN_BORDER_PADDING 5
#define CORNER_RADIUS 8
#define GAPS 5
#define SIDEBAR_WIDTH 250
#define MENU_ICON_SIZE (32 + 2 * GAPS)
// The menu bar is a column of square buttons down the left edge, so it is one
// button wide. It used to be a 250px row, most of it empty -- and that empty
// part still swallowed the clicks meant for the map behind it.
#define MENU_BAR_WIDTH MENU_ICON_SIZE

// Every panel hangs off the same corner: clear of the bar beside it, and the
// screen border away from the top. Their heights are all measured from there.
#define PANEL_ORIGIN_X (SCREEN_BORDER_PADDING + MENU_BAR_WIDTH + GAPS)
#define PANEL_ORIGIN_Y SCREEN_BORDER_PADDING
#define PANEL_HEIGHT(window_height) ((window_height) - 2 * SCREEN_BORDER_PADDING)
// The panels that are still empty. The run list has a width of its own, being
// the sum of its columns.
#define PANEL_WIDTH 400

#define ELEMENTS_HEIGHT 30
#define ELEMENTS_WIDTH 180
#define LIST_ENTRY_HEIGHT 30
#define HEADER_HEIGHT 50
// Fixed rather than fitted to the button inside it, so the panel's height is
// the sum of three known numbers rather than something to be measured.
#define RUN_LIST_FOOTER_HEIGHT (LIST_ENTRY_HEIGHT + 2 * GAPS)
#define FILTERS_WIDTH 300
#define FILTERS_MINMAX_WIDTH 80

// The scroll position indicator, in its own column to the right of the rows so
// it never sits on top of one.
#define RUN_LIST_SCROLLBAR_WIDTH 8
#define RUN_LIST_SCROLLBAR_MIN_THUMB 24
#define RUN_LIST_GUTTER_WIDTH (RUN_LIST_SCROLLBAR_WIDTH + 2 * GAPS)

// Every run-list column is the same width; the list is as wide as the sum, plus
// the gutter the indicator lives in.
#define RUN_LIST_COLUMN_WIDTH 100
#define RUN_LIST_COLUMN_COUNT 8
#define RUN_LIST_WIDTH \
    (RUN_LIST_COLUMN_COUNT * RUN_LIST_COLUMN_WIDTH + 2 * GAPS + RUN_LIST_GUTTER_WIDTH)

// --- Text ---
#define FILTER_TEXT_FONT_SIZE 12
#define LABEL_FONT_SIZE 16
#define HEADING_FONT_SIZE 20

// Animation and text-input state for the whole UI. Defined in ui.c.
extern UIState ui;

// One track attribute, formatted into this frame's text arena. The result
// stays valid until the end of the frame, which is what Clay needs since it
// keeps the pointer rather than copying the characters.
const char *ui_track_text(const GpxTrack *track, TrackText field);

// Formatted text that lives until the end of the frame. Clay keeps the pointer
// it is given rather than copying, so a local buffer would be read after its
// frame had gone.
const char *ui_frame_printf(const char *fmt, ...);

// Emits a Clay text element for a null-terminated string.
void ui_draw_text(const char *string, uint16_t font_size, Clay_Color color,
                  Clay_TextAlignment align);

// Slides a panel to open (1) or shut (0), and the reverse of whatever it is
// doing now.
void ui_panel_move(Anim *panel, float target);
void ui_panel_toggle(Anim *panel);

// Where a panel `width` wide sits this frame: off the left edge when shut, at
// PANEL_ORIGIN_X when open, and proportionally between the two while it slides.
float ui_panel_offset_x(MenuPanel panel, int width);

// The opacity everything drawn from here on is scaled by, and the scaling
// itself. Set it to a panel's anim value before drawing the panel and back to
// 1 afterwards; every colour a panel draws with goes through ui_fade, which is
// what makes it fade in as it slides. Text is already covered by ui_draw_text.
void ui_fade_set(float alpha);
Clay_Color ui_fade(Clay_Color color);

// The panels, each drawn by its own translation unit.
void ui_draw_filter_panel(struct application *appl, GpxCollection *collection);
void ui_draw_run_list(struct application *appl, GpxCollection *collection);
// An empty panel with a titled header: what statistics, records and settings
// are until they have something to show.
void ui_draw_simple_panel(struct application *appl, MenuPanel panel,
                          const char *title);
// Releases the run list's between-frames row buffer.
void ui_runlist_free_scratch(void);
// A row click, consumed once by the next layout pass.
bool ui_runlist_take_click(int *track_id);
// Moves the run list by whole mouse-wheel detents, if the pointer is over it.
// Returns whether the wheel was the list's to take.
bool ui_runlist_scroll_by_wheel(int mouse_x, int mouse_y, int detents);
// Eases the drawn offset toward where the wheel has put it. Returns whether it
// moved, which is what keeps the frames coming while it settles.
bool ui_runlist_scroll_tick(float dt);
// The row drawn with a selection border, or -1.
int ui_runlist_selected_row(void);

#endif
