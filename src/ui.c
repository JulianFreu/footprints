#include "ui_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "filters.h"
#include "heat.h"
#include "map.h"
#include "background.h"
#include "track_format.h"
#include "track_sort.h"
#include "tracks.h"

#include "log.h"

#include "clay.h"
#include "clay_sdl.h"
#include "colors.h"

// Text drawn this frame.
//
// Clay stores the pointer to a string's characters rather than copying them,
// so anything handed to it has to stay alive until Clay_EndLayout and the
// render that follows. That is why the display strings used to be kept on
// GpxTrack. A bump allocator reset at the top of each frame gives them the
// lifetime they need without putting presentation in the domain type.
static char frame_text_arena[FRAME_TEXT_ARENA_BYTES];
static size_t frame_text_used = 0;

static void ui_frame_text_reset(void) {
    frame_text_used = 0;
}

// Hands out `size` bytes of this frame's text, or NULL once it is spent.
static char *frame_text_alloc(size_t size) {
    if (frame_text_used + size > sizeof(frame_text_arena))
        return NULL;

    char *slot = &frame_text_arena[frame_text_used];
    frame_text_used += size;
    return slot;
}

const char *ui_track_text(const GpxTrack *track, TrackText field) {
    char *slot = frame_text_alloc(TRACK_TEXT_MAX);
    if (!slot)
        return ""; // arena spent; draw nothing rather than scribble past it
    return track_format(track, field, slot, TRACK_TEXT_MAX);
}

// Formatted text with the lifetime Clay needs. A local buffer will not do:
// Clay keeps the pointer and reads it during the render that follows
// Clay_EndLayout, by which time the frame it lived in is gone.
const char *ui_frame_printf(const char *fmt, ...) {
    char *slot = frame_text_alloc(UI_FRAME_STRING_MAX);
    if (!slot)
        return "";

    va_list args;
    va_start(args, fmt);
    vsnprintf(slot, UI_FRAME_STRING_MAX, fmt, args);
    va_end(args);
    return slot;
}

UIState ui = {
    .right_sidebar = {0},
    .panels = {{0}},
    .open_panel = PANEL_NONE,
    .filters = {0},
    .filters_host = PANEL_RUN_LIST};

static Clay_Arena clay_memory;

// Slides a panel to open (1) or shut (0).
void ui_panel_move(Anim *panel, float target) {
    anim_to(panel, target, PANEL_ANIMATION_SECONDS, ANIM_EASE_IN_OUT);
}

// Reverses whatever the panel is doing. Asked of where it is headed rather
// than where it is: a panel a tenth of the way open is an opening panel, and
// toggling it should shut it. Deciding from its position instead is what used
// to make a second press during a close do nothing.
void ui_panel_toggle(Anim *panel) {
    ui_panel_move(panel, anim_target(panel) > 0.5f ? 0.0f : 1.0f);
}

// All four panels slide in from off the left edge to the same corner, so where
// one is depends only on how far along its anim is and how wide it is.
float ui_panel_offset_x(MenuPanel panel, int width) {
    return -width + anim_value(&ui.panels[panel]) * (PANEL_ORIGIN_X + width);
}

// Three of the four are a constant. The statistics panel is the exception: it
// stretches to the window, so it is also the only one with no room beside it
// for the filters -- and it gives that room back as they slide out, which is
// what lets the plot reflow into it again as they slide back in.
int ui_panel_width(const struct application *appl, MenuPanel panel) {
    switch (panel) {
    case PANEL_STATISTICS: {
        float wing = (ui.filters_host == PANEL_STATISTICS) ? anim_value(&ui.filters) : 0.0f;
        int width = STATS_PANEL_WIDTH(appl->window_width) -
                    (int)(wing * (FILTERS_WIDTH + GAPS));
        return width < STATS_PANEL_MIN_WIDTH ? STATS_PANEL_MIN_WIDTH : width;
    }
    case PANEL_RECORDS:
        return RECORDS_WIDTH;
    case PANEL_SETTINGS:
        return SETTINGS_WIDTH;
    default:
        return RUN_LIST_WIDTH;
    }
}

