#include "ui_internal.h"

#include <math.h>

#include "background.h"
#include "colors.h"
#include "records.h"
#include "time_util.h"

// The records panel: a section per category down a scrolling column, each one a
// header over the handful of activities that hold it. Clicking a row selects
// the track and takes the map to it, the same as a run-list row.
//
// The table itself is records.c. Everything here is either layout or the state
// the layout reads, and none of it is written during a layout pass.

// --- State ---

static RecordTable table;

// Rebuilt on the next update rather than wherever the tracks changed: that is
// sometimes a worker's results landing and sometimes the middle of a layout
// pass, and neither is a place to walk the collection.
static bool table_dirty = true;

// The panel owns its scroll offset for the reason ui_runlist.c gives: Clay drops
// a scroll container's position after two updates without a layout, and this
// application lays out only on the frames it draws.
static float scroll_target = 0.0f;  // where the wheel has put it, in pixels
static float scroll_current = 0.0f; // what the layout draws
static float scroll_max = 0.0f;     // from the content and the viewport

// Set by a row click and consumed by the next update, which is when the
// selection and the map centre can both be moved.
static bool row_clicked = false;
static int clicked_track_id = -1;

// Unlike the run list and the statistics panel there is no free_scratch here,
// and its absence is deliberate rather than an oversight: a RecordTable is a
// fixed-size struct, so the panel never allocates and has nothing to give back.

void ui_records_invalidate(void) {
    table_dirty = true;
}

bool ui_records_take_click(int *track_id) {
    if (!row_clicked)
        return false;
    row_clicked = false;
    *track_id = clicked_track_id;
    return true;
}

// --- Geometry ---

// The height the sections come to, written from the same numbers Clay lays them
// out with -- the section's own header plus a row pitch each, since the
// column's childGap is what the pitch includes. Working it out here rather than
// measuring afterwards is what keeps the scroll limit and the rows in step.
static float content_height(void) {
    float height = 2 * GAPS; // the container's padding, top and bottom

    for (int category = 0; category < RECORD_CATEGORY_COUNT; category++) {
        // A category nothing qualifies for still shows its header and a line
        // saying so, so the panel is a list of eight categories from the first
        // frame rather than a short one that grows as the library does.
        int rows = table.list[category].count > 0 ? table.list[category].count : 1;
        height += RECORDS_SECTION_HEADER_HEIGHT + (float)rows * RECORDS_ROW_PITCH;
        if (category < RECORD_CATEGORY_COUNT - 1)
            height += RECORDS_SECTION_GAP;
    }

    return height;
}

// --- Scrolling ---

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

// How far the panel can be scrolled, given the room the body has. Re-asked
// every frame: a filter that empties half the categories shortens the content,
// and the view must not be left hanging past the end of it.
static void update_scroll_max(int viewport_h) {
    scroll_max = content_height() - (float)viewport_h;
    if (scroll_max < 0.0f)
        scroll_max = 0.0f;
    clamp_scroll();
}

bool ui_records_scroll_by_wheel(int mouse_x, int mouse_y, int detents) {
    // Last frame's box. The wheel is the pointer's, and the pointer was over
    // whatever was drawn last -- the same thing Clay resolves hover against.
    Clay_ElementData container = Clay_GetElementData(CLAY_ID("RecordsScrollContainer"));
    if (!container.found)
        return false;

    Clay_BoundingBox box = container.boundingBox;
    if (mouse_x < box.x || mouse_x >= box.x + box.width ||
        mouse_y < box.y || mouse_y >= box.y + box.height)
        return false;

    // The same feel as the run list, in the same units: SDL reports a wheel
    // turned away from the hand as positive, which is a move toward the top.
    scroll_target -= (float)detents * (RUN_LIST_SCROLL_ROWS_PER_STEP * RECORDS_ROW_PITCH);
    clamp_scroll();
    return true;
}

static bool scroll_tick(float dt) {
    if (scroll_current == scroll_target)
        return false;

    scroll_current = anim_approach(scroll_current, scroll_target, RUN_LIST_SCROLL_TAU, dt);
    // Arriving exactly is what lets the panel stop asking for frames; an
    // asymptote never would.
    if (fabsf(scroll_target - scroll_current) < 0.5f)
        scroll_current = scroll_target;
    return true;
}

// --- The update pass ---

