#include "ui_internal.h"

#include <stdio.h>

#include "background.h"
#include "colors.h"
#include "tracks.h"

// The filter panel: the seven range filters, the activity-type toggles, and
// the two buttons underneath them. Everything here is driven by the filter
// table in filters.c, so a new filter adds no code to this file.
//
// Nothing in the layout writes to the model. A press records what was pressed
// and ui_filters_update acts on it before the next layout, the same way the
// statistics panel's buttons and the run list's rows are consumed. Editing a
// field goes through filters.c, which keeps the digits, the text and the bound
// in step; this file never writes any of the three itself.

// --- State ---

// The field being edited, as the (attribute, end) pair filter_field_id packs
// into one number, or NO_FIELD. Zero is a valid packed id, so "nothing focused"
// needs a value of its own.
#define NO_FIELD (-1)
#define FILTER_FIELD_COUNT (FILTER_COUNT * BOUND_COUNT)
// The two chevrons and the name, between a row's pair of input fields.
#define FILTER_ROW_LABELS 3

static int focused_field = NO_FIELD;
// What the focused field held when it was focused, so ESC can put it back.
static char focus_undo[FILTER_DIGITS_MAX + 1];

// Set by a hover callback, consumed by the next update. -1 and false are
// "nothing pressed"; every real value is a non-negative enumerator.
static int pending_focus = NO_FIELD;
static int pending_type_toggle = -1;
static bool pending_clear = false;
static bool pending_heat = false;

// A field changed and the visible set has not caught up with it yet.
static bool filters_dirty = false;

// Seconds since the last edit, against FILTER_SETTLE_SECONDS; negative when
// there is nothing waiting. The heat tiles are the expensive half of applying
// a filter, so they follow the field going quiet rather than every keystroke
// in it -- typing "1500" rasterises the map once, not four times.
static float settle_elapsed = -1.0f;

static float caret_phase = 0.0f;

static bool caret_shown(void) {
    return caret_phase < FILTER_CARET_BLINK_SECONDS / 2.0f;
}

// --- Presses ---

static void clicked_filter_field(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_focus = (int)userData;
}

static void clicked_type_filter(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_type_toggle = (int)userData;
}

static void clicked_clear_filters(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_clear = true;
}

static void clicked_calculate_heat(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_heat = true;
}

// --- Input ---

// Moves the caret into a field, remembering what was in it. Only ever called
// with an id that came from filter_field_id.
static void focus_field(const FilterSettings *filters, int field_id) {
    FilterAttribute attribute;
    FilterBoundEnd end;
    if (!filter_field_unpack((uint16_t)field_id, &attribute, &end))
        return;

    focused_field = field_id;
    snprintf(focus_undo, sizeof(focus_undo), "%s",
             filter_bound_digits(filters, attribute, end));
    // A caret that has just arrived is a caret that is shown.
    caret_phase = 0.0f;
}

// The digit a key stands for, or -1. The keypad is what someone typing numbers
// reaches for, so it counts as much as the row above the letters.
static int key_digit(SDL_Keycode key) {
    if (key >= SDLK_0 && key <= SDLK_9)
        return (int)(key - SDLK_0);
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
        return 1 + (int)(key - SDLK_KP_1);
    if (key == SDLK_KP_0)
        return 0;
    return -1;
}

bool ui_filters_input_active(void) {
    return focused_field != NO_FIELD;
}

void ui_filters_blur(void) {
    focused_field = NO_FIELD;
}

bool ui_filters_handle_key(GpxCollection *collection, SDL_Keycode key,
                           bool shift_held) {
    FilterAttribute attribute;
    FilterBoundEnd end;
    if (focused_field == NO_FIELD ||
        !filter_field_unpack((uint16_t)focused_field, &attribute, &end))
        return false;

    FilterSettings *filters = &collection->filters;

    int typed = key_digit(key);
    if (typed >= 0) {
        filter_bound_push_digit(filters, attribute, end, (char)('0' + typed));
        filters_dirty = true;
        caret_phase = 0.0f;
        return true;
    }

    switch (key) {
    case SDLK_BACKSPACE:
        filter_bound_backspace(filters, attribute, end);
        filters_dirty = true;
        caret_phase = 0.0f;
        return true;

    case SDLK_DELETE:
        filter_bound_clear(filters, attribute, end);
        filters_dirty = true;
        caret_phase = 0.0f;
        return true;

    case SDLK_ESCAPE:
        // Back to whatever the field held when it was focused. An edit that
        // was applied as it was typed still has to be one that can be taken
        // back, or there would be no way out of a half-typed bound.
        filter_bound_set_digits(filters, attribute, end, focus_undo);
        filters_dirty = true;
        focused_field = NO_FIELD;
        return true;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        focused_field = NO_FIELD;
        return true;

    case SDLK_TAB: {
        int step = shift_held ? -1 : 1;
        focus_field(filters,
                    (focused_field + step + FILTER_FIELD_COUNT) % FILTER_FIELD_COUNT);
        return true;
    }

    default:
        // Everything else is somebody else's. A key this panel has no use for
        // used to commit the field and drop out of it, which made typing
        // anything next to a digit an exit.
        return false;
    }
}