// How opaque what is being drawn right now should be. Clay has no notion of
// opacity -- not per element, not inherited, not global -- so a panel fading in
// is every colour inside it scaled on the way past. The panel being drawn sets
// this from its own anim and puts it back afterwards, which is what lets a
// whole subtree fade without anything inside it knowing that it is fading.
static float panel_fade = 1.0f;

void ui_fade_set(float alpha) {
    panel_fade = alpha;
}

Clay_Color ui_fade(Clay_Color color) {
    color.a *= panel_fade;
    return color;
}

static void draw_text(const char *string, uint16_t font_size, Clay_Color color,
                      Clay_TextAlignment align, Clay_TextElementConfigWrapMode wrap) {
    Clay_String clay_string = {
        .chars = string,
        .length = strlen(string),
        .isStaticallyAllocated = false};
    // Every string in the UI comes through here, so text needs no fading of its
    // own at the call sites.
    CLAY_TEXT(clay_string, CLAY_TEXT_CONFIG({.fontSize = font_size, .textColor = ui_fade(color), .textAlignment = align, .wrapMode = wrap}));
}

void ui_draw_text(const char *string, uint16_t font_size, Clay_Color color, Clay_TextAlignment align) {
    draw_text(string, font_size, color, align, CLAY_TEXT_WRAP_WORDS);
}

// The same, for a label deliberately drawn in a box narrower than itself: the
// statistics x axis puts one under every nth bar, and the room it needs is the
// empty cells either side of it rather than a second line it has no height for.
void ui_draw_text_unwrapped(const char *string, uint16_t font_size, Clay_Color color, Clay_TextAlignment align) {
    draw_text(string, font_size, color, align, CLAY_TEXT_WRAP_NONE);
}

static void clay_handle_error(Clay_ErrorData error) {
    char buffer[512];
    size_t len = error.errorText.length;
    if (len >= sizeof(buffer))
        len = sizeof(buffer) - 1;

    memcpy(buffer, error.errorText.chars, len);
    buffer[len] = '\0';

    fprintf(stderr, "Clay ERROR: %s\n", buffer);
}

void clay_init(struct application *appl) {
    Clay_SetMaxElementCount(32000);
    uint32_t clay_required_memory = Clay_MinMemorySize();

    clay_memory = (Clay_Arena){
        .memory = malloc(clay_required_memory),
        .capacity = clay_required_memory};

    Clay_Initialize(
        clay_memory,
        (Clay_Dimensions){.width = appl->window_width, .height = appl->window_height},
        (Clay_ErrorHandler){.errorHandlerFunction = clay_handle_error, .userData = 0});
    Clay_SetMeasureTextFunction(clay_sdl_measure_text, appl->fonts);
}

void clay_free_memory(void) {
    ui_runlist_free_scratch();
    ui_stats_free_scratch();
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

    // A menu icon that is missing is not fatal: load_icon says so and the
    // button falls back to its label.
    appl->icons.statistics = load_icon("resources/statistics.png");
    appl->icons.records = load_icon("resources/records.png");
    appl->icons.settings = load_icon("resources/settings.png");

    appl->icons.date = load_icon("resources/date.png");
    appl->icons.clock = load_icon("resources/clock.png");
    appl->icons.duration = load_icon("resources/duration.png");
    appl->icons.pace = load_icon("resources/pace.png");
    appl->icons.distance = load_icon("resources/distance.png");
    appl->icons.elev_up = load_icon("resources/up.png");
    appl->icons.elev_down = load_icon("resources/down.png");
    appl->icons.peak = load_icon("resources/peak.png");
}

