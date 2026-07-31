#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "colors.h"
#include "background.h"
#include "log.h"
#include "tracks.h"

// The filter panel: the seven range filters, the activity-type toggles, and
// the two buttons underneath them. Everything here is driven by the filter
// table in filters.c, so a new filter adds no code to this file.

static void clicked_type_filter(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        bool *show_type = (bool *)userData;
        if (*show_type == true)
            *show_type = false;
        else
            *show_type = true;
    }
}

static void init_numbers_input(void) {
    // clear buffer
    for (int i = 0; i < INPUT_BUFFER_SIZE; i++) {
        ui.text_input_buffer[i] = '\0';
    }
    ui.text_input_mode = true;
    ui.text_input_length = 0;
}

static void clicked_filter_field(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        init_numbers_input();
        ui.active_filter_id = (int)userData;
        LOG_DEBUG("start number input\n");
    }
}

static void clear_char_array(char *str, int size) {
    for (int i = 0; i < size; i++)
        str[i] = '\0';
}

static void format_pace_filter_str(char *str) {
    if (ui.text_input_length < 7) {
        clear_char_array(str, FILTER_TEXT_SIZE);
        snprintf(str, FILTER_TEXT_SIZE, "%s", ui.text_input_buffer);
        if (ui.text_input_length > 2) {
            str[ui.text_input_length] = str[ui.text_input_length - 1];
            str[ui.text_input_length - 1] = str[ui.text_input_length - 2];
            str[ui.text_input_length - 2] = ':';
        }
    }
}

static void format_date_filter_str(char *str) {
    if (ui.text_input_length < 9) {
        clear_char_array(str, FILTER_TEXT_SIZE);
        str[0] = ui.text_input_buffer[0];
        str[1] = ui.text_input_buffer[1];
        str[2] = '.';
        str[3] = ui.text_input_buffer[2];
        str[4] = ui.text_input_buffer[3];
        str[5] = '.';
        str[6] = ui.text_input_buffer[4];
        str[7] = ui.text_input_buffer[5];
        str[8] = ui.text_input_buffer[6];
        str[9] = ui.text_input_buffer[7];
    }
}

static void format_duration_filter_str(char *str) {
    if (ui.text_input_length < 8) {
        clear_char_array(str, FILTER_TEXT_SIZE);
        snprintf(str, FILTER_TEXT_SIZE, "%s", ui.text_input_buffer);
        if (ui.text_input_length > 2) {
            str[ui.text_input_length] = str[ui.text_input_length - 1];
            str[ui.text_input_length - 1] = str[ui.text_input_length - 2];
            str[ui.text_input_length - 2] = ':';
        }
        if (ui.text_input_length > 4) {
            str[ui.text_input_length + 1] = str[ui.text_input_length];
            str[ui.text_input_length] = str[ui.text_input_length - 1];
            str[ui.text_input_length - 1] = str[ui.text_input_length - 2];
            str[ui.text_input_length - 2] = str[ui.text_input_length - 3];
            str[ui.text_input_length - 3] = str[ui.text_input_length - 4];
            str[ui.text_input_length - 4] = ':';
        }
    }
}

static void format_elev_filter_str(char *str) {
    if (ui.text_input_length < 6) {
        clear_char_array(str, FILTER_TEXT_SIZE);
        snprintf(str, FILTER_TEXT_SIZE, "%s", ui.text_input_buffer);
    }
}

static void format_distance_filter_str(char *str) {
    if (ui.text_input_length == 0) {
        clear_char_array(str, FILTER_TEXT_SIZE);
    } else if (ui.text_input_length == 1) {
        clear_char_array(str, FILTER_TEXT_SIZE);
        str[0] = '0';
        str[1] = '.';
        str[2] = '0';
        str[3] = ui.text_input_buffer[0];
    } else if (ui.text_input_length == 2) {
        clear_char_array(str, FILTER_TEXT_SIZE);
        str[0] = '0';
        str[1] = '.';
        str[2] = ui.text_input_buffer[0];
        str[3] = ui.text_input_buffer[1];
    } else if (ui.text_input_length < 7) {
        clear_char_array(str, FILTER_TEXT_SIZE);
        snprintf(str, FILTER_TEXT_SIZE, "%s", ui.text_input_buffer);
        str[ui.text_input_length - 2] = '.';
        str[ui.text_input_length - 1] = ui.text_input_buffer[ui.text_input_length - 2];
        str[ui.text_input_length] = ui.text_input_buffer[ui.text_input_length - 1];
    }
}

