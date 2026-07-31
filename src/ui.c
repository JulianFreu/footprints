#include "ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "filters.h"
#include "heat.h"
#include "tracks.h"

#include "log.h"

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_renderer_sdl.c"
#include "colors.h"

#define SCREEN_BORDER_PADDING 5
#define CORNER_RADIUS 8
#define GAPS 5
#define SIDEBAR_WIDTH 250
#define MENU_BAR_WIDTH 250
#define MENU_ICON_SIZE 32 + 2 * GAPS

// Text sizes, in points, for the three kinds of label the UI draws.
#define FILTER_TEXT_FONT_SIZE 12
#define LABEL_FONT_SIZE 16
#define HEADING_FONT_SIZE 20

#define ELEMENTS_HEIGHT 30
#define ELEMENTS_WIDTH 180
#define LIST_ENTRY_HEIGHT 30
#define HEADER_HEIGHT 50
#define FILTERS_WIDTH 300
#define FILTERS_MINMAX_WIDTH 80

#define WIDTH_TYPE 100
#define WIDTH_DATE 100
#define WIDTH_DISTANCE 100
#define WIDTH_PACE 100
#define WIDTH_DURATION 100
#define WIDTH_UPHILL 100
#define WIDTH_DOWNHILL 100
#define WIDTH_TOP 100

#define NONE 0
#define IMPORT_DATA 1
#define LIST_RUNS 2
#define SETTINGS 3
#define STATISTICS 4

UIState ui = {
    .right_sidebar = {0},
    .run_list = {0},
    .filters_animation = {0},
    .active_filter_id = NO_ACTIVE_FILTER};

static GpxCollection *g_collection = NULL; // tmp global pointer for compare functions
static Clay_Arena clay_memory;

static bool ui_new_track_selected = false;
static int ui_track = -1;

// Starts a panel opening or closing, whichever is the reverse of what it is
// currently doing.
static void animation_toggle(struct AnimationState *anim_obj) {
    bool should_open = !(anim_obj->opening || anim_obj->progress > 0.0f);
    anim_obj->opening = should_open;
    anim_obj->closing = !should_open;
}

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

