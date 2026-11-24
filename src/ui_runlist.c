#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>

#include "colors.h"
#include "track_format.h"
#include "track_sort.h"
#include "tracks.h"

// The run list: its sortable header, its rows, the virtualised scroll container
// that emits only the rows actually on screen, and the indicator beside them.

// One row and the gap under it: the unit everything about the list is measured
// in, from which row is first on screen to how far a wheel detent moves.
#define ROW_PITCH (LIST_ENTRY_HEIGHT + GAPS)

// Set by a row click and consumed by the next layout pass, which is when the
// selection and the map centre can both be updated.
static bool row_clicked = false;
static int clicked_track_id = -1;

// The list owns its scroll offset rather than leaving it to Clay. Clay drops a
// scroll container's position as soon as two updates pass without a layout, and
// this application only lays out on the frames it draws -- so on an otherwise
// idle window a wheel detent kept landing on a container Clay had already
// forgotten, doing nothing and putting the list back at the top. Owning it here
// also gives the indicator and the smoothing something to read.
static float scroll_target = 0.0f;  // where the wheel has put the list, in pixels
static float scroll_current = 0.0f; // what the layout draws; eases toward the target
static float scroll_max = 0.0f;     // set by the layout, from the rows and the viewport

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

static void clamp_scroll(void) {
    if (scroll_target > scroll_max)
        scroll_target = scroll_max;
    if (scroll_target < 0.0f)
        scroll_target = 0.0f;
    if (scroll_current > scroll_max)
        scroll_current = scroll_max;
    if (scroll_current < 0.0f)
        scroll_current = 0.0f;
}

bool ui_runlist_scroll_by_wheel(int mouse_x, int mouse_y, int detents) {
    // Last frame's box. The wheel is the pointer's, and the pointer was over
    // whatever was drawn last -- which is the same thing Clay resolves hover
    // against.
    Clay_ElementData container = Clay_GetElementData(CLAY_ID("RunListScrollContainer"));
    if (!container.found)
        return false;

    Clay_BoundingBox box = container.boundingBox;
    if (mouse_x < box.x || mouse_x >= box.x + box.width ||
        mouse_y < box.y || mouse_y >= box.y + box.height)
        return false;

    // SDL reports a wheel turned away from the hand as positive, which is a
    // move toward the top of the list.
    scroll_target -= (float)detents * (RUN_LIST_SCROLL_ROWS_PER_STEP * ROW_PITCH);
    clamp_scroll();
    return true;
}

bool ui_runlist_scroll_tick(float dt) {
    if (scroll_current == scroll_target)
        return false;

    scroll_current = anim_approach(scroll_current, scroll_target, RUN_LIST_SCROLL_TAU, dt);
    // Arriving exactly is what lets the list stop asking for frames; an
    // asymptote never would.
    if (fabsf(scroll_target - scroll_current) < 0.5f)
        scroll_current = scroll_target;
    return true;
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
                                                               .backgroundColor = ui_fade(Clay_Hovered() ? accent_color_hl : accent_color),
                                                               .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered()) {
            collection->to_be_sorted_by = sort_type;
        }
        Clay_OnHover(clicked_list_headers, (intptr_t)collection);
        ui_draw_text(str, LABEL_FONT_SIZE, bg_d, CLAY_TEXT_ALIGN_CENTER);
        switch (sort_type) {
        case ATTR_DATE:
            ui_draw_text("[dd:mm:yyyy]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case ATTR_DISTANCE:
            ui_draw_text("[km]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case ATTR_PACE:
            ui_draw_text("[min/km]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case ATTR_DURATION:
            ui_draw_text("[hh:mm:ss]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case ATTR_UPHILL:
            ui_draw_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case ATTR_DOWNHILL:
            ui_draw_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        case ATTR_HIGHPOINT:
            ui_draw_text("[m]", 12, bg_d, CLAY_TEXT_ALIGN_CENTER);
            break;
        default:
            break;
        }
    }
}

static void draw_run_list_header(GpxCollection *collection) {
    CLAY(CLAY_ID("RunListHeader"), {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                                               .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)},
                                               .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                    .backgroundColor = ui_fade(accent_color),
                                    .cornerRadius = {
                                        .topLeft = CORNER_RADIUS,
                                        .topRight = CORNER_RADIUS,
                                        .bottomLeft = 0,
                                        .bottomRight = 0}}) {
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Type", ATTR_TYPE);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Date", ATTR_DATE);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Distance", ATTR_DISTANCE);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Pace", ATTR_PACE);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Duration", ATTR_DURATION);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Uphill", ATTR_UPHILL);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Downhill", ATTR_DOWNHILL);
        draw_run_list_header_attribute(collection, RUN_LIST_COLUMN_WIDTH, "Highest", ATTR_HIGHPOINT);
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
             .border = {.color = ui_fade(border), .width = (clicked_track_id == track->track_id) ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(2) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(LIST_ENTRY_HEIGHT)}, .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = ui_fade(Clay_Hovered() ? bg_l : bg_d),
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

// Stands in for the rows scrolled past, so the content is laid out where it
// would be if every row had been emitted. The container puts a childGap after
// the spacer, which is part of the pitch being replaced.
static void draw_run_list_spacer(int rows, int id) {
    if (rows <= 0)
        return;
    CLAY(CLAY_IDI_LOCAL("RunListSpacer", id),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(rows * ROW_PITCH - GAPS)}}}) {
    }
}

// How far the list can be scrolled and where the indicator sits, worked out
// together so the thumb cannot come to disagree with what the list does. The
// offset is clamped on the way past: a filter that shortens the list must not
// leave the view hanging past the end of it.
typedef struct ScrollMetrics {
    float thumb_y; // top of the thumb, from the top of the track it runs in
    float thumb_h;
} ScrollMetrics;