// Lays typed digits out into the field's display form. Which layout applies
// comes from the attribute's FilterFormat, so a new filter reusing an existing
// shape needs nothing here.
static void format_filter_input(char *str, FilterFormat format) {
    switch (format) {
    case FILTER_FORMAT_DATE:
        format_date_filter_str(str);
        break;
    case FILTER_FORMAT_DISTANCE:
        format_distance_filter_str(str);
        break;
    case FILTER_FORMAT_DURATION:
        format_duration_filter_str(str);
        break;
    case FILTER_FORMAT_PACE:
        format_pace_filter_str(str);
        break;
    case FILTER_FORMAT_ELEVATION:
        format_elev_filter_str(str);
        break;
    }
}

static void draw_input_field(FilterAttribute attribute, FilterBoundEnd end,
                             FilterSettings *filters) {
    uint16_t field_id = filter_field_id(attribute, end);
    bool editing = ui.text_input_mode && ui.active_filter_id == (int)field_id;

    CLAY(CLAY_IDI_LOCAL("InputFieldFilter", field_id),
         {
             .border = {.color = border, .width = editing ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(3) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)},
                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = Clay_Hovered() ? blue : dark_blue,
             .cornerRadius = CORNER_RADIUS,
         }) {
        Clay_OnHover(clicked_filter_field, field_id);
        // Drawing only reads the field; the text is reformatted on the
        // keystroke that changes it, not from the layout pass.
        ui_draw_text(filter_bound_text(filters, attribute, end), FILTER_TEXT_FONT_SIZE, bg1, CLAY_TEXT_ALIGN_CENTER);
    }
}

