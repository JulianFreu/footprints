#include "ui_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "filters.h"
#include "heat.h"
#include "track_sort.h"
#include "tracks.h"

#include "log.h"

#include "clay.h"
#include "clay_sdl.h"
#include "colors.h"

// Menu identifiers. Only LIST_RUNS is wired up so far; the rest name the
// buttons the menu is expected to grow.
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

static Clay_Arena clay_memory;

// Starts a panel opening or closing, whichever is the reverse of what it is
// currently doing.
void ui_animation_toggle(struct AnimationState *anim_obj) {
    bool should_open = !(anim_obj->opening || anim_obj->progress > 0.0f);
    anim_obj->opening = should_open;
    anim_obj->closing = !should_open;
}

void ui_draw_text(const char *string, uint16_t font_size, Clay_Color color, Clay_TextAlignment align) {
    Clay_String clay_string = {
        .chars = string,
        .length = strlen(string),
        .isStaticallyAllocated = false};
    CLAY_TEXT(clay_string, CLAY_TEXT_CONFIG({.fontSize = font_size, .textColor = color, .textAlignment = align}));
}

float get_delta_time(Uint32 last_frame_time) {
    Uint32 now = SDL_GetTicks();
    float deltaTime = ((float)now - (float)last_frame_time) / 1000.0f;
    return deltaTime;
}

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
    ui_animation_toggle(&ui.run_list);
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

void clay_draw_ui(struct application *appl, GpxCollection *collection) {
    if (!clay_memory.memory) {
        fprintf(stderr, "[CLAY] ERROR: clay_memory not initialized!\n");
        return;
    }
    // defaulting

    appl->mouse_over_ui = false;
    int clicked_track;
    if (ui_runlist_take_click(&clicked_track)) {
        appl->selected_track = clicked_track;
        appl->world_x = collection->tracks[clicked_track].mid_x - ((appl->window_width / 4) << (MAX_ZOOM - appl->zoom));
        appl->world_y = collection->tracks[clicked_track].mid_y;
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

    int list_offset_y = MENU_ICON_SIZE + 2 * SCREEN_BORDER_PADDING;
    ui_draw_filter_panel(appl, collection, list_offset_y);
    ui_draw_run_list(appl, collection, list_offset_y);

    Clay_RenderCommandArray render_commands = Clay_EndLayout();
    clay_sdl_render(appl->renderer, render_commands, appl->fonts);
    // end UI
}
