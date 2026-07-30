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
    .right_sidebar = {.opening = false, .closing = false, .animation = 0, .ticks = 0},
    .run_list = {.opening = false, .closing = false, .animation = 0, .ticks = 0},
    .filters_animation = {.opening = false, .closing = false, .animation = 0, .ticks = 0}};

static GpxCollection *g_collection = NULL; // tmp global pointer for compare functions
static Clay_Arena clayMemory;

static bool ui_new_track_selected = false;
static int ui_track = -1;

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
    Clay_String clayString = {
        .chars = string,
        .length = strlen(string),
        .isStaticallyAllocated = false};
    CLAY_TEXT(clayString, CLAY_TEXT_CONFIG({.fontSize = fontSize, .textColor = color, .textAlignment = align}));
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
        ui.activeFilterID = (uint16_t)userData;
        LOG_DEBUG("start number input\n");
    }
}

static void clear_char_array(char *str, int size) {
    for (int i = 0; i < size; i++)
        str[i] = '\0';
}

static void format_pace_filter_str(char *str) {
    if (ui.text_input_length < 7) {
        clear_char_array(str, INPUT_BUFFER_SIZE);
        snprintf(str, INPUT_BUFFER_SIZE, "%s", ui.text_input_buffer);
        if (ui.text_input_length > 2) {
            str[ui.text_input_length] = str[ui.text_input_length - 1];
            str[ui.text_input_length - 1] = str[ui.text_input_length - 2];
            str[ui.text_input_length - 2] = ':';
        }
    }
}

static void format_date_filter_str(char *str) {
    if (ui.text_input_length < 9) {
        clear_char_array(str, INPUT_BUFFER_SIZE);
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
        clear_char_array(str, INPUT_BUFFER_SIZE);
        snprintf(str, INPUT_BUFFER_SIZE, "%s", ui.text_input_buffer);
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
        clear_char_array(str, INPUT_BUFFER_SIZE);
        snprintf(str, INPUT_BUFFER_SIZE, "%s", ui.text_input_buffer);
    }
}

static void format_distance_filter_str(char *str) {
    if (ui.text_input_length == 0) {
        clear_char_array(str, INPUT_BUFFER_SIZE);
    } else if (ui.text_input_length == 1) {
        clear_char_array(str, INPUT_BUFFER_SIZE);
        str[0] = '0';
        str[1] = '.';
        str[2] = '0';
        str[3] = ui.text_input_buffer[0];
    } else if (ui.text_input_length == 2) {
        clear_char_array(str, INPUT_BUFFER_SIZE);
        str[0] = '0';
        str[1] = '.';
        str[2] = ui.text_input_buffer[0];
        str[3] = ui.text_input_buffer[1];
    } else if (ui.text_input_length < 7) {
        clear_char_array(str, INPUT_BUFFER_SIZE);
        snprintf(str, INPUT_BUFFER_SIZE, "%s", ui.text_input_buffer);
        str[ui.text_input_length - 2] = '.';
        str[ui.text_input_length - 1] = ui.text_input_buffer[ui.text_input_length - 2];
        str[ui.text_input_length] = ui.text_input_buffer[ui.text_input_length - 1];
    }
}

