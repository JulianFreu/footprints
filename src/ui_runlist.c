#include "ui_internal.h"

#include <stdlib.h>

#include "colors.h"
#include "track_format.h"
#include "track_sort.h"
#include "tracks.h"

// The run list: its sortable header, its rows, and the virtualised scroll
// container that emits only the rows actually on screen.

// Set by a row click and consumed by the next layout pass, which is when the
// selection and the map centre can both be updated.
static bool row_clicked = false;
static int clicked_track_id = -1;

bool ui_runlist_take_click(int *track_id) {
    if (!row_clicked)
        return false;
    row_clicked = false;
    *track_id = clicked_track_id;
    return true;
}

int ui_runlist_selected_row(void) {
    return clicked_track_id;
}

static void clicked_toggle_filter_view(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        ui_panel_toggle(&ui.filters);
}

static void clicked_run_entry(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        row_clicked = true;
        clicked_track_id = (int)userData;
    }
}

static void clicked_list_headers(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        GpxCollection *collection = (GpxCollection *)userData;
        track_sort_by(collection, collection->to_be_sorted_by);
    }
}

static void draw_run_list_header_attribute(GpxCollection *collection, int width, const char *str, AttributeType sort_type) {
    CLAY(CLAY_IDI_LOCAL("RunListHeaderAttribute", sort_type), {.layout = {.sizing = {.width = width, .height = CLAY_SIZING_GROW(0)},
                                                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                                                                          .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                                               .backgroundColor = Clay_Hovered() ? accent_color_hl : accent_color,
                                                               .cornerRadius = CORNER_RADIUS}) {
        if (Clay_Hovered()) {
            collection->to_be_sorted_by = sort_type;
        }
        Clay_OnHover(clicked_list_headers, (intptr_t)collection);
        ui_draw_text(str, LABEL_FONT_SIZE, bg_d, CLAY_TEXT_ALIGN_CENTER);
        switch (sort_type) {
        case DATE:
            ui_draw_text("[dd:mm:yyyy]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case DISTANCE:
            ui_draw_text("[km]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case PACE:
            ui_draw_text("[min/km]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case DURATION:
            ui_draw_text("[hh:mm:ss]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case UPHILL:
            ui_draw_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case DOWNHILL:
            ui_draw_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case HIGHPOINT:
            ui_draw_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        default:
            break;
        }
    }
}

static void draw_run_list_bottom(const char *total_visible_tracks, GpxCollection *collection) {
    CLAY(CLAY_ID("RunListBottom"), {.layout = {
                                        .padding = CLAY_PADDING_ALL(GAPS),
                                        .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIT()},
                                        .childGap = GAPS,
                                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                    .backgroundColor = bg6,
                                    .cornerRadius = {.bottomLeft = CORNER_RADIUS, .bottomRight = CORNER_RADIUS}}) {
        CLAY(CLAY_ID("RunListBottomSPACE"), {.layout = {
                                                 .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()}}}) {
        }
        CLAY(CLAY_ID("FilterOptionsButton"), {.layout = {
                                                  .padding = CLAY_PADDING_ALL(GAPS),
                                                  .sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIT()},
                                                  .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                                  .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                              .backgroundColor = Clay_Hovered() ? bg_l : bg_d,
                                              .cornerRadius = CORNER_RADIUS}) {
            Clay_OnHover(clicked_toggle_filter_view, 0);
            ui_draw_text("Toggle Filter View", 16, dark_aqua, CLAY_TEXT_ALIGN_CENTER);
        }
    }
}

static void draw_run_list_header(GpxCollection *collection) {
    CLAY(CLAY_ID("RunListHeader"), {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                                               .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)},
                                               .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                    .backgroundColor = accent_color,
                                    .cornerRadius = {
                                        .topLeft = CORNER_RADIUS,
                                        .topRight = CORNER_RADIUS,
                                        .bottomLeft = 0,
                                        .bottomRight = 0}}) {
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Type", TYPE);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Date", DATE);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Distance", DISTANCE);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Pace", PACE);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Duration", DURATION);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Uphill", UPHILL);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Downhill", DOWNHILL);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Highest", HIGHPOINT);
    }
}

