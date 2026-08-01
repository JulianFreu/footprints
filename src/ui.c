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
    .active_filter_id = NO_ACTIVE_FILTER};

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

void ui_draw_text(const char *string, uint16_t font_size, Clay_Color color, Clay_TextAlignment align) {
    Clay_String clay_string = {
        .chars = string,
        .length = strlen(string),
        .isStaticallyAllocated = false};
    CLAY_TEXT(clay_string, CLAY_TEXT_CONFIG({.fontSize = font_size, .textColor = color, .textAlignment = align}));
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
    appl->icons.elev_profile = NULL;
}

void ui_free_icons(struct application *appl) {
    SDL_Surface **surfaces[] = {
        &appl->icons.menu_burger, &appl->icons.statistics, &appl->icons.records,
        &appl->icons.settings, &appl->icons.date, &appl->icons.clock,
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
// One panel is open at a time: they all occupy the same corner, so a second one
// sliding in over the first would only hide it. Pressing the button of the
// panel already showing shuts it and leaves nothing open.
void ui_toggle_panel(MenuPanel panel) {
    bool opening = ui.open_panel != panel;

    for (int i = 0; i < PANEL_COUNT; i++)
        ui_panel_move(&ui.panels[i], (opening && i == panel) ? 1.0f : 0.0f);

    // The filter panel is a wing of the run list rather than a menu panel of
    // its own, so it goes wherever the run list goes.
    ui_panel_move(&ui.filters, anim_target(&ui.panels[PANEL_RUN_LIST]));

    ui.open_panel = opening ? panel : PANEL_NONE;
}

// What TAB has always done, now one of four.
void ui_toggle_run_list(void) {
    ui_toggle_panel(PANEL_RUN_LIST);
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
             .backgroundColor = Clay_Hovered() ? big_button_color : color,
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
          },
          .layout = {.childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(MENU_BAR_WIDTH), .height = CLAY_SIZING_FIT()}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_menu_button(appl->icons.menu_burger, "Runs", PANEL_RUN_LIST);
        draw_menu_button(appl->icons.statistics, "Stats", PANEL_STATISTICS);
        draw_menu_button(appl->icons.records, "Recs", PANEL_RECORDS);
        draw_menu_button(appl->icons.settings, "Set", PANEL_SETTINGS);
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
          .backgroundColor = bg,
          .border = {.color = dark_aqua, .width = CLAY_BORDER_OUTSIDE(2)},
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
              .backgroundColor = bg4,
              .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
            CLAY(CLAY_ID("ProgressFill"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(fraction),
                                        .height = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = dark_aqua,
                  .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
            }
        }
    }
}

// Input and animation, ahead of the layout that reads them. Clay resolves the
// pointer against the previous frame's boxes either way, so doing it here is
// where it always belonged: what changes the model happens on the event that
// caused it, and laying out the UI only reads.
void ui_update(struct application *appl, GpxCollection *collection) {
    if (!clay_memory.memory)
        return;

    float delta_time = appl->delta_time;

    int clicked_track;
    if (ui_runlist_take_click(&clicked_track)) {
        appl->selected_track = clicked_track;
        // Offset by a quarter of the window so the track lands clear of the
        // panels on the left rather than dead centre.
        appl->world_x = collection->tracks[clicked_track].mid_x -
                        (appl->window_width / 4) * map_world_per_pixel(appl);
        appl->world_y = collection->tracks[clicked_track].mid_y;
        app_request_redraw(appl);
    }

    Clay_SetPointerState(
        (Clay_Vector2){appl->mouse_x, appl->mouse_y}, appl->left_mouse_button_pressed);

    // Clay's own scroll containers are not used: it forgets a container's
    // position after two updates without a layout, and this application lays out
    // only on the frames it draws. The run list keeps its own offset instead.
    if (appl->wheel_y)
        ui_runlist_scroll_by_wheel(appl->mouse_x, appl->mouse_y, appl->wheel_y);

    // The sidebar follows the selection rather than a button, so what it should
    // be doing is decided here rather than by a click handler. Re-asking for
    // the target it already has costs nothing, which is what lets this be
    // written as a per-frame condition.
    ui_panel_move(&ui.right_sidebar, appl->selected_track > -1 ? 1.0f : 0.0f);

    // Each panel that moved is a reason to draw another frame. This is the
    // whole of what the frame loop needs to know about the UI's animations.
    bool moved = anim_tick(&ui.filters, delta_time);
    moved |= anim_tick(&ui.right_sidebar, delta_time);
    for (int panel = 0; panel < PANEL_COUNT; panel++)
        moved |= anim_tick(&ui.panels[panel], delta_time);
    moved |= ui_runlist_scroll_tick(delta_time);
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

    CLAY(CLAY_ID("Right sidebar"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {
                  .x = appl->window_width - anim_value(&ui.right_sidebar) * (SCREEN_BORDER_PADDING + SIDEBAR_WIDTH),
                  .y = SCREEN_BORDER_PADDING

              },
          },
          .layout = {.padding = CLAY_PADDING_ALL(GAPS), .childGap = GAPS, .sizing = {.width = CLAY_SIZING_FIXED(SIDEBAR_WIDTH), .height = appl->window_height - 2 * SCREEN_BORDER_PADDING}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = bg,
          .border = {.color = dark_aqua, .width = {.betweenChildren = 2}},
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

            // Reloaded by update_track_info_graphs when the selection changes.
            if (appl->icons.elev_profile) {
                CLAY(CLAY_ID("ElevationProfile"),
                     {.layout = {.sizing = {.width = CLAY_SIZING_GROW(200), .height = CLAY_SIZING_FIXED(100)}},
                      .image = {.imageData = appl->icons.elev_profile}}) {
                }
            }
        }
    }

    if (background_busy(&appl->background)) {
        draw_progress_panel(appl);
    } else {
        ui_draw_filter_panel(appl, collection);
        ui_draw_run_list(appl, collection);
        ui_draw_simple_panel(appl, PANEL_STATISTICS, "Statistics");
        ui_draw_simple_panel(appl, PANEL_RECORDS, "Records");
        ui_draw_simple_panel(appl, PANEL_SETTINGS, "Settings");
    }

    Clay_RenderCommandArray render_commands = Clay_EndLayout();
    clay_sdl_render(appl->renderer, render_commands, appl->fonts);
}