static void draw_input_field(uint16_t filter_id, FilterSettings *filters) {
    CLAY(CLAY_IDI_LOCAL("InputFieldFilter", filter_id),
         {
             .border = {.color = border, .width = (ui.text_input_mode && ui.activeFilterID == filter_id) ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(3) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)},
                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = Clay_Hovered() ? blue : darkBlue,
             .cornerRadius = CORNER_RADIUS,
         }) {
        int font_size = 12;
        Clay_Color color = bg1;
        Clay_OnHover(clicked_filter_field, filter_id);
        switch (filter_id) {
        case FILTER_DATE | HIGH_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_date_filter_str(filters->end_date_str);
            draw_clay_text(filters->end_date_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_DATE | LOW_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_date_filter_str(filters->start_date_str);
            draw_clay_text(filters->start_date_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_DISTANCE | HIGH_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_distance_filter_str(filters->distance_high_str);
            draw_clay_text(filters->distance_high_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_DISTANCE | LOW_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_distance_filter_str(filters->distance_low_str);
            draw_clay_text(filters->distance_low_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_DURATION | HIGH_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_duration_filter_str(filters->duration_high_str);
            draw_clay_text(filters->duration_high_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_DURATION | LOW_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_duration_filter_str(filters->duration_low_str);
            draw_clay_text(filters->duration_low_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_PACE | HIGH_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_pace_filter_str(filters->pace_high_str);
            draw_clay_text(filters->pace_high_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_PACE | LOW_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_pace_filter_str(filters->pace_low_str);
            draw_clay_text(filters->pace_low_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_UPHILL | HIGH_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_elev_filter_str(filters->elev_up_high_str);
            draw_clay_text(filters->elev_up_high_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_UPHILL | LOW_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_elev_filter_str(filters->elev_up_low_str);
            draw_clay_text(filters->elev_up_low_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_DOWNHILL | HIGH_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_elev_filter_str(filters->elev_down_high_str);
            draw_clay_text(filters->elev_down_high_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_DOWNHILL | LOW_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_elev_filter_str(filters->elev_down_low_str);
            draw_clay_text(filters->elev_down_low_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_PEAK | HIGH_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_elev_filter_str(filters->high_point_high_str);
            draw_clay_text(filters->high_point_high_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        case FILTER_PEAK | LOW_LIMIT:
            if (filter_id == ui.activeFilterID)
                format_elev_filter_str(filters->high_point_low_str);
            draw_clay_text(filters->high_point_low_str, font_size, color, CLAY_TEXT_ALIGN_CENTER);
            break;
        default:
            break;
        }
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
            draw_clay_text("Min", 20, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_ID_LOCAL("FilterType"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            draw_clay_text("Type", 20, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_ID_LOCAL("FilterMax"),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_MINMAX_WIDTH), .height = CLAY_SIZING_GROW(0)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            draw_clay_text("Max", 20, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
    }
}

static void draw_type_filter(ActivityType type, bool *show_type) {
    Clay_Color background_color;
    Clay_Color background_color_hl;
    if (*show_type == true) {
        background_color = darkGreen;
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
        draw_clay_text(activity_type_label(type), 16, bg, CLAY_TEXT_ALIGN_CENTER);
    }
}

static void draw_type_filter_container(FilterSettings *filter) {
    CLAY(CLAY_ID_LOCAL("TypesFilterContainer"),
         {
             .layout = {.padding = CLAY_PADDING_ALL(3 * GAPS), .childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(FILTERS_WIDTH / 2), .height = CLAY_SIZING_FIT(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
             .cornerRadius = CORNER_RADIUS,
         }) {
        draw_type_filter(Run, &filter->showRuns);
        draw_type_filter(Cycling, &filter->showCycling);
        draw_type_filter(Hike, &filter->showHikes);
        draw_type_filter(Other, &filter->showOther);
    }
}

static void draw_filter(uint16_t filter_id, FilterSettings *filters) {
    CLAY(CLAY_IDI_LOCAL("Filter", filter_id),
         {
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = GAPS},
             .backgroundColor = bg1,
             .cornerRadius = CORNER_RADIUS,
         }) {
        draw_input_field(filter_id | LOW_LIMIT, filters);
        CLAY(CLAY_IDI_LOCAL("lesser", filter_id),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            draw_clay_text("<", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_IDI_LOCAL("FilterName", filter_id),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            switch (filter_id) {
            case FILTER_DISTANCE:
                draw_clay_text("distance", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
                break;
            case FILTER_DURATION:
                draw_clay_text("duration", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
                break;
            case FILTER_PACE:
                draw_clay_text("pace", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
                break;
            case FILTER_DATE:
                draw_clay_text("date", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
                break;
            case FILTER_UPHILL:
                draw_clay_text("up", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
                break;
            case FILTER_DOWNHILL:
                draw_clay_text("down", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
                break;
            case FILTER_PEAK:
                draw_clay_text("peak", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
                break;
            default:
                break;
            }
        }
        CLAY(CLAY_IDI_LOCAL("greater", filter_id),
             {
                 .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}},
                 .backgroundColor = bg1,
                 .cornerRadius = CORNER_RADIUS,
             }) {
            draw_clay_text("<", 16, fg1, CLAY_TEXT_ALIGN_CENTER);
        }
        draw_input_field(filter_id | HIGH_LIMIT, filters);
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
        free_track_tile_cache(&collection->track_tile_cache);
    }
}
static void clicked_show_filtered_tracks(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        TrackTileTextureCache *tracks_cache = (TrackTileTextureCache *)userData;
        free_track_tile_cache(tracks_cache);
    }
}

static void clicked_toggle_filter_view(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (ui.filters_animation.animation > 0) {
            ui.filters_animation.opening = false;
            ui.filters_animation.closing = true;
        } else {
            ui.filters_animation.opening = true;
            ui.filters_animation.closing = false;
        }
    }
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

float get_delta_time(Uint32 lastFrameTime) {
    Uint32 now = SDL_GetTicks();
    float deltaTime = ((float)now - (float)lastFrameTime) / 1000.0f;
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
        draw_clay_text(str, 16, bg_d, CLAY_TEXT_ALIGN_CENTER);
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
            draw_clay_text("Toggle Filter View", 16, darkAqua, CLAY_TEXT_ALIGN_CENTER);
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
        draw_clay_text(str, 16, fg_l, CLAY_TEXT_ALIGN_CENTER);
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

static void draw_run_list_scroll_container(GpxCollection *collection, int height) {
    CLAY(CLAY_ID("RunListScrollContainer"),
         {
             .layout = {
                 .padding = CLAY_PADDING_ALL(GAPS),
                 .childGap = GAPS,
                 .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(height) /*CLAY_SIZING_GROW()*/},
                 .layoutDirection = CLAY_TOP_TO_BOTTOM},
             .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()},
             .backgroundColor = bg,
         }) {
        if (collection->total_tracks > 0) {
            for (int i = 0; i < collection->total_tracks; i++) {
                if (collection->tracks[collection->list_order[i]].visible_in_list) // check if filtered out
                    draw_run_list_entry(&collection->tracks[collection->list_order[i]]);
            }
        }
    }
}

static void continue_animation(struct AnimationState *anim_obj) {
    // ticks are used as radians in sin(), -> sin(90°) = 1 and sin(0°) = 0
    if (anim_obj->opening) {
        anim_obj->ticks += 4;
        if (anim_obj->ticks >= 90) {
            anim_obj->ticks = 90;
            anim_obj->opening = false;
        }
        anim_obj->animation = sin(anim_obj->ticks * M_PI / 180);
    } else if (anim_obj->closing) {
        anim_obj->ticks -= 4;
        if (anim_obj->ticks <= 0) {
            anim_obj->ticks = 0;
            anim_obj->closing = false;
        }
        anim_obj->animation = sin(anim_obj->ticks * M_PI / 180);
    }
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
    uint32_t clayRequiredMemory = Clay_MinMemorySize();

    clayMemory = (Clay_Arena){
        .memory = malloc(clayRequiredMemory),
        .capacity = clayRequiredMemory};

    Clay_Initialize(
        clayMemory,
        (Clay_Dimensions){.width = appl->window_width, .height = appl->window_height},
        (Clay_ErrorHandler){.errorHandlerFunction = clay_handle_error, .userData = 0});
    Clay_SetMeasureTextFunction(SDL2_MeasureText, appl->fonts);
}

void clay_free_memory(void) {
    free(clayMemory.memory);
    clayMemory.memory = NULL;
    clayMemory.capacity = 0;
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
static void clicked_menu_button(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (ui.run_list.opening == true || ui.run_list.ticks > 0) {
            ui.run_list.opening = false;
            ui.run_list.closing = true;
            ui.filters_animation.opening = false;
            ui.filters_animation.closing = true;
        } else if (ui.run_list.closing == true || ui.run_list.ticks == 0) {
            ui.run_list.opening = true;
            ui.run_list.closing = false;
            ui.filters_animation.opening = true;
            ui.filters_animation.closing = false;
        }
    }
}

static void draw_menu_button(SDL_Surface *icon, Clay_Color color, uint32_t button_id) {
    CLAY(CLAY_IDI_LOCAL("MenuButton", button_id),
         {
             .layout = MenuButtonLayout,
             .backgroundColor = Clay_Hovered() ? bigButtonColor : color,
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
            draw_clay_text(value, 16, fg_l, CLAY_TEXT_ALIGN_CENTER);
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
    if (!clayMemory.memory) {
        fprintf(stderr, "[CLAY] ERROR: clayMemory not initialized!\n");
        return;
    }
    // defaulting

    appl->mouseOverUI = false;
    if (ui_new_track_selected == true) {
        appl->selected_track = ui_track;
        ui_new_track_selected = false;
        appl->world_x = collection->tracks[ui_track].mid_x - ((appl->window_width / 4) << (MAX_ZOOM - appl->zoom));
        appl->world_y = collection->tracks[ui_track].mid_y;
    }

    Clay_SetPointerState(
        (Clay_Vector2){appl->mouse_x, appl->mouse_y}, appl->leftMouseButtonPressed);

    Clay_UpdateScrollContainers(
        false,
        (Clay_Vector2){0, (float)appl->wheel_y * 5},
        get_delta_time(appl->lastFrameTime));

    // filter options
    continue_animation(&ui.filters_animation);

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
    continue_animation(&ui.right_sidebar);

    // left sidebar
    continue_animation(&ui.run_list);

    // draw UI
    char fpsLabel[64];
    snprintf(fpsLabel, sizeof(fpsLabel), "FPS: %d", appl->currentFPS);

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
            appl->mouseOverUI = true;

        draw_menu_button(appl->icons.menu_burger, darkRed, LIST_RUNS);
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
          .border = {.color = darkAqua, .width = {.betweenChildren = 2}},
          .cornerRadius = CORNER_RADIUS}) {
        if (Clay_Hovered())
            appl->mouseOverUI = true;

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
                appl->mouseOverUI = true;
            draw_filter_header();
            draw_filter(FILTER_DISTANCE, &collection->filters);
            draw_filter(FILTER_DURATION, &collection->filters);
            draw_filter(FILTER_PACE, &collection->filters);
            draw_filter(FILTER_DATE, &collection->filters);
            draw_filter(FILTER_UPHILL, &collection->filters);
            draw_filter(FILTER_DOWNHILL, &collection->filters);
            draw_filter(FILTER_PEAK, &collection->filters);

            bool pre_showRuns = collection->filters.showRuns;
            bool pre_showHikes = collection->filters.showHikes;
            bool pre_showCycling = collection->filters.showCycling;
            bool pre_showOther = collection->filters.showOther;
            draw_type_filter_container(&collection->filters);
            // check if anything changed

            CLAY(CLAY_ID_LOCAL("FilterTracksState"), {.layout = {
                                                          .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW()},
                                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
                draw_clay_text(collection->total_visible_tracks_str, 16, fg, CLAY_TEXT_ALIGN_CENTER);
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
                draw_clay_text("Calculate Heat", 16, darkAqua, CLAY_TEXT_ALIGN_CENTER);
            }

            CLAY(CLAY_ID("DisplayFilteredButton"), {.layout = {
                                                        .padding = CLAY_PADDING_ALL(GAPS),
                                                        .sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIT()},
                                                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                    .backgroundColor = Clay_Hovered() ? bg_l : bg_d,
                                                    .cornerRadius = CORNER_RADIUS}) {
                Clay_OnHover(clicked_show_filtered_tracks, (intptr_t)&collection->track_tile_cache);
                draw_clay_text("Show Filtered Tracks", 16, darkAqua, CLAY_TEXT_ALIGN_CENTER);
            }
            // Re-filter only when a type toggle actually changed. This read
            // `==` joined by `||`, i.e. "if anything is unchanged", which was
            // true on nearly every frame.
            if (pre_showRuns != collection->filters.showRuns ||
                pre_showHikes != collection->filters.showHikes ||
                pre_showCycling != collection->filters.showCycling ||
                pre_showOther != collection->filters.showOther)
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
            appl->mouseOverUI = true;

        draw_run_list_header(collection);
        draw_run_list_scroll_container(collection, appl->window_height - (MENU_ICON_SIZE + 2 * GAPS) - (LIST_ENTRY_HEIGHT + 2 * GAPS) - (LIST_ENTRY_HEIGHT + 2 * GAPS) - (2 * SCREEN_BORDER_PADDING));
        draw_run_list_bottom(collection->total_visible_tracks_str, collection);
    }

    Clay_RenderCommandArray ui_renderCommands = Clay_EndLayout();
    Clay_SDL2_Render(appl->renderer, ui_renderCommands, appl->fonts);
    // end UI
}