// The static artwork only. The graph surfaces are made by tracks.c whenever the
// selection changes, and given back there.
void ui_free_icons(struct application *appl) {
    SDL_Surface **surfaces[] = {
        &appl->icons.menu_burger, &appl->icons.statistics, &appl->icons.records,
        &appl->icons.settings, &appl->icons.date, &appl->icons.clock,
        &appl->icons.duration, &appl->icons.pace, &appl->icons.distance,
        &appl->icons.elev_up, &appl->icons.elev_down, &appl->icons.peak};

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
// One panel is open at a time: they all occupy the same corner, so a second one
// sliding in over the first would only hide it. Pressing the button of the
// panel already showing shuts it and leaves nothing open.
void ui_invalidate_derived(void) {
    ui_stats_invalidate();
    ui_records_invalidate();
}

// The panels the filters narrow. Settings configures the application rather
// than reading the collection, so it is the one that gets no filter wing.
static bool panel_takes_filters(MenuPanel panel) {
    return panel == PANEL_RUN_LIST || panel == PANEL_STATISTICS ||
           panel == PANEL_RECORDS;
}

void ui_toggle_panel(MenuPanel panel) {
    // A panel may have been left stale while it was shut, so whatever is about
    // to slide in gets a fresh look at the tracks.
    ui_invalidate_derived();

    bool opening = ui.open_panel != panel;

    for (int i = 0; i < PANEL_COUNT; i++)
        ui_panel_move(&ui.panels[i], (opening && i == panel) ? 1.0f : 0.0f);

    // The filter panel is a wing of whichever panel reads the collection rather
    // than a menu panel of its own, so it goes wherever that one goes. The host
    // is left alone while it shuts, so the filters slide away beside the panel
    // they were flanking rather than jumping to another one on the way out.
    bool wing = opening && panel_takes_filters(panel);
    if (wing)
        ui.filters_host = panel;
    ui_panel_move(&ui.filters, wing ? 1.0f : 0.0f);

    ui.open_panel = opening ? panel : PANEL_NONE;
}

// What TAB has always done, now one of four.
void ui_toggle_run_list(void) {
    ui_toggle_panel(PANEL_RUN_LIST);
}

static void clicked_toggle_filter_view(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        ui_panel_toggle(&ui.filters);
}

// Indexed by the panel rather than given local ids: three panels draw this
// footer, and while one slides out over another two of them are laid out in the
// same frame -- ids they shared would be declared twice.
void ui_draw_panel_footer(MenuPanel panel) {
    CLAY(CLAY_IDI("PanelFooter", panel), {.layout = {
                                              .padding = CLAY_PADDING_ALL(GAPS),
                                              .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(PANEL_FOOTER_HEIGHT)},
                                              .childGap = GAPS,
                                              .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = ui_fade(bg6),
                                          .cornerRadius = {.bottomLeft = CORNER_RADIUS, .bottomRight = CORNER_RADIUS}}) {
        CLAY(CLAY_IDI("PanelFooterSpacer", panel), {.layout = {
                                                        .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()}}}) {
        }
        CLAY(CLAY_IDI("FilterOptionsButton", panel), {.layout = {
                                                          .padding = CLAY_PADDING_ALL(GAPS),
                                                          .sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIT()},
                                                          .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                      .backgroundColor = ui_fade(Clay_Hovered() ? bg_l : bg_d),
                                                      .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
            Clay_OnHover(clicked_toggle_filter_view, 0);
            ui_draw_text("Toggle Filter View", LABEL_FONT_SIZE, dark_aqua, CLAY_TEXT_ALIGN_CENTER);
        }
    }
}

static void clicked_menu_button(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        ui_toggle_panel((MenuPanel)userData);
}

// The button for one panel. `label` is drawn in place of an icon that failed to
// load, so a button is still worth pressing before its artwork exists.
static void draw_menu_button(SDL_Surface *icon, const char *label, MenuPanel panel) {
    Clay_Color color = accent_color;
    if (ui.open_panel == panel)
        color = accent_color_hl;

    CLAY(CLAY_IDI_LOCAL("MenuButton", panel),
         {
             .layout = MenuButtonLayout,
             .backgroundColor = ui_fade(Clay_Hovered() ? big_button_color : color),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(clicked_menu_button, panel);
        if (icon) {
            CLAY(CLAY_IDI_LOCAL("MenuButtonIcon", panel),
                 {.layout = {
                      .padding = CLAY_PADDING_ALL(GAPS),
                      .sizing = {.width = CLAY_SIZING_FIXED(32),
                                 .height = CLAY_SIZING_FIXED(32)}},
                  .image = icon}) {}
        } else {
            ui_draw_text(label, FILTER_TEXT_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
        }
    }
}

// The buttons down the left edge, one per panel, in the order of the enum.
static void draw_menu_bar(struct application *appl) {
    CLAY(CLAY_ID("MenuBar"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {.x = SCREEN_BORDER_PADDING, .y = SCREEN_BORDER_PADDING},
              // Clay sorts floating elements by z order before drawing them,
              // and everything else leaves this at 0. The buttons are what a
              // panel slides out from under, so they belong above it.
              .zIndex = 1,
          },
          .layout = {.childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(MENU_BAR_WIDTH), .height = CLAY_SIZING_FIT()}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_menu_button(appl->icons.menu_burger, "Runs", PANEL_RUN_LIST);
        draw_menu_button(appl->icons.statistics, "Stats", PANEL_STATISTICS);
        draw_menu_button(appl->icons.records, "Recs", PANEL_RECORDS);
        // No icon of its own yet, so it draws as its label -- which is what
        // every button here does until its artwork exists.
        draw_menu_button(NULL, "Grmn", PANEL_GARMIN);
        draw_menu_button(appl->icons.settings, "Set", PANEL_SETTINGS);
    }
}

static void draw_sidebar_track_info(SDL_Surface *icon, const char *value, const char *unit, int id) {
    CLAY(CLAY_IDI_LOCAL("SidebarAttribute", id),
         {
             .layout = {
                 .padding = CLAY_PADDING_ALL(SIDEBAR_ROW_PADDING),
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
            ui_draw_text(value, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
        }
        CLAY(CLAY_IDI_LOCAL("Unit", id),
             {
                 .layout = {
                     .sizing = {.width = CLAY_SIZING_FIXED(70), .height = CLAY_SIZING_GROW(0)},
                     .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                     .padding = {.left = GAPS}},
             }) {
            ui_draw_text(unit, 12, fg_l, CLAY_TEXT_ALIGN_CENTER);
        }
    }
}

// Where the pointer is over the sidebar's graphs, and which point of the
// selected track that works out to.
//
// Panel-local, and consumed both by the layout below -- for the readout -- and
// by the frame loop, which draws the dot on the map from it. Kept as a fraction
// as well as a point so that the line lands exactly under the cursor rather
// than on the nearest sample to it.
static bool graph_hover_active;
static float graph_hover_fraction; // 0..1 along the track's distance
static int graph_hover_point = -1;

bool ui_graph_hover_point(int *point_index) {
    if (!graph_hover_active)
        return false;
    *point_index = graph_hover_point;
    return true;
}

// Resolved against last frame's boxes, which is the same thing Clay resolves
// hover against -- the pointer was over whatever was drawn last. On the first
// frame a graph is drawn there is no box yet and nothing is hovered; nothing
// else follows from that.
static void update_graph_hover(struct application *appl, GpxCollection *collection) {
    const bool was_active = graph_hover_active;
    const int was_point = graph_hover_point;

    graph_hover_active = false;
    graph_hover_point = -1;

    if (appl->selected_track >= 0 && appl->selected_track < collection->total_tracks &&
        !background_busy(&appl->background)) {
        for (int kind = 0; kind < TRACK_SERIES_COUNT; kind++) {
            Clay_ElementData graph = Clay_GetElementData(CLAY_IDI("TrackGraph", kind));
            if (!graph.found)
                continue;

            Clay_BoundingBox box = graph.boundingBox;
            if (box.width <= 0.0f ||
                appl->mouse_x < box.x || appl->mouse_x >= box.x + box.width ||
                appl->mouse_y < box.y || appl->mouse_y >= box.y + box.height)
                continue;

            graph_hover_fraction = ((float)appl->mouse_x - box.x) / box.width;
            graph_hover_point = track_series_index_at_fraction(
                &collection->tracks[appl->selected_track], graph_hover_fraction);
            graph_hover_active = graph_hover_point >= 0;
            break; // the three sit side by side vertically; one can be hovered
        }
    }

    // The dot on the map moves with the point, so a frame is owed whenever it
    // changes.
    if (graph_hover_active != was_active || graph_hover_point != was_point)
        app_request_redraw(appl);
}

// The line through every graph at the hovered distance. Drawn after Clay's own
// render pass rather than as part of the layout: it belongs on top of the
// pictures that pass has just put down, and Clay has no primitive for a line.
static void draw_graph_cursor(struct application *appl) {
    if (!graph_hover_active)
        return;

    Clay_Color color = ui_fade(fg_l);
    SDL_SetRenderDrawBlendMode(appl->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(appl->renderer, (Uint8)color.r, (Uint8)color.g,
                           (Uint8)color.b, (Uint8)color.a);

    for (int kind = 0; kind < TRACK_SERIES_COUNT; kind++) {
        Clay_ElementData graph = Clay_GetElementData(CLAY_IDI("TrackGraph", kind));
        if (!graph.found)
            continue;

        Clay_BoundingBox box = graph.boundingBox;
        SDL_Rect line = {(int)(box.x + graph_hover_fraction * box.width), (int)box.y,
                         SIDEBAR_CURSOR_WIDTH, (int)box.height};
        SDL_RenderFillRect(appl->renderer, &line);
    }
}

// The graphs under the attribute rows, and the readout that follows the cursor
// across them. A graph the track carries nothing for is left out entirely, so a
// run recorded without a heart rate monitor shows two rather than an empty box.
static void draw_sidebar_graphs(struct application *appl, const GpxTrack *track) {
    const bool hovering = graph_hover_active && graph_hover_point < track->total_points;

    // A space rather than an empty string when nothing is hovered: the rows
    // keep their height, so the graphs do not jump as the pointer crosses them.
    const char *distance_text = " ";
    const char *time_text = " ";
    if (hovering) {
        const GpxPoint *point = &track->points[graph_hover_point];
        distance_text = ui_frame_printf("%.2f km", point->partial_distance / 1000.0f);
        if (!isnan(point->elapsed_secs)) {
            int seconds = (int)point->elapsed_secs;
            time_text = ui_frame_printf("%02d:%02d:%02d", seconds / 3600,
                                        (seconds % 3600) / 60, seconds % 60);
        }
    }

    CLAY(CLAY_ID("SidebarCursorRow"),
         {.layout = {.padding = {.left = SIDEBAR_ROW_PADDING, .right = SIDEBAR_ROW_PADDING},
                     .sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = CLAY_SIZING_FIXED(SIDEBAR_GRAPH_HEADER_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
        ui_draw_text(distance_text, FILTER_TEXT_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_LEFT);
        CLAY(CLAY_ID("SidebarCursorSpacer"),
             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        ui_draw_text(time_text, FILTER_TEXT_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_RIGHT);
    }

    for (int kind = 0; kind < TRACK_SERIES_COUNT; kind++) {
        if (!appl->icons.graphs[kind])
            continue;

        const TrackSeries *series = &appl->track_series[kind];
        const char *value_text = " ";
        if (hovering && graph_hover_point < series->count) {
            char value[TRACK_SERIES_TEXT_MAX];
            value_text = ui_frame_printf(
                "%s %s",
                track_series_format((TrackSeriesKind)kind, series->values[graph_hover_point],
                                    value, sizeof(value)),
                track_series_unit((TrackSeriesKind)kind));
        }

        CLAY(CLAY_IDI("TrackGraphSection", kind),
             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                    .height = CLAY_SIZING_GROW(0, SIDEBAR_GRAPH_HEADER_HEIGHT + TRACK_GRAPH_HEIGHT)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            CLAY(CLAY_IDI("TrackGraphHeader", kind),
                 {.layout = {.padding = {.left = SIDEBAR_ROW_PADDING, .right = SIDEBAR_ROW_PADDING},
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_FIXED(SIDEBAR_GRAPH_HEADER_HEIGHT)},
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                ui_draw_text(track_series_label((TrackSeriesKind)kind),
                             FILTER_TEXT_FONT_SIZE, fg_d, CLAY_TEXT_ALIGN_LEFT);
                CLAY(CLAY_IDI("TrackGraphHeaderSpacer", kind),
                     {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                ui_draw_text(value_text, FILTER_TEXT_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_RIGHT);
            }
            // Grows into whatever height is left over, up to the size the
            // picture was drawn at. A window too short for three graphs squashes
            // them rather than spilling them out of the panel, and the cursor is
            // placed by fraction of the box, so a squashed graph still lines up.
            CLAY(CLAY_IDI("TrackGraph", kind),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_GROW(0, TRACK_GRAPH_HEIGHT)}},
                  .image = {.imageData = appl->icons.graphs[kind]}}) {}
        }
    }
}

// Shown while a worker holds the collection. The window used to simply stop
// redrawing for the duration, so there was nothing to see and no way to tell
// the difference from a hang.
static void draw_progress_panel(struct application *appl) {
    BgStage stage = background_stage(&appl->background);
    float fraction = background_fraction(&appl->background);

    const char *label = ui_frame_printf("%s  %d%%", background_stage_label(stage),
                                        (int)(fraction * 100.0f));

    CLAY(CLAY_ID("ProgressPanel"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {.x = (float)appl->window_width / 2 - PROGRESS_PANEL_WIDTH / 2,
                         .y = (float)appl->window_height / 2 - PROGRESS_PANEL_HEIGHT / 2}},
          .layout = {.padding = CLAY_PADDING_ALL(2 * GAPS), .childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(PROGRESS_PANEL_WIDTH), .height = CLAY_SIZING_FIXED(PROGRESS_PANEL_HEIGHT)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = ui_fade(bg),
          .border = {.color = ui_fade(dark_aqua), .width = CLAY_BORDER_OUTSIDE(2)},
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        // The panel covers the map, so clicks on it must not fall through to
        // the map underneath.
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        ui_draw_text(label, LABEL_FONT_SIZE, fg, CLAY_TEXT_ALIGN_CENTER);

        // The bar: a full-width trough with the finished part drawn over it.
        CLAY(CLAY_ID("ProgressTrough"),
             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                    .height = CLAY_SIZING_FIXED(PROGRESS_BAR_HEIGHT)}},
              .backgroundColor = ui_fade(bg4),
              .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
            CLAY(CLAY_ID("ProgressFill"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(fraction),
                                        .height = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = ui_fade(dark_aqua),
                  .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
            }
        }
    }
}

// A row click from either of the two lists that have them. The bounds check is
// not paranoia: the records table holds a snapshot of track ids, and a rescan
// that found fewer files would otherwise index past the end of the array.
static void select_clicked_track(struct application *appl,
                                 GpxCollection *collection, int track_id) {
    if (track_id < 0 || track_id >= collection->total_tracks)
        return;

    appl->selected_track = track_id;
    // Offset by a quarter of the window so the track lands clear of the
    // panels on the left rather than dead centre.
    appl->world_x = collection->tracks[track_id].mid_x -
                    (appl->window_width / 4) * map_world_per_pixel(appl);
    appl->world_y = collection->tracks[track_id].mid_y;
    app_request_redraw(appl);
}

// Input and animation, ahead of the layout that reads them. Clay resolves the
// pointer against the previous frame's boxes either way, so doing it here is
// where it always belonged: what changes the model happens on the event that
// caused it, and laying out the UI only reads.
void ui_update(struct application *appl, GpxCollection *collection) {
    if (!clay_memory.memory)
        return;

    float delta_time = appl->delta_time;

    // Asked of each list in turn rather than through a short circuit: a click
    // the other one is holding has to be taken this frame too, not left pending.
    int clicked_track;
    if (ui_runlist_take_click(&clicked_track))
        select_clicked_track(appl, collection, clicked_track);
    if (ui_records_take_click(&clicked_track))
        select_clicked_track(appl, collection, clicked_track);

    Clay_SetPointerState(
        (Clay_Vector2){appl->mouse_x, appl->mouse_y}, appl->left_mouse_button_pressed);

    // Clay's own scroll containers are not used: it forgets a container's
    // position after two updates without a layout, and this application lays out
    // only on the frames it draws. The panels keep their own offsets instead.
    //
    // Each hit-tests its own box and declines the wheel if the pointer is
    // somewhere else, so this is an ordered offer rather than a decision about
    // which panel is open -- which matters on the frames where one is still
    // sliding out from under another.
    if (appl->wheel_y) {
        if (!ui_stats_pan_by_wheel(appl->mouse_x, appl->mouse_y, appl->wheel_y) &&
            !ui_records_scroll_by_wheel(appl->mouse_x, appl->mouse_y, appl->wheel_y))
            ui_runlist_scroll_by_wheel(appl->mouse_x, appl->mouse_y, appl->wheel_y);
    }

    // The sidebar follows the selection rather than a button, so what it should
    // be doing is decided here rather than by a click handler. Re-asking for
    // the target it already has costs nothing, which is what lets this be
    // written as a per-frame condition.
    ui_panel_move(&ui.right_sidebar, appl->selected_track > -1 ? 1.0f : 0.0f);

    update_graph_hover(appl, collection);

    // Icons fade with the panel they sit in, and an icon's alpha lives on its
    // surface: the renderer builds a texture from the surface every frame, and
    // SDL_CreateTextureFromSurface carries the surface's alpha and colour mod
    // over to it. Set here rather than in the layout, which only reads.
    Uint8 sidebar_alpha = (Uint8)(anim_value(&ui.right_sidebar) * 255.0f);
    SDL_Surface *sidebar_icons[] = {
        appl->icons.date, appl->icons.clock, appl->icons.duration,
        appl->icons.pace, appl->icons.distance, appl->icons.elev_up,
        appl->icons.elev_down, appl->icons.peak};

    for (size_t i = 0; i < sizeof(sidebar_icons) / sizeof(sidebar_icons[0]); i++) {
        if (sidebar_icons[i])
            SDL_SetSurfaceAlphaMod(sidebar_icons[i], sidebar_alpha);
    }

    // The graphs are surfaces handed to Clay the same way, so they fade with
    // the panel for the same reason. They come and go with the selection, which
    // is why they are not part of the array above.
    for (int kind = 0; kind < TRACK_SERIES_COUNT; kind++) {
        if (appl->icons.graphs[kind])
            SDL_SetSurfaceAlphaMod(appl->icons.graphs[kind], sidebar_alpha);
    }

    // Each panel that moved is a reason to draw another frame. This is the
    // whole of what the frame loop needs to know about the UI's animations.
    bool moved = anim_tick(&ui.filters, delta_time);
    moved |= anim_tick(&ui.right_sidebar, delta_time);
    for (int panel = 0; panel < PANEL_COUNT; panel++)
        moved |= anim_tick(&ui.panels[panel], delta_time);
    moved |= ui_runlist_scroll_tick(delta_time);
    moved |= ui_filters_update(appl, collection);
    moved |= ui_stats_update(appl, collection);
    moved |= ui_records_update(appl, collection);
    moved |= ui_settings_update(appl, collection);
    moved |= ui_garmin_update(appl, collection);
    if (moved)
        app_request_redraw(appl);
}

void clay_draw_ui(struct application *appl, GpxCollection *collection) {
    if (!clay_memory.memory) {
        fprintf(stderr, "[CLAY] ERROR: clay_memory not initialized!\n");
        return;
    }
    appl->mouse_over_ui = false;

    Clay_SetLayoutDimensions((Clay_Dimensions){
        .width = appl->window_width,
        .height = appl->window_height});

    ui_frame_text_reset();
    Clay_BeginLayout();

    draw_menu_bar(appl);

    // Every panel below fades in as it slides, by scaling the colours it draws
    // with. The fade is put back to 1 after each one so that whatever is drawn
    // next starts from opaque.
    ui_fade_set(anim_value(&ui.right_sidebar));
    CLAY(CLAY_ID("Right sidebar"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {
                  .x = appl->window_width - anim_value(&ui.right_sidebar) * (SCREEN_BORDER_PADDING + SIDEBAR_WIDTH),
                  .y = SCREEN_BORDER_PADDING

              },
          },
          .layout = {.padding = CLAY_PADDING_ALL(GAPS), .childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(SIDEBAR_WIDTH), .height = appl->window_height - 2 * SCREEN_BORDER_PADDING}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = ui_fade(bg),
          .border = {.color = ui_fade(dark_aqua), .width = {.betweenChildren = 2}},
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        if (appl->selected_track >= 0 && !background_busy(&appl->background)) {
            const GpxTrack *track = &collection->tracks[appl->selected_track];

            draw_sidebar_track_info(appl->icons.date, ui_track_text(track, TRACK_TEXT_DATE), "", 0);
            draw_sidebar_track_info(appl->icons.clock, ui_track_text(track, TRACK_TEXT_TIME), "", 1);
            draw_sidebar_track_info(appl->icons.duration, ui_track_text(track, TRACK_TEXT_DURATION), "h", 2);
            draw_sidebar_track_info(appl->icons.pace, ui_track_text(track, TRACK_TEXT_PACE), "min/km", 3);
            draw_sidebar_track_info(appl->icons.distance, ui_track_text(track, TRACK_TEXT_DISTANCE), "km", 4);
            draw_sidebar_track_info(appl->icons.elev_up, ui_track_text(track, TRACK_TEXT_ELEV_UP), "m", 5);
            draw_sidebar_track_info(appl->icons.elev_down, ui_track_text(track, TRACK_TEXT_ELEV_DOWN), "m", 6);
            draw_sidebar_track_info(appl->icons.peak, ui_track_text(track, TRACK_TEXT_HIGH_POINT), "m", 7);

            draw_sidebar_track_info(appl->icons.peak, activity_type_label(track->act_type), " ", 8);

            // Rebuilt by update_track_info_graphs when the selection changes.
            draw_sidebar_graphs(appl, track);
        }
    }

    ui_fade_set(1.0f);

    if (background_busy(&appl->background)) {
        draw_progress_panel(appl); // does not slide, so it does not fade
    } else {
        ui_fade_set(anim_value(&ui.filters));
        ui_draw_filter_panel(appl, collection);

        ui_fade_set(anim_value(&ui.panels[PANEL_RUN_LIST]));
        ui_draw_run_list(appl, collection);

        ui_fade_set(anim_value(&ui.panels[PANEL_STATISTICS]));
        ui_draw_statistics_panel(appl);

        ui_fade_set(anim_value(&ui.panels[PANEL_RECORDS]));
        ui_draw_records_panel(appl);

        ui_fade_set(anim_value(&ui.panels[PANEL_GARMIN]));
        ui_draw_garmin_panel(appl);

        ui_fade_set(anim_value(&ui.panels[PANEL_SETTINGS]));
        ui_draw_settings_panel(appl);

        ui_fade_set(1.0f);
    }

    Clay_RenderCommandArray render_commands = Clay_EndLayout();

    // Alpha is discarded unless the renderer is blending: SDL defaults to
    // SDL_BLENDMODE_NONE, and the vendored Clay renderer never sets a blend
    // mode of its own. Set from out here so that file stays untouched.
    SDL_SetRenderDrawBlendMode(appl->renderer, SDL_BLENDMODE_BLEND);
    clay_sdl_render(appl->renderer, render_commands, appl->fonts);

    // On top of what Clay has just drawn, and faded with the sidebar the way
    // everything inside it is.
    ui_fade_set(anim_value(&ui.right_sidebar));
    draw_graph_cursor(appl);
    ui_fade_set(1.0f);
}