bool ui_records_update(struct application *appl, GpxCollection *collection) {
    // While a job is in flight the collection belongs to its worker, and
    // records_build walks the array it may be reallocating.
    if (background_busy(&appl->background))
        return false;

    bool changed = false;

    // Nothing to compute for a panel that is neither open nor on its way there.
    bool showing = anim_value(&ui.panels[PANEL_RECORDS]) > 0.0f ||
                   anim_target(&ui.panels[PANEL_RECORDS]) > 0.5f;
    if (table_dirty && showing) {
        records_build(&table, collection);
        table_dirty = false;
        changed = true;
    }

    return scroll_tick(appl->delta_time) || changed;
}

// --- Text ---

static const char *entry_date_text(const RecordEntry *entry) {
    char date[TRACK_TEXT_MAX];
    char time_of_day[TRACK_TEXT_MAX];
    if (!utc_to_display_strings(entry->start_utc, date, sizeof(date),
                                time_of_day, sizeof(time_of_day)))
        return "";
    return ui_frame_printf("%s", date);
}

static const char *entry_value_text(RecordCategory category, const RecordEntry *entry) {
    char value[RECORD_VALUE_MAX];
    records_format_value(category, entry->value, value, sizeof(value));

    const char *unit = records_category_unit(category);
    if (unit[0] == '\0')
        return ui_frame_printf("%s", value);
    return ui_frame_printf("%s %s", value, unit);
}

static const char *entry_detail_text(RecordCategory category, const RecordEntry *entry) {
    char detail[RECORD_DETAIL_MAX];
    records_format_detail(category, entry, detail, sizeof(detail));
    return ui_frame_printf("%s", detail);
}

// --- Clicks ---

static void clicked_record_row(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        row_clicked = true;
        clicked_track_id = (int)userData;
    }
}

// --- Layout ---