static void draw_clay_text(const char *string, uint16_t fontSize, Clay_Color color, Clay_TextAlignment align) {
    Clay_String clay_string = {
        .chars = string,
        .length = strlen(string),
        .isStaticallyAllocated = false};
    CLAY_TEXT(clay_string, CLAY_TEXT_CONFIG({.fontSize = fontSize, .textColor = color, .textAlignment = align}));
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
        // Drawing only reads the field. It used to reformat the stored text on
        // every frame, so laying out the UI rewrote the model it was drawing.
        draw_clay_text(filter_bound_text(filters, attribute, end), FILTER_TEXT_FONT_SIZE, bg1, CLAY_TEXT_ALIGN_CENTER);
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
            draw_clay_text("Min", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_ID_LOCAL("FilterType"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            draw_clay_text("Type", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_ID_LOCAL("FilterMax"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            draw_clay_text("Max", HEADING_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
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
        draw_clay_text(activity_type_label(type), LABEL_FONT_SIZE, bg, CLAY_TEXT_ALIGN_CENTER);
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
            draw_clay_text("<", LABEL_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_IDI_LOCAL("FilterName", attribute),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            draw_clay_text(filter_display_name(attribute), LABEL_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_IDI_LOCAL("greater", attribute),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            draw_clay_text("<", LABEL_FONT_SIZE, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        draw_input_field(attribute, BOUND_HIGH, filters);
    }
}

// Sort key for the numeric attributes. All of them sort descending, so one
// comparator covers the lot; type and date need their own orderings and are
// handled directly in compare_tracks.
static float track_sort_key(const GpxTrack *track, AttributeType criteria) {
    switch (criteria) {
    case DISTANCE:
        return track->distance;
    case DURATION:
        return track->duration_secs;
    case PACE:
        return track->secs_per_km;
    case UPHILL:
        return track->elev_up;
    case DOWNHILL:
        return track->elev_down;
    case HIGHPOINT:
        return track->high_point;
    default:
        return 0.0f;
    }
}

// Criteria the active qsort is ordering by. qsort gives the comparator no
// user-data channel, so this and g_collection are set by sort_tracks around
// the call and cleared afterwards; sorting must stay single threaded.
static AttributeType g_sort_criteria = ID;

static int compare_tracks(const void *a, const void *b) {
    const GpxTrack *t1 = &g_collection->tracks[*(const int *)a];
    const GpxTrack *t2 = &g_collection->tracks[*(const int *)b];

    if (g_sort_criteria == TYPE) {
        int a_type = t1->act_type, b_type = t2->act_type;
        return (a_type < b_type) - (a_type > b_type);
    }
    if (g_sort_criteria == DATE)
        return -strcmp(t1->start_time_raw, t2->start_time_raw); // latest first

    // Pace is "lower is better", so it reads ascending; the rest descending.
    float k1 = track_sort_key(t1, g_sort_criteria);
    float k2 = track_sort_key(t2, g_sort_criteria);
    if (g_sort_criteria == PACE)
        return (k1 > k2) - (k1 < k2);
    return (k1 < k2) - (k1 > k2);
}

static void sort_tracks(GpxCollection *collection, AttributeType criteria) {
    g_collection = collection;
    g_sort_criteria = criteria;

    int n = collection->total_tracks;

    // Initialize list_order with 0..n-1
    for (int i = 0; i < n; ++i)
        collection->list_order[i] = i;

    qsort(collection->list_order, n, sizeof(int), compare_tracks);

    g_collection = NULL; // clear global pointer for safety
}

static void reverse_list_order(GpxCollection *collection) {
    int *order = collection->list_order;
    int n = collection->total_tracks;

    for (int i = 0; i < n / 2; ++i) {
        int temp = order[i];
        order[i] = order[n - 1 - i];
        order[n - 1 - i] = temp;
    }
}

// Clicking the column that is already sorted flips the order.
static void sort_tracks_by(GpxCollection *collection, AttributeType criteria) {
    if (collection->current_sorting == criteria) {
        reverse_list_order(collection);
        return;
    }
    switch (criteria) {
    case TYPE:
    case DATE:
    case DISTANCE:
    case DURATION:
    case PACE:
    case UPHILL:
    case DOWNHILL:
    case HIGHPOINT:
        sort_tracks(collection, criteria);
        collection->current_sorting = criteria;
        break;
    default:
        fprintf(stderr, "Unknown sort criteria\n");
        break;
    }
}

static void clicked_list_headers(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        GpxCollection *collection = (GpxCollection *)userData;
        sort_tracks_by(collection, collection->to_be_sorted_by);
    }
}

static void clicked_calculate_heat(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        GpxCollection *collection = (GpxCollection *)userData;

        // reset heat for all points
        for (int track = 0; track < collection->total_tracks; track++) {
            for (int pt = 0; pt < collection->tracks[track].total_points; pt++) {
                collection->tracks[track].points[pt].heat = 0;
            }
        }
        // recalculate heat
        calculate_heatmap(collection);
        tracks_invalidate_cache(collection);
    }
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

static void clicked_toggle_filter_view(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        animation_toggle(&ui.filters_animation);
}
static void clicked_run_entry(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        ui_new_track_selected = true;
        ui_track = userData;
    }
}

float get_delta_time(Uint32 last_frame_time) {
    Uint32 now = SDL_GetTicks();
    float deltaTime = ((float)now - (float)last_frame_time) / 1000.0f;
    return deltaTime;
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
        draw_clay_text(str, LABEL_FONT_SIZE, bg_d, CLAY_TEXT_ALIGN_CENTER);
        switch (sort_type) {
        case DATE:
            draw_clay_text("[dd:mm:yyyy]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case DISTANCE:
            draw_clay_text("[km]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case PACE:
            draw_clay_text("[min/km]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case DURATION:
            draw_clay_text("[hh:mm:ss]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case UPHILL:
            draw_clay_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case DOWNHILL:
            draw_clay_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case HIGHPOINT:
            draw_clay_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
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
            draw_clay_text("Toggle Filter View", 16, dark_aqua, CLAY_TEXT_ALIGN_CENTER);
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
        draw_run_list_header_attribute(collection, WIDTH_TYPE, "Type", TYPE);
        draw_run_list_header_attribute(collection, WIDTH_DATE, "Date", DATE);
        draw_run_list_header_attribute(collection, WIDTH_DISTANCE, "Distance", DISTANCE);
        draw_run_list_header_attribute(collection, WIDTH_PACE, "Pace", PACE);
        draw_run_list_header_attribute(collection, WIDTH_DURATION, "Duration", DURATION);
        draw_run_list_header_attribute(collection, WIDTH_UPHILL, "Uphill", UPHILL);
        draw_run_list_header_attribute(collection, WIDTH_DOWNHILL, "Downhill", DOWNHILL);
        draw_run_list_header_attribute(collection, WIDTH_TOP, "Highest", HIGHPOINT);
    }
}

static void draw_run_entry_attribute(int width, const char *str, int id) {
    CLAY(CLAY_IDI_LOCAL("RunEntryAttribute", id),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
        // showClayText(str, 16, bg_d, CLAY_TEXT_ALIGN_CENTER);
        draw_clay_text(str, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

static void draw_run_list_entry(GpxTrack *track) {
    CLAY(CLAY_IDI_LOCAL("RunListEntry", track->track_id),
         {
             .border = {.color = border, .width = (ui_track == track->track_id) ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(2) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = Clay_Hovered() ? bg_l : bg_d,
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(clicked_run_entry, track->track_id);
        draw_run_entry_attribute(WIDTH_TYPE, activity_type_label(track->act_type), track->track_id * 8 + 0);

        draw_run_entry_attribute(WIDTH_DATE, track->start_date_str, track->track_id * 8 + 1);
        draw_run_entry_attribute(WIDTH_DISTANCE, track->distance_str, track->track_id * 8 + 2);
        draw_run_entry_attribute(WIDTH_PACE, track->pace_str, track->track_id * 8 + 3);
        draw_run_entry_attribute(WIDTH_DURATION, track->duration_str, track->track_id * 8 + 4);
        draw_run_entry_attribute(WIDTH_UPHILL, track->elev_up_str, track->track_id * 8 + 5);
        draw_run_entry_attribute(WIDTH_DOWNHILL, track->elev_down_str, track->track_id * 8 + 6);
        draw_run_entry_attribute(WIDTH_TOP, track->high_point_str, track->track_id * 8 + 7);
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
static void free_visible_rows(void) {
    free(visible_rows);
    visible_rows = NULL;
    visible_rows_capacity = 0;
}

// Advances a panel's slide by however much wall-clock time has passed.
//
// The step used to be a fixed number of degrees per frame, which tied the
// speed of every panel to the frame rate: the same slide took a quarter of a
// second at 60 fps and a second and a half at 10.
static void continue_animation(struct AnimationState *anim_obj, float delta_time) {
    // progress runs 0..90 and is read as degrees, so sin() eases the ends:
    // sin(0) = 0 and sin(90 degrees) = 1.
    float step = delta_time * PANEL_ANIMATION_DEGREES_PER_SECOND;

    if (anim_obj->opening) {
        anim_obj->progress += step;
        if (anim_obj->progress >= 90.0f) {
            anim_obj->progress = 90.0f;
            anim_obj->opening = false;
        }
    } else if (anim_obj->closing) {
        anim_obj->progress -= step;
        if (anim_obj->progress <= 0.0f) {
            anim_obj->progress = 0.0f;
            anim_obj->closing = false;
        }
    } else {
        return;
    }

    anim_obj->animation = sinf(anim_obj->progress * (float)M_PI / 180.0f);
}

static void clay_handle_error(Clay_ErrorData error) {
    // Convert error text (Clay_StringSlice) to null-terminated string
    char buffer[512];
    size_t len = error.errorText.length;
    if (len >= sizeof(buffer))
        len = sizeof(buffer) - 1;

    memcpy(buffer, error.errorText.chars, len);
    buffer[len] = '\0';

    // Log or print the error
    fprintf(stderr, "Clay ERROR: %s\n", buffer);
}

void clay_init(struct application *appl) {
    printf("[CLAY] clay_Init called\n");

    Clay_SetMaxElementCount(32000);
    // Configure Clay
    uint32_t clay_required_memory = Clay_MinMemorySize();

    clay_memory = (Clay_Arena){
        .memory = malloc(clay_required_memory),
        .capacity = clay_required_memory};

    Clay_Initialize(
        clay_memory,
        (Clay_Dimensions){.width = appl->window_width, .height = appl->window_height},
        (Clay_ErrorHandler){.errorHandlerFunction = clay_handle_error, .userData = 0});
    Clay_SetMeasureTextFunction(SDL2_MeasureText, appl->fonts);
}

void clay_free_memory(void) {
    free_visible_rows();
    free(clay_memory.memory);
    clay_memory.memory = NULL;
    clay_memory.capacity = 0;
}

static SDL_Surface *load_icon(const char *path) {
    SDL_Surface *surface = IMG_Load(path);
    if (!surface)
        fprintf(stderr, "Warning: could not load icon %s: %s\n", path, IMG_GetError());
    return surface;
}

// Decode every static icon once. Previously each of these was reloaded from
// disk on every frame and never freed.
void ui_load_icons(struct application *appl) {
    appl->icons.menu_burger = load_icon("resources/menu-burger.png");
    if (appl->icons.menu_burger)
        SDL_SetSurfaceColorMod(appl->icons.menu_burger, 250, 0, 0);

    appl->icons.date = load_icon("resources/date.png");
    appl->icons.clock = load_icon("resources/clock.png");
    appl->icons.duration = load_icon("resources/duration.png");
    appl->icons.pace = load_icon("resources/pace.png");
    appl->icons.distance = load_icon("resources/distance.png");
    appl->icons.elev_up = load_icon("resources/up.png");
    appl->icons.elev_down = load_icon("resources/down.png");
    appl->icons.peak = load_icon("resources/peak.png");
    appl->icons.elev_profile = NULL;
}

void ui_free_icons(struct application *appl) {
    SDL_Surface **surfaces[] = {
        &appl->icons.menu_burger, &appl->icons.date, &appl->icons.clock,
        &appl->icons.duration, &appl->icons.pace, &appl->icons.distance,
        &appl->icons.elev_up, &appl->icons.elev_down, &appl->icons.peak,
        &appl->icons.elev_profile};

    for (size_t i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); i++) {
        if (*surfaces[i]) {
            SDL_FreeSurface(*surfaces[i]);
            *surfaces[i] = NULL;
        }
    }
}

static const Clay_LayoutConfig MenuButtonLayout = {
    .sizing = {.width = CLAY_SIZING_FIXED(MENU_ICON_SIZE), .height = CLAY_SIZING_FIXED(MENU_ICON_SIZE)},
    .childGap = GAPS,
    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
};
// The run list and the filter panel slide together, from the menu button and
// from the TAB key alike.
void ui_toggle_run_list(void) {
    animation_toggle(&ui.run_list);
    ui.filters_animation.opening = ui.run_list.opening;
    ui.filters_animation.closing = ui.run_list.closing;
}

static void clicked_menu_button(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        ui_toggle_run_list();
}

static void draw_menu_button(SDL_Surface *icon, Clay_Color color, uint32_t button_id) {
    CLAY(CLAY_IDI_LOCAL("MenuButton", button_id),
         {
             .layout = MenuButtonLayout,
             .backgroundColor = Clay_Hovered() ? big_button_color : color,
             .cornerRadius = CORNER_RADIUS,
         }) {
        Clay_OnHover(clicked_menu_button, button_id);
        CLAY(CLAY_IDI_LOCAL("MenuButtonIcon", button_id),
             {.layout = {
                  .padding = CLAY_PADDING_ALL(GAPS),
                  .sizing = {.width = CLAY_SIZING_FIXED(32),
                             .height = CLAY_SIZING_FIXED(32)}},
              .image = icon}) {}
    }
}

static void draw_sidebar_track_info(SDL_Surface *icon, const char *value, const char *unit, int id) {
    CLAY(CLAY_IDI_LOCAL("SidebarAttribute", id),
         {
             .layout = {
                 .padding = CLAY_PADDING_ALL(10),
                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT()},
                 .layoutDirection = CLAY_LEFT_TO_RIGHT},
         }) {
        CLAY(CLAY_IDI_LOCAL("Icon", id),
             {
                 .layout = {
                     .sizing = {.width = CLAY_SIZING_FIXED(32), .height = CLAY_SIZING_FIXED(32)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                 },
                 .image = {.imageData = icon},
             }) {
        }
        CLAY(CLAY_IDI_LOCAL("Empty", id),
             {
                 .layout = {
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                 },
             }) {
        }
        CLAY(CLAY_IDI_LOCAL("Value", id),
             {
                 .layout = {
                     .sizing = {.width = CLAY_SIZING_FIXED(100), .height = CLAY_SIZING_GROW(0)},
                     .childAlignment = {.x = CLAY_ALIGN_X_RIGHT, .y = CLAY_ALIGN_Y_CENTER},
                 },
             }) {
            draw_clay_text(value, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_IDI_LOCAL("Unit", id),
             {
                 .layout = {
                     .sizing = {.width = CLAY_SIZING_FIXED(70), .height = CLAY_SIZING_GROW(0)},
                     .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                     .padding = {.left = GAPS}},
             }) {
            draw_clay_text(unit, 12, fg_l, CLAY_TEXT_ALIGN_CENTER);
        }
    }
}

void clay_draw_ui(struct application *appl, GpxCollection *collection) {
    if (!clay_memory.memory) {
        fprintf(stderr, "[CLAY] ERROR: clay_memory not initialized!\n");
        return;
    }
    // defaulting

    appl->mouse_over_ui = false;
    if (ui_new_track_selected == true) {
        appl->selected_track = ui_track;
        ui_new_track_selected = false;
        appl->world_x = collection->tracks[ui_track].mid_x - ((appl->window_width / 4) << (MAX_ZOOM - appl->zoom));
        appl->world_y = collection->tracks[ui_track].mid_y;
    }

    Clay_SetPointerState(
        (Clay_Vector2){appl->mouse_x, appl->mouse_y}, appl->left_mouse_button_pressed);

    float delta_time = get_delta_time(appl->last_frame_time);

    Clay_UpdateScrollContainers(
        false,
        (Clay_Vector2){0, (float)appl->wheel_y * SCROLL_PIXELS_PER_WHEEL_STEP},
        delta_time);

    // filter options
    continue_animation(&ui.filters_animation, delta_time);

    // right sidebar
    if (appl->selected_track > -1) {
        ui.right_sidebar.closing = false;
        if (ui.right_sidebar.animation < 1)
            ui.right_sidebar.opening = true;
    } else {
        ui.right_sidebar.opening = false;
        if (ui.right_sidebar.animation > 0)
            ui.right_sidebar.closing = true;
    }
    continue_animation(&ui.right_sidebar, delta_time);

    // left sidebar
    continue_animation(&ui.run_list, delta_time);

    // draw UI
    char fps_label[64];
    snprintf(fps_label, sizeof(fps_label), "FPS: %d", appl->current_fps);

    Clay_SetLayoutDimensions((Clay_Dimensions){
        .width = appl->window_width,
        .height = appl->window_height});

    Clay_BeginLayout();

    CLAY(CLAY_ID("MenuBar"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {
                  .x = -MENU_BAR_WIDTH + (SCREEN_BORDER_PADDING + MENU_BAR_WIDTH),
                  .y = SCREEN_BORDER_PADDING},
          },
          .layout = {.childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(MENU_BAR_WIDTH), .height = CLAY_SIZING_FIT()}, .layoutDirection = CLAY_LEFT_TO_RIGHT},
          .cornerRadius = CORNER_RADIUS}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_menu_button(appl->icons.menu_burger, dark_red, LIST_RUNS);
    }

    CLAY(CLAY_ID("Right sidebar"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {
                  .x = appl->window_width - ui.right_sidebar.animation * (SCREEN_BORDER_PADDING + SIDEBAR_WIDTH),
                  .y = SCREEN_BORDER_PADDING

              },
          },
          .layout = {.padding = CLAY_PADDING_ALL(GAPS), .childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(SIDEBAR_WIDTH), .height = appl->window_height - 2 * SCREEN_BORDER_PADDING}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = bg,
          .border = {.color = dark_aqua, .width = {.betweenChildren = 2}},
          .cornerRadius = CORNER_RADIUS}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        if (appl->selected_track >= 0) {
            const GpxTrack *track = &collection->tracks[appl->selected_track];

            draw_sidebar_track_info(appl->icons.date, track->start_date_str, "", 0);
            draw_sidebar_track_info(appl->icons.clock, track->start_time_str, "", 1);
            draw_sidebar_track_info(appl->icons.duration, track->duration_str, "h", 2);
            draw_sidebar_track_info(appl->icons.pace, track->pace_str, "min/km", 3);
            draw_sidebar_track_info(appl->icons.distance, track->distance_str, "km", 4);
            draw_sidebar_track_info(appl->icons.elev_up, track->elev_up_str, "m", 5);
            draw_sidebar_track_info(appl->icons.elev_down, track->elev_down_str, "m", 6);
            draw_sidebar_track_info(appl->icons.peak, track->high_point_str, "m", 7);

            draw_sidebar_track_info(appl->icons.peak, activity_type_label(track->act_type), " ", 8);

            // Reloaded by update_track_info_graphs when the selection changes.
            if (appl->icons.elev_profile) {
                CLAY(CLAY_ID("ElevationProfile"),
                     {.layout = {.sizing = {.width = CLAY_SIZING_GROW(200), .height = CLAY_SIZING_FIXED(100)}},
                      .image = {.imageData = appl->icons.elev_profile}}) {
                }
            }
        }
    }

    uint16_t list_width = WIDTH_TYPE + WIDTH_DATE + WIDTH_DISTANCE + WIDTH_PACE + WIDTH_DURATION + WIDTH_UPHILL + WIDTH_DOWNHILL + WIDTH_TOP + 2 * GAPS;
    uint16_t list_offset_y = MENU_ICON_SIZE + 2 * SCREEN_BORDER_PADDING;
    if (ui.filters_animation.animation != 0) {
        CLAY(CLAY_ID("FilterOptions"),
             {.floating = {
                  .attachTo = CLAY_ATTACH_TO_ROOT,
                  .offset = {
                      .x = ui.run_list.animation * (SCREEN_BORDER_PADDING + list_width) - (FILTERS_WIDTH + GAPS) + ui.filters_animation.animation * (FILTERS_WIDTH + 2 * GAPS),
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
                draw_clay_text(collection->total_visible_tracks_str, LABEL_FONT_SIZE, fg, CLAY_TEXT_ALIGN_CENTER);
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
                Clay_OnHover(clicked_calculate_heat, (intptr_t)collection);
                draw_clay_text("Calculate Heat", LABEL_FONT_SIZE, dark_aqua, CLAY_TEXT_ALIGN_CENTER);
            }

            CLAY(CLAY_ID("DisplayFilteredButton"), {.layout = {
                                                        .padding = CLAY_PADDING_ALL(GAPS),
                                                        .sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIT()},
                                                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                    .backgroundColor = Clay_Hovered() ? bg_l : bg_d,
                                                    .cornerRadius = CORNER_RADIUS}) {
                Clay_OnHover(clicked_show_filtered_tracks, (intptr_t)collection);
                draw_clay_text("Show Filtered Tracks", LABEL_FONT_SIZE, dark_aqua, CLAY_TEXT_ALIGN_CENTER);
            }
            // Re-filter only when a type toggle actually changed.
            if (memcmp(shown_before, collection->filters.show_activity,
                       sizeof(shown_before)) != 0)
                apply_filter_values(collection);
        }
    }
    CLAY(CLAY_ID("RunsListMenu"),
         {
             .floating = {
                 .attachTo = CLAY_ATTACH_TO_ROOT,
                 .offset = {
                     .x = -list_width + ui.run_list.animation * (SCREEN_BORDER_PADDING + list_width),
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

    Clay_RenderCommandArray ui_renderCommands = Clay_EndLayout();
    Clay_SDL2_Render(appl->renderer, ui_renderCommands, appl->fonts);
    // end UI
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

void ui_text_input_begin(void) {
    init_numbers_input();
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