static void draw_run_entry_attribute(int width, const char *str, int id) {
    CLAY(CLAY_IDI_LOCAL("RunEntryAttribute", id),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
        ui_draw_text(str, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

static void draw_run_list_entry(GpxTrack *track) {
    CLAY(CLAY_IDI_LOCAL("RunListEntry", track->track_id),
         {
             .border = {.color = border, .width = (clicked_track_id == track->track_id) ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(2) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = Clay_Hovered() ? bg_l : bg_d,
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(clicked_run_entry, track->track_id);
        draw_run_entry_attribute(RUN_LIST_COLUMN_WIDTH, activity_type_label(track->act_type), track->track_id * RUN_LIST_COLUMN_COUNT + 0);

        draw_run_entry_attribute(RUN_LIST_COLUMN_WIDTH, ui_track_text(track, TRACK_TEXT_DATE), track->track_id * RUN_LIST_COLUMN_COUNT + 1);
        draw_run_entry_attribute(RUN_LIST_COLUMN_WIDTH, ui_track_text(track, TRACK_TEXT_DISTANCE), track->track_id * RUN_LIST_COLUMN_COUNT + 2);
        draw_run_entry_attribute(RUN_LIST_COLUMN_WIDTH, ui_track_text(track, TRACK_TEXT_PACE), track->track_id * RUN_LIST_COLUMN_COUNT + 3);
        draw_run_entry_attribute(RUN_LIST_COLUMN_WIDTH, ui_track_text(track, TRACK_TEXT_DURATION), track->track_id * RUN_LIST_COLUMN_COUNT + 4);
        draw_run_entry_attribute(RUN_LIST_COLUMN_WIDTH, ui_track_text(track, TRACK_TEXT_ELEV_UP), track->track_id * RUN_LIST_COLUMN_COUNT + 5);
        draw_run_entry_attribute(RUN_LIST_COLUMN_WIDTH, ui_track_text(track, TRACK_TEXT_ELEV_DOWN), track->track_id * RUN_LIST_COLUMN_COUNT + 6);
        draw_run_entry_attribute(RUN_LIST_COLUMN_WIDTH, ui_track_text(track, TRACK_TEXT_HIGH_POINT), track->track_id * RUN_LIST_COLUMN_COUNT + 7);
    }
}

// Track ids of the rows that pass the filters, in display order. Rebuilt each
// frame -- it is one comparison per track, against emitting a Clay element per
// track -- and reused so the common case allocates nothing.
static int *visible_rows = NULL;
static int visible_rows_capacity = 0;

static int collect_visible_rows(const GpxCollection *collection) {
    if (collection->total_tracks > visible_rows_capacity) {
        int *grown = realloc(visible_rows, (size_t)collection->total_tracks * sizeof(int));
        if (!grown)
            return 0;
        visible_rows = grown;
        visible_rows_capacity = collection->total_tracks;
    }

    int count = 0;
    for (int i = 0; i < collection->total_tracks; i++) {
        int track_id = collection->list_order[i];
        if (collection->tracks[track_id].visible_in_list)
            visible_rows[count++] = track_id;
    }
    return count;
}

// Stands in for the rows scrolled past, so the scrollbar and the content height
// stay the same as if every row had been emitted. The container puts a childGap
// after the spacer, which is part of the pitch being replaced.
static void draw_run_list_spacer(int rows, int row_pitch, int id) {
    if (rows <= 0)
        return;
    CLAY(CLAY_IDI_LOCAL("RunListSpacer", id),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(rows * row_pitch - GAPS)}}}) {
    }
}

// Only the rows actually on screen are handed to Clay. Every emitted row costs
// eight text elements, and the renderer rasterises and uploads each one every
// frame, so a library of several hundred tracks was paying for thousands of
// glyph rasterisations per frame to draw the twenty-odd rows that are visible.
static void draw_run_list_scroll_container(GpxCollection *collection, int height) {
    const int row_pitch = LIST_ENTRY_HEIGHT + GAPS;
    int visible_count = collect_visible_rows(collection);

    CLAY(CLAY_ID("RunListScrollContainer"),
         {
             .layout = {
                 .padding = CLAY_PADDING_ALL(GAPS),
                 .childGap = GAPS,
                 .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(height)},
                 .layoutDirection = CLAY_TOP_TO_BOTTOM},
             .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()},
             .backgroundColor = bg,
         }) {
        // Must be read with the container open -- Clay resolves the offset
        // against the currently open element.
        float scroll_y = Clay_GetScrollOffset().y;

        int first_row = (int)(-scroll_y / row_pitch);
        if (first_row < 0)
            first_row = 0;
        if (first_row > visible_count)
            first_row = visible_count;

        // One extra row at each end so a partially scrolled row is still drawn.
        int last_row = first_row + height / row_pitch + 2;
        if (last_row > visible_count)
            last_row = visible_count;

        draw_run_list_spacer(first_row, row_pitch, 0);
        for (int row = first_row; row < last_row; row++)
            draw_run_list_entry(&collection->tracks[visible_rows[row]]);
        draw_run_list_spacer(visible_count - last_row, row_pitch, 1);
    }
}

// Releases the row buffer kept between frames.
void ui_runlist_free_scratch(void) {
    free(visible_rows);
    visible_rows = NULL;
    visible_rows_capacity = 0;
}

void ui_draw_run_list(struct application *appl, GpxCollection *collection,
                      int list_offset_y) {
    CLAY(CLAY_ID("RunsListMenu"),
         {
             .floating = {
                 .attachTo = CLAY_ATTACH_TO_ROOT,
                 .offset = {
                     .x = -RUN_LIST_WIDTH + anim_value(&ui.run_list) * (SCREEN_BORDER_PADDING + RUN_LIST_WIDTH),
                     .y = list_offset_y},
             },
             .layout = {.sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIXED(appl->window_height - 2 * SCREEN_BORDER_PADDING - GAPS - MENU_ICON_SIZE)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
         }) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_run_list_header(collection);
        draw_run_list_scroll_container(collection, appl->window_height - (MENU_ICON_SIZE + 2 * GAPS) - (LIST_ENTRY_HEIGHT + 2 * GAPS) - (LIST_ENTRY_HEIGHT + 2 * GAPS) - (2 * SCREEN_BORDER_PADDING));
        draw_run_list_bottom(collection->total_visible_tracks_str, collection);
    }
}