static void draw_filter_header() {
    CLAY(CLAY_ID_LOCAL("Filter"),
         {
             .layout = {.padding = CLAY_PADDING_ALL(GAPS), .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = GAPS},
             .backgroundColor = bg1,
             .cornerRadius = CORNER_RADIUS,
         }) {
        CLAY(CLAY_ID_LOCAL("FilterMin"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            ui_draw_text("Min", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_ID_LOCAL("FilterType"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            ui_draw_text("Type", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_ID_LOCAL("FilterMax"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            ui_draw_text("Max", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
    }
}

static void draw_type_filter(ActivityType type, bool *show_type) {
    Clay_Color background_color;
    Clay_Color background_color_hl;
    if (*show_type == true) {
        background_color = dark_green;
        background_color_hl = green;
    } else {
        background_color = bg5;
        background_color_hl = bg8;
    }

    CLAY(CLAY_IDI_LOCAL("TypesFilter", type),
         {
             .layout = {.sizing = {.width = CLAY_SIZING_FIXED(FILTERS_WIDTH / 2), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
             .backgroundColor = Clay_Hovered() ? background_color_hl : background_color,
             .cornerRadius = CORNER_RADIUS,
         }) {
        Clay_OnHover(clicked_type_filter, (intptr_t)show_type);
        ui_draw_text(activity_type_label(type), LABEL_FONT_SIZE, bg, CLAY_TEXT_ALIGN_CENTER);
    }
}

static void draw_type_filter_container(FilterSettings *filter) {
    CLAY(CLAY_ID_LOCAL("TypesFilterContainer"),
         {
             .layout = {.padding = CLAY_PADDING_ALL(3 * GAPS), .childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_WIDTH / 2), .height = CLAY_SIZING_FIT(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
             .cornerRadius = CORNER_RADIUS,
         }) {
        for (int type = 0; type < ACTIVITY_TYPE_COUNT; type++)
            draw_type_filter((ActivityType)type, &filter->show_activity[type]);
    }
}

static void draw_filter(FilterAttribute attribute, FilterSettings *filters) {
    CLAY(CLAY_IDI_LOCAL("Filter", attribute),
         {
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = GAPS},
             .backgroundColor = bg1,
             .cornerRadius = CORNER_RADIUS,
         }) {
        draw_input_field(attribute, BOUND_LOW, filters);
        CLAY(CLAY_IDI_LOCAL("lesser", attribute),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            ui_draw_text("<", LABEL_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_IDI_LOCAL("FilterName", attribute),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            ui_draw_text(filter_display_name(attribute), LABEL_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_IDI_LOCAL("greater", attribute),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            ui_draw_text("<", LABEL_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        draw_input_field(attribute, BOUND_HIGH, filters);
    }
}

static void clicked_calculate_heat(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state != CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        return;

    struct application *appl = (struct application *)userData;

    // Handed to a worker rather than run here: this used to recalculate the
    // whole heatmap inside the click handler, so the window stopped responding
    // for as long as it took. background_start_heat declines while a job is
    // already running, which is also what stops a second click stacking one.
    background_start_heat(&appl->background, appl->collection);
}
static void clicked_show_filtered_tracks(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        GpxCollection *collection = (GpxCollection *)userData;
        tracks_invalidate_cache(collection);
    }
}

void ui_draw_filter_panel(struct application *appl, GpxCollection *collection,
                          int list_offset_y) {
    if (ui.filters_animation.animation != 0) {
        CLAY(CLAY_ID("FilterOptions"),
             {.floating = {
                  .attachTo = CLAY_ATTACH_TO_ROOT,
                  .offset = {
                      .x = ui.run_list.animation * (SCREEN_BORDER_PADDING + RUN_LIST_WIDTH) - (FILTERS_WIDTH + GAPS) + ui.filters_animation.animation * (FILTERS_WIDTH + 2 * GAPS),
                      .y = list_offset_y},
              },
              .cornerRadius = CORNER_RADIUS,
              .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .padding = CLAY_PADDING_ALL(GAPS), .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_WIDTH), .height = CLAY_SIZING_FIXED(appl->window_height - 3 * GAPS - MENU_ICON_SIZE - 25)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = GAPS},
              .backgroundColor = bg}) {
            if (Clay_Hovered())
                appl->mouse_over_ui = true;
            draw_filter_header();
            for (int attribute = 0; attribute < FILTER_COUNT; attribute++)
                draw_filter((FilterAttribute)attribute, &collection->filters);

            bool shown_before[ACTIVITY_TYPE_COUNT];
            memcpy(shown_before, collection->filters.show_activity, sizeof(shown_before));
            draw_type_filter_container(&collection->filters);

            CLAY(CLAY_ID_LOCAL("FilterTracksState"), {.layout = {
                                                          .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW()},
                                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
                ui_draw_text(collection->total_visible_tracks_str, LABEL_FONT_SIZE, fg, CLAY_TEXT_ALIGN_CENTER);
            }
            CLAY(CLAY_ID_LOCAL("space"), {.layout = {
                                              .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW()}}}) {
            }
            CLAY(CLAY_ID("Calculate Heat"), {.layout = {
                                                 .padding = CLAY_PADDING_ALL(GAPS),
                                                 .sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIT()},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                             .backgroundColor = Clay_Hovered() ? bg_l : bg_d,
                                             .cornerRadius = CORNER_RADIUS}) {
                Clay_OnHover(clicked_calculate_heat, (intptr_t)appl);
                ui_draw_text("Calculate Heat", LABEL_FONT_SIZE, dark_aqua, CLAY_TEXT_ALIGN_CENTER);
            }

            CLAY(CLAY_ID("DisplayFilteredButton"), {.layout = {
                                                        .padding = CLAY_PADDING_ALL(GAPS),
                                                        .sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIT()},
                                                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                    .backgroundColor = Clay_Hovered() ? bg_l : bg_d,
                                                    .cornerRadius = CORNER_RADIUS}) {
                Clay_OnHover(clicked_show_filtered_tracks, (intptr_t)collection);
                ui_draw_text("Show Filtered Tracks", LABEL_FONT_SIZE, dark_aqua, CLAY_TEXT_ALIGN_CENTER);
            }
            // Re-filter only when a type toggle actually changed.
            if (memcmp(shown_before, collection->filters.show_activity,
                       sizeof(shown_before)) != 0)
                apply_filter_values(collection);
        }
    }
}

// Rewrites the active field's text from the digits typed so far. Called on each
// keystroke rather than from the layout pass, so drawing stays read-only.
static void refresh_active_filter_text(FilterSettings *filters) {
    FilterAttribute attribute;
    FilterBoundEnd end;
    if (ui.active_filter_id < 0 ||
        !filter_field_unpack((uint16_t)ui.active_filter_id, &attribute, &end))
        return;

    format_filter_input(filter_bound_text(filters, attribute, end),
                        filter_format(attribute));
}

bool ui_text_input_active(void) {
    return ui.text_input_mode;
}

// Accepts one typed digit into the field being edited.
void ui_text_input_digit(GpxCollection *collection, char digit) {
    // Leave space for the null terminator.
    if (ui.text_input_length < sizeof(ui.text_input_buffer) - 1) {
        ui.text_input_buffer[ui.text_input_length++] = digit;
        ui.text_input_buffer[ui.text_input_length] = '\0';
    }
    refresh_active_filter_text(&collection->filters);
}

// Leaves input mode and commits what was typed.
void ui_text_input_finish(GpxCollection *collection) {
    ui.text_input_mode = false;
    ui.active_filter_id = NO_ACTIVE_FILTER;
    LOG_DEBUG("%s\n", ui.text_input_buffer);
    save_filter_values(&collection->filters);
    apply_filter_values(collection);
}