static void draw_records_header(void) {
    CLAY(CLAY_ID("RecordsHeader"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(accent_color),
          .cornerRadius = {.topLeft = CORNER_RADIUS, .topRight = CORNER_RADIUS, .bottomLeft = 0, .bottomRight = 0}}) {
        ui_draw_text("Records", HEADING_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

// One cell of a row. Indexed by the row's place in the whole panel rather than
// with a _LOCAL id: _LOCAL seeds the hash from the parent of the element being
// opened, and every section's first row shares the shape of every other's.
static void draw_records_cell(int width, const char *text, uint16_t font_size,
                              Clay_Color color, Clay_TextAlignment align, int id) {
    CLAY(CLAY_IDI("RecordsCell", id),
         {.layout = {.padding = {.left = GAPS, .right = GAPS},
                     .sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_FIXED(RECORDS_ROW_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        ui_draw_text_unwrapped(text, font_size, color, align);
    }
}

static void draw_records_row(struct application *appl, RecordCategory category,
                             int rank, const RecordEntry *entry) {
    int row_index = (int)category * RECORDS_TOP_N + rank;
    bool selected = (entry->track_id == appl->selected_track);

    CLAY(CLAY_IDI("RecordsRow", row_index),
         {
             .border = {.color = ui_fade(border), .width = selected ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(2) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(RECORDS_ROW_HEIGHT)},
                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = ui_fade(Clay_Hovered() ? bg_l : bg_d),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(clicked_record_row, entry->track_id);

        int cell = row_index * RECORDS_COLUMN_COUNT;
        // The best is the record; the two under it are what it beat, so they
        // are lettered down rather than shouted equally.
        Clay_Color value_color = (rank == 0) ? yellow : fg_l;

        draw_records_cell(RECORDS_RANK_WIDTH, ui_frame_printf("%d.", rank + 1),
                          FILTER_TEXT_FONT_SIZE, grey2, CLAY_TEXT_ALIGN_LEFT, cell + 0);
        draw_records_cell(RECORDS_VALUE_WIDTH, entry_value_text(category, entry),
                          LABEL_FONT_SIZE, value_color, CLAY_TEXT_ALIGN_LEFT, cell + 1);
        draw_records_cell(RECORDS_DATE_WIDTH, entry_date_text(entry),
                          FILTER_TEXT_FONT_SIZE, fg_d, CLAY_TEXT_ALIGN_LEFT, cell + 2);
        draw_records_cell(RECORDS_DETAIL_WIDTH, entry_detail_text(category, entry),
                          FILTER_TEXT_FONT_SIZE, fg_d, CLAY_TEXT_ALIGN_RIGHT, cell + 3);
    }
}

static void draw_records_section(struct application *appl, RecordCategory category) {
    const RecordList *list = &table.list[category];

    CLAY(CLAY_IDI("RecordsSection", category),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIT()},
                     .childGap = GAPS,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        CLAY(CLAY_IDI("RecordsSectionHeader", category),
             {.layout = {.padding = {.left = GAPS, .right = GAPS},
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(RECORDS_SECTION_HEADER_HEIGHT)},
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .layoutDirection = CLAY_LEFT_TO_RIGHT},
              .backgroundColor = ui_fade(bg6),
              .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
            ui_draw_text_unwrapped(records_category_label(category), LABEL_FONT_SIZE,
                                   fg_l, CLAY_TEXT_ALIGN_LEFT);
        }

        for (int rank = 0; rank < list->count; rank++)
            draw_records_row(appl, category, rank, &list->entry[rank]);

        // Written as a condition rather than an early return: CLAY() is a loop
        // that closes its element on the way round, and returning out of the
        // middle of one leaves it open for whatever is drawn next.
        if (list->count == 0) {
            CLAY(CLAY_IDI("RecordsEmpty", category),
                 {.layout = {.padding = {.left = GAPS, .right = GAPS},
                             .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(RECORDS_ROW_HEIGHT)},
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
                ui_draw_text_unwrapped("Nothing that far yet", FILTER_TEXT_FONT_SIZE,
                                       grey1, CLAY_TEXT_ALIGN_LEFT);
            }
        }
    }
}

// The indicator beside the sections. Always emitted, thumb or no thumb: a table
// that shrinks to fit would otherwise change the panel's width as it went.
static void draw_records_scrollbar(int viewport_h) {
    float thumb_h = 0.0f;
    float thumb_y = 0.0f;

    if (scroll_max > 0.0f) {
        const float track_h = (float)(viewport_h - 2 * GAPS);
        thumb_h = track_h * (float)viewport_h / content_height();
        if (thumb_h < SCROLLBAR_MIN_THUMB)
            thumb_h = SCROLLBAR_MIN_THUMB;
        if (thumb_h > track_h)
            thumb_h = track_h;
        thumb_y = (scroll_current / scroll_max) * (track_h - thumb_h);
    }

    CLAY(CLAY_ID("RecordsScrollbar"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_FIXED(SCROLLBAR_GUTTER_WIDTH), .height = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        if (thumb_h > 0.0f) {
            // A spacer above the thumb is how a plain element puts a child at
            // an offset; nothing here has to float.
            CLAY(CLAY_ID("RecordsScrollbarSpacer"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(thumb_y)}}}) {
            }
            CLAY(CLAY_ID("RecordsScrollbarThumb"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(thumb_h)}},
                  .backgroundColor = ui_fade(grey0),
                  .cornerRadius = CLAY_CORNER_RADIUS(SCROLLBAR_WIDTH / 2)}) {
            }
        }
    }
}

// Every section is emitted, unlike the run list's rows. Eight categories of
// three rows is about a hundred and thirty elements against Clay's limit of
// thirty-two thousand, and the sections vary in height, which is exactly what
// makes virtualising them fiddly for nothing.
static void draw_records_body(struct application *appl, int height) {
    update_scroll_max(height);

    CLAY(CLAY_ID("RecordsBody"),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(height)},
                     .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
        CLAY(CLAY_ID("RecordsScrollContainer"),
             {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                         .childGap = RECORDS_SECTION_GAP,
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .clip = {.vertical = true, .childOffset = {0, -scroll_current}}}) {
            for (int category = 0; category < RECORD_CATEGORY_COUNT; category++)
                draw_records_section(appl, (RecordCategory)category);
        }

        draw_records_scrollbar(height);
    }
}

void ui_draw_records_panel(struct application *appl) {
    // Nothing to draw while it is shut. This has to be decided before the
    // CLAY() block rather than inside it: CLAY() expands to a for loop, and
    // returning out of the middle of one leaves the layout unclosed.
    if (anim_value(&ui.panels[PANEL_RECORDS]) <= 0.0f)
        return;

    int panel_h = PANEL_HEIGHT(appl->window_height);
    int body_height = panel_h - HEADER_HEIGHT - PANEL_FOOTER_HEIGHT;
    if (body_height < RECORDS_ROW_PITCH)
        body_height = RECORDS_ROW_PITCH;

    CLAY(CLAY_ID("RecordsPanel"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {.x = ui_panel_offset_x(PANEL_RECORDS, RECORDS_WIDTH),
                         .y = PANEL_ORIGIN_Y},
          },
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(RECORDS_WIDTH), .height = CLAY_SIZING_FIXED(panel_h)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = ui_fade(bg),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_records_header();
        draw_records_body(appl, body_height);
        ui_draw_panel_footer(PANEL_RECORDS);
    }
}