static ScrollMetrics update_scroll_metrics(int visible_count, int viewport_h) {
    // The rows and the container's padding: the height a fully drawn list has.
    float content_h = visible_count > 0
                          ? (float)(visible_count * ROW_PITCH - GAPS + 2 * GAPS)
                          : 0.0f;

    scroll_max = content_h - (float)viewport_h;
    if (scroll_max < 0.0f)
        scroll_max = 0.0f;
    clamp_scroll();

    ScrollMetrics metrics = {0};
    if (scroll_max <= 0.0f)
        return metrics; // the list fits; there is nothing to indicate

    const float track_h = (float)(viewport_h - 2 * GAPS);
    metrics.thumb_h = track_h * (float)viewport_h / content_h;
    if (metrics.thumb_h < SCROLLBAR_MIN_THUMB)
        metrics.thumb_h = SCROLLBAR_MIN_THUMB;
    if (metrics.thumb_h > track_h)
        metrics.thumb_h = track_h;
    metrics.thumb_y = (scroll_current / scroll_max) * (track_h - metrics.thumb_h);
    return metrics;
}

// Only the rows actually on screen are handed to Clay. Every emitted row costs
// eight text elements, and the renderer rasterises and uploads each one every
// frame, so a library of several hundred tracks was paying for thousands of
// glyph rasterisations per frame to draw the twenty-odd rows that are visible.
static void draw_run_list_scroll_container(GpxCollection *collection, int height,
                                           int visible_count) {
    CLAY(CLAY_ID("RunListScrollContainer"),
         {
             .layout = {
                 .padding = CLAY_PADDING_ALL(GAPS),
                 .childGap = GAPS,
                 .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                 .layoutDirection = CLAY_TOP_TO_BOTTOM},
             .clip = {.vertical = true, .childOffset = {0, -scroll_current}},
         }) {
        int first_row = (int)(scroll_current / ROW_PITCH);
        if (first_row < 0)
            first_row = 0;
        if (first_row > visible_count)
            first_row = visible_count;

        // One extra row at each end so a partially scrolled row is still drawn.
        int last_row = first_row + height / ROW_PITCH + 2;
        if (last_row > visible_count)
            last_row = visible_count;

        draw_run_list_spacer(first_row, 0);
        for (int row = first_row; row < last_row; row++)
            draw_run_list_entry(&collection->tracks[visible_rows[row]]);
        draw_run_list_spacer(visible_count - last_row, 1);
    }
}

// The gutter is always emitted, thumb or no thumb: a list that shrinks below a
// screenful would otherwise change the panel's width as it went.
static void draw_run_list_scrollbar(ScrollMetrics metrics) {
    CLAY(CLAY_ID("RunListScrollbar"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_FIXED(SCROLLBAR_GUTTER_WIDTH), .height = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        // A list that fits leaves the gutter empty. Written as a condition
        // rather than an early return: CLAY() is a loop that closes the element
        // on its way round, and returning out of the middle of one leaves it
        // open for everything drawn after it to be nested inside.
        if (metrics.thumb_h > 0.0f) {
            // A spacer above the thumb is how a plain element puts a child at an
            // offset; nothing here has to float.
            CLAY(CLAY_ID("RunListScrollbarSpacer"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(metrics.thumb_y)}}}) {
            }
            CLAY(CLAY_ID("RunListScrollbarThumb"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(metrics.thumb_h)}},
                  .backgroundColor = ui_fade(grey0),
                  .cornerRadius = CLAY_CORNER_RADIUS(SCROLLBAR_WIDTH / 2)}) {
            }
        }
    }
}

// The rows and the indicator beside them. The background lives here rather than
// on the scroll container, so the gutter is the same colour as what it sits next
// to without being clipped along with the rows.
static void draw_run_list_body(GpxCollection *collection, int height) {
    int visible_count = collect_visible_rows(collection);
    ScrollMetrics metrics = update_scroll_metrics(visible_count, height);

    CLAY(CLAY_ID("RunListBody"),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(height)},
                     .layoutDirection = CLAY_LEFT_TO_RIGHT},
          .backgroundColor = ui_fade(bg)}) {
        draw_run_list_scroll_container(collection, height, visible_count);
        draw_run_list_scrollbar(metrics);
    }
}

// Releases the row buffer kept between frames.
void ui_runlist_free_scratch(void) {
    free(visible_rows);
    visible_rows = NULL;
    visible_rows_capacity = 0;
}

void ui_draw_run_list(struct application *appl, GpxCollection *collection) {
    // The one place the height is divided up. The header and footer are fixed,
    // so the rows get whatever is left between them and the bottom of the
    // screen, and the panel is the sum of the three -- rather than the panel and
    // the rows each working out a height of their own and disagreeing.
    int body_height = PANEL_HEIGHT(appl->window_height) - HEADER_HEIGHT -
                      PANEL_FOOTER_HEIGHT;
    if (body_height < ROW_PITCH)
        body_height = ROW_PITCH;

    CLAY(CLAY_ID("RunsListMenu"),
         {
             .floating = {
                 .attachTo = CLAY_ATTACH_TO_ROOT,
                 .offset = {
                     .x = ui_panel_offset_x(PANEL_RUN_LIST, RUN_LIST_WIDTH),
                     .y = PANEL_ORIGIN_Y},
             },
             .layout = {.sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIT()}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
         }) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_run_list_header(collection);
        draw_run_list_body(collection, body_height);
        ui_draw_panel_footer(PANEL_RUN_LIST);
    }
}