// --- Update ---

bool ui_filters_update(struct application *appl, GpxCollection *collection) {
    // While a job is in flight the collection belongs to its worker.
    if (background_busy(&appl->background))
        return false;

    // A field cannot be focused on a panel that is on its way out: the keys
    // would be going to a box nobody can see.
    if (focused_field != NO_FIELD && anim_target(&ui.filters) < 0.5f)
        focused_field = NO_FIELD;

    bool changed = false;
    // A press has no next keystroke to wait for, so there is nothing for it to
    // be coalesced with and no reason to make it wait out the settle.
    bool immediate = false;

    if (pending_focus != NO_FIELD) {
        focus_field(&collection->filters, pending_focus);
        pending_focus = NO_FIELD;
        changed = true;
    }

    if (pending_type_toggle >= 0) {
        bool *shown = &collection->filters.show_activity[pending_type_toggle];
        *shown = !*shown;
        pending_type_toggle = -1;
        filters_dirty = true;
        immediate = true;
    }

    if (pending_clear) {
        reset_filters(&collection->filters);
        pending_clear = false;
        focused_field = NO_FIELD;
        filters_dirty = true;
        immediate = true;
    }

    if (pending_heat) {
        pending_heat = false;
        // Handed to a worker rather than run here: this used to recalculate
        // the whole heatmap inside the click handler, so the window stopped
        // responding for as long as it took. background_start_heat declines
        // while a job is already running, which is also what stops a second
        // press stacking one.
        background_start_heat(&appl->background, collection);
        changed = true;
    }

    if (filters_dirty) {
        apply_filter_values(collection);
        // Raising a flag is all this does; the statistics and the records
        // rebuild on their own next update.
        ui_invalidate_derived();
        filters_dirty = false;
        settle_elapsed = immediate ? FILTER_SETTLE_SECONDS : 0.0f;
        changed = true;
    }

    if (settle_elapsed >= 0.0f) {
        settle_elapsed += appl->delta_time;
        if (settle_elapsed >= FILTER_SETTLE_SECONDS) {
            tracks_invalidate_filtered_view(collection);
            settle_elapsed = -1.0f;
        }
        changed = true;
    }

    if (focused_field != NO_FIELD) {
        caret_phase += appl->delta_time;
        if (caret_phase >= FILTER_CARET_BLINK_SECONDS)
            caret_phase -= FILTER_CARET_BLINK_SECONDS;
        // The blink is the one thing here that moves on its own, and this is
        // what keeps the frames coming for it.
        changed = true;
    }

    return changed;
}

// --- Layout ---

static void draw_input_field(FilterAttribute attribute, FilterBoundEnd end,
                             const FilterSettings *filters) {
    uint16_t field_id = filter_field_id(attribute, end);
    bool editing = (focused_field == (int)field_id);
    const char *text = filter_bound_text(filters, attribute, end);
    bool unset = (text[0] == '\0');

    CLAY(CLAY_IDI_LOCAL("InputFieldFilter", field_id),
         {
             .border = {.color = ui_fade(accent_color), .width = editing ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(2) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)},
                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = ui_fade(Clay_Hovered() ? blue : dark_blue),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(clicked_filter_field, field_id);
        // Drawing only reads the field. The text was rewritten by the keystroke
        // that changed it, not by this pass.
        //
        // An empty bound draws a dash rather than nothing, so an open end reads
        // as open rather than as a field that failed to draw.
        ui_draw_text(unset ? "–" : text, FILTER_TEXT_FONT_SIZE,
                     unset ? bg5 : bg1, CLAY_TEXT_ALIGN_CENTER);

        if (editing && caret_shown()) {
            CLAY(CLAY_ID_LOCAL("Caret"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(FILTER_CARET_WIDTH), .height = CLAY_SIZING_FIXED(FILTER_TEXT_FONT_SIZE)}},
                  .backgroundColor = ui_fade(bg1)}) {
            }
        }
    }
}

static void draw_filter_header(void) {
    CLAY(CLAY_ID_LOCAL("Filter"),
         {
             .layout = {.padding = CLAY_PADDING_ALL(GAPS), .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = GAPS},
             .backgroundColor = ui_fade(bg1),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        CLAY(CLAY_ID_LOCAL("FilterMin"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = ui_fade(bg1),
                 .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
             }) {
            ui_draw_text("Min", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_ID_LOCAL("FilterType"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = ui_fade(bg1),
                 .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
             }) {
            ui_draw_text("Type", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_ID_LOCAL("FilterMax"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = ui_fade(bg1),
                 .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
             }) {
            ui_draw_text("Max", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
    }
}

static void draw_type_filter(ActivityType type, bool shown) {
    Clay_Color background_color = shown ? dark_green : bg5;
    Clay_Color background_color_hl = shown ? green : bg8;

    CLAY(CLAY_IDI_LOCAL("TypesFilter", type),
         {
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
             .backgroundColor = ui_fade(Clay_Hovered() ? background_color_hl : background_color),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(clicked_type_filter, (intptr_t)type);
        ui_draw_text(activity_type_label(type), LABEL_FONT_SIZE, bg, CLAY_TEXT_ALIGN_CENTER);
    }
}

static void draw_type_filter_container(const FilterSettings *filter) {
    CLAY(CLAY_ID_LOCAL("TypesFilterContainer"),
         {
             .layout = {.padding = CLAY_PADDING_ALL(GAPS), .childGap = GAPS, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        int rows = (ACTIVITY_TYPE_COUNT + FILTERS_TYPE_COLUMNS - 1) / FILTERS_TYPE_COLUMNS;
        for (int row = 0; row < rows; row++) {
            CLAY(CLAY_IDI_LOCAL("TypesFilterRow", row),
                 {.layout = {.childGap = GAPS, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                for (int column = 0; column < FILTERS_TYPE_COLUMNS; column++) {
                    int type = row * FILTERS_TYPE_COLUMNS + column;
                    if (type < ACTIVITY_TYPE_COUNT)
                        draw_type_filter((ActivityType)type, filter->show_activity[type]);
                }
            }
        }
    }
}

// One of the three boxes between a row's two input fields: the two chevrons and
// the filter's name.
//
// The index has to be unique across every row, not just within one. CLAY() takes
// its id before it opens the element, so the parent CLAY_IDI_LOCAL seeds with is
// the row's parent -- the panel -- and every row would otherwise be asking for
// the same three ids.
static void draw_filter_label(FilterAttribute attribute, int slot, const char *label) {
    CLAY(CLAY_IDI_LOCAL("FilterLabel", (uint16_t)(attribute * FILTER_ROW_LABELS + slot)),
         {
             .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
             .backgroundColor = ui_fade(bg1),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        ui_draw_text(label, LABEL_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
    }
}

static void draw_filter(FilterAttribute attribute, const FilterSettings *filters) {
    CLAY(CLAY_IDI_LOCAL("Filter", attribute),
         {
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = GAPS},
             .backgroundColor = ui_fade(bg1),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        draw_input_field(attribute, BOUND_LOW, filters);
        draw_filter_label(attribute, 0, "<");
        draw_filter_label(attribute, 1, filter_display_name(attribute));
        draw_filter_label(attribute, 2, "<");
        draw_input_field(attribute, BOUND_HIGH, filters);
    }
}

static void draw_filter_button(uint16_t slot, const char *label,
                               void (*on_click)(Clay_ElementId, Clay_PointerData, intptr_t)) {
    CLAY(CLAY_IDI_LOCAL("FilterButton", slot),
         {
             .layout = {.padding = CLAY_PADDING_ALL(GAPS), .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(ELEMENTS_HEIGHT)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
             .backgroundColor = ui_fade(Clay_Hovered() ? bg_l : bg_d),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(on_click, 0);
        ui_draw_text(label, LABEL_FONT_SIZE, dark_aqua, CLAY_TEXT_ALIGN_CENTER);
    }
}

void ui_draw_filter_panel(struct application *appl, GpxCollection *collection) {
    // Decided before the CLAY() block rather than inside it: CLAY() expands to
    // a for loop, and returning out of the middle of one leaves the layout
    // unclosed.
    if (anim_value(&ui.filters) <= 0.0f)
        return;

    const FilterSettings *filters = &collection->filters;
    // Measured off the panel the filters are a wing of: shut, they hide behind
    // its right end; open, they rest one gap clear of it.
    int host_w = ui_panel_width(appl, ui.filters_host);

    CLAY(CLAY_ID("FilterOptions"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {
                  .x = ui_panel_offset_x(ui.filters_host, host_w) + host_w - (FILTERS_WIDTH + GAPS) + anim_value(&ui.filters) * (FILTERS_WIDTH + 2 * GAPS),
                  .y = PANEL_ORIGIN_Y},
          },
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
          .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .padding = CLAY_PADDING_ALL(GAPS), .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_WIDTH), .height = CLAY_SIZING_FIXED(PANEL_HEIGHT(appl->window_height))}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = GAPS},
          .backgroundColor = ui_fade(bg)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_filter_header();
        for (int attribute = 0; attribute < FILTER_COUNT; attribute++)
            draw_filter((FilterAttribute)attribute, filters);
        draw_type_filter_container(filters);

        // Everything above sits at the top of the panel and everything below
        // at the bottom; this is the room between them.
        CLAY(CLAY_ID_LOCAL("FilterSpacer"),
             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}}) {
        }

        CLAY(CLAY_ID_LOCAL("FilterTracksState"),
             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
            ui_draw_text(collection->total_visible_tracks_str, LABEL_FONT_SIZE, fg, CLAY_TEXT_ALIGN_CENTER);
        }

        // The two things the panel does that are not a filter. Everything the
        // filters themselves do now happens as they are typed, so there is no
        // third button applying them.
        draw_filter_button(0, "Clear All Filters", clicked_clear_filters);
        draw_filter_button(1, "Calculate Heat", clicked_calculate_heat);
    }
}
