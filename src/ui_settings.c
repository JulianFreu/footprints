#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "background.h"
#include "colors.h"
#include "map.h"
#include "settings.h"
#include "tracks.h"

// The settings panel: the heat ramp's setpoints, the map provider and its key,
// the two numbers the heat is calculated with, the folder the library is read
// from, and the view the window opens at.
//
// Nothing in the layout writes to the model, the same rule the filter and
// statistics panels keep: a press records what was pressed and
// ui_settings_update acts on it before the next layout. Everything that
// commits also writes the settings file, which is small enough that there is
// nothing to be gained by batching and a crash to lose by it.

// --- Fields ---

#define NO_FIELD (-1)

// The editable boxes, in the order TAB walks them. The setpoints come first so
// their enumerators and their indices are the same number.
typedef enum SettingsField {
    FIELD_SETPOINT_0 = 0,
    FIELD_HEAT_RADIUS = HEAT_SETPOINT_COUNT,
    FIELD_POINT_SIZE,
    FIELD_API_KEY,
    FIELD_GPX_DIR,
    FIELD_COUNT
} SettingsField;

static bool field_is_text(SettingsField field) {
    return field == FIELD_API_KEY || field == FIELD_GPX_DIR;
}

// --- State ---

static int focused_field = NO_FIELD;
// What is being typed. The setting itself is not written until the edit is
// committed, so an abandoned edit leaves nothing behind.
static char edit_buffer[SETTINGS_EDIT_MAX];
// What the field held when it was focused, so ESC can put it back.
static char focus_undo[SETTINGS_EDIT_MAX];

// The key is a secret, so it is drawn masked unless this is on.
static bool show_api_key = false;

// Set by a hover callback, consumed by the next update. -1 and false are
// "nothing pressed".
static int pending_focus = NO_FIELD;
static int pending_provider = -1;
static bool pending_show_key = false;
static bool pending_show_profiler = false;
static bool pending_show_heat_tooltip = false;
static bool pending_clear_cache = false;
static bool pending_reload_library = false;
static bool pending_recalculate = false;
static bool pending_capture_view = false;
static bool pending_reset_view = false;
// Raised by a committed edit and consumed by the next update. Committing
// happens on a keystroke or a click, neither of which has the collection whose
// tiles have to be dropped, so what the edit invalidated is recorded here and
// acted on where that pointer is to hand.
static bool pending_drop_heat_tiles = false;
static bool pending_save = false;

// The radius the heat on screen was calculated with. Changing the setting does
// nothing to the tracks until the calculation is run again, so the button that
// runs it lights up exactly while the two disagree.
static float applied_heat_radius = -1.0f;

static float caret_phase = 0.0f;

static bool caret_shown(void) {
    return caret_phase < FILTER_CARET_BLINK_SECONDS / 2.0f;
}

// --- Presses ---

static void clicked_field(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_focus = (int)user_data;
}

static void clicked_provider(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_provider = (int)user_data;
}

static void clicked_show_key(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_show_key = true;
}

static void clicked_show_profiler(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_show_profiler = true;
}

static void clicked_show_heat_tooltip(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_show_heat_tooltip = true;
}

static void clicked_clear_cache(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_clear_cache = true;
}

static void clicked_reload_library(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_reload_library = true;
}

static void clicked_recalculate(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_recalculate = true;
}

static void clicked_capture_view(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_capture_view = true;
}

static void clicked_reset_view(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_reset_view = true;
}

// --- Reading and writing a field ---

// What a field currently says, formatted into `out`. This is what the box
// shows when it is not being edited, and what an edit starts from.
static void field_text(SettingsField field, char *out, size_t size) {
    if (field >= FIELD_SETPOINT_0 && field < FIELD_SETPOINT_0 + HEAT_SETPOINT_COUNT) {
        snprintf(out, size, "%d", settings.heat_setpoint[field - FIELD_SETPOINT_0]);
        return;
    }

    switch (field) {
    case FIELD_HEAT_RADIUS:
        snprintf(out, size, "%d", (int)settings.heat_radius_pixels);
        break;
    case FIELD_POINT_SIZE:
        snprintf(out, size, "%d", settings.track_point_size);
        break;
    case FIELD_API_KEY:
        snprintf(out, size, "%s", settings.stadia_api_key);
        break;
    case FIELD_GPX_DIR:
        snprintf(out, size, "%s", settings.gpx_dir);
        break;
    default:
        snprintf(out, size, "%s", "");
        break;
    }
}

// What committing a field has to happen next. A setting is not just a number
// written down: the tiles it was baked into, the tracks it was read from or
// the map it was fetched with have to be told.
typedef enum SettingsEffect {
    EFFECT_NONE = 0,
    EFFECT_REDRAW_HEAT, // the rasterised heat tiles are stale
} SettingsEffect;

// Writes `edit_buffer` into the setting the focused field stands for. Anything
// unparseable leaves the setting alone -- an empty box is an abandoned edit,
// not a request for zero.
static SettingsEffect commit_field(SettingsField field) {
    if (field_is_text(field)) {
        if (field == FIELD_API_KEY) {
            // The precision is spelled out rather than left to snprintf's own
            // truncation: the edit buffer is the longest a field can be and the
            // key is shorter, and without it the compiler warns about a
            // truncation that is exactly what is wanted here.
            snprintf(settings.stadia_api_key, sizeof(settings.stadia_api_key), "%.*s",
                     (int)sizeof(settings.stadia_api_key) - 1, edit_buffer);
            map_set_api_key(settings.stadia_api_key);
        } else {
            // An empty folder would scan the working directory and find
            // nothing, which reads as an empty library rather than a bad path.
            if (edit_buffer[0] != '\0')
                snprintf(settings.gpx_dir, sizeof(settings.gpx_dir), "%s", edit_buffer);
        }
        return EFFECT_NONE;
    }

    if (edit_buffer[0] == '\0')
        return EFFECT_NONE;

    int value = atoi(edit_buffer);

    if (field >= FIELD_SETPOINT_0 && field < FIELD_SETPOINT_0 + HEAT_SETPOINT_COUNT) {
        int index = field - FIELD_SETPOINT_0;
        settings.heat_setpoint[index] = value;
        // Anchored on the one just typed, so it keeps the value it was given
        // and the others move out of its way. Repairing without an anchor
        // would answer "the hottest colour at 60 %" by moving the 60.
        settings_repair_setpoints_around(&settings, index);
        return EFFECT_REDRAW_HEAT;
    }

    switch (field) {
    case FIELD_HEAT_RADIUS:
        // Only the calculation reads this, and that is a button of its own --
        // the tiles on screen are still an honest picture of the last one.
        settings.heat_radius_pixels = (float)value;
        break;
    case FIELD_POINT_SIZE:
        settings.track_point_size = value;
        return EFFECT_REDRAW_HEAT;
    default:
        break;
    }

    return EFFECT_NONE;
}

static void focus_field(SettingsField field) {
    focused_field = field;
    field_text(field, edit_buffer, sizeof(edit_buffer));
    snprintf(focus_undo, sizeof(focus_undo), "%s", edit_buffer);
    caret_phase = 0.0f; // a caret that has just arrived is a caret that is shown

    // Only the text fields need SDL translating keystrokes into characters;
    // the number fields read the keycodes directly.
    if (field_is_text(field))
        SDL_StartTextInput();
    else
        SDL_StopTextInput();
}

// Records what a commit invalidated. Every site that commits goes through this
// rather than acting itself, so none of them has to be reached from somewhere
// that holds the collection.
static void note_effect(SettingsEffect effect) {
    if (effect == EFFECT_REDRAW_HEAT)
        pending_drop_heat_tiles = true;
    pending_save = true;
}

// Puts the edit into the settings and leaves the field.
static void commit_and_blur(void) {
    if (focused_field == NO_FIELD)
        return;

    note_effect(commit_field((SettingsField)focused_field));
    focused_field = NO_FIELD;
    SDL_StopTextInput();
}

// --- Input ---

bool ui_settings_input_active(void) {
    return focused_field != NO_FIELD;
}

void ui_settings_blur(void) {
    // Committing rather than discarding: clicking away from a field is the
    // same as pressing return on it, which is what makes an edit that was
    // finished but not confirmed still count.
    commit_and_blur();
}

// The digit a key stands for, or -1. The keypad counts as much as the row
// above the letters, the same way the filter fields treat it.
static int key_digit(SDL_Keycode key) {
    if (key >= SDLK_0 && key <= SDLK_9)
        return (int)(key - SDLK_0);
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
        return 1 + (int)(key - SDLK_KP_1);
    if (key == SDLK_KP_0)
        return 0;
    return -1;
}

static void buffer_append(const char *text) {
    size_t used = strlen(edit_buffer);
    snprintf(edit_buffer + used, sizeof(edit_buffer) - used, "%s", text);
}

static void buffer_backspace(void) {
    size_t used = strlen(edit_buffer);
    if (used > 0)
        edit_buffer[used - 1] = '\0';
}

void ui_settings_handle_text(const char *text) {
    // Only the text fields take characters. A number field that took them
    // would let "12e4" into a box the commit then reads as 12.
    if (focused_field == NO_FIELD || !field_is_text((SettingsField)focused_field))
        return;

    buffer_append(text);
    caret_phase = 0.0f;
}

bool ui_settings_handle_key(SDL_Keycode key, bool shift_held) {
    if (focused_field == NO_FIELD)
        return false;

    // A number field has no SDL_TEXTINPUT to work from, so its digits arrive
    // here. A text field's do not: they have already come through as text, and
    // taking them again would double every digit typed into the key.
    if (!field_is_text((SettingsField)focused_field)) {
        int typed = key_digit(key);
        if (typed >= 0) {
            char digit[2] = {(char)('0' + typed), '\0'};
            buffer_append(digit);
            caret_phase = 0.0f;
            return true;
        }
    }

    switch (key) {
    case SDLK_BACKSPACE:
        buffer_backspace();
        caret_phase = 0.0f;
        return true;

    case SDLK_DELETE:
        edit_buffer[0] = '\0';
        caret_phase = 0.0f;
        return true;

    case SDLK_ESCAPE: {
        // Back to whatever the field held when it was focused, and out of it.
        // Nothing was written on the way in, so there is nothing to undo.
        snprintf(edit_buffer, sizeof(edit_buffer), "%s", focus_undo);
        focused_field = NO_FIELD;
        SDL_StopTextInput();
        return true;
    }

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        commit_and_blur();
        return true;

    case SDLK_TAB: {
        // Commits without leaving edit mode: the next field is focused rather
        // than nothing being.
        note_effect(commit_field((SettingsField)focused_field));
        int step = shift_held ? -1 : 1;
        focus_field((SettingsField)((focused_field + step + FIELD_COUNT) % FIELD_COUNT));
        return true;
    }

    default:
        // Everything else is somebody else's. A key this panel has no use for
        // must not throw the field out of edit mode.
        return false;
    }
}

// --- Update ---

// Everything a provider switch has to do beyond writing the setting down: the
// tiles on screen are the old map's, and so is everything queued for download.
static void switch_provider(struct application *appl, MapProvider provider) {
    settings.provider = provider;
    map_set_provider(provider);
    map_flush_download_queue(&appl->download_queue);
    map_reset_pending();
    tile_cache_clear(&appl->tile_cache);
}

bool ui_settings_update(struct application *appl, GpxCollection *collection) {
    // What the current heat was calculated with is not known until there is
    // something to compare against.
    if (applied_heat_radius < 0.0f)
        applied_heat_radius = settings.heat_radius_pixels;

    // A field cannot stay focused on a panel that is on its way out: the keys
    // would be going to a box nobody can see.
    if (focused_field != NO_FIELD && anim_target(&ui.panels[PANEL_SETTINGS]) < 0.5f)
        ui_settings_blur();

    bool changed = false;

    if (pending_focus != NO_FIELD) {
        // Moving between two boxes commits the one being left, the same as
        // pressing return in it would.
        if (focused_field != NO_FIELD && focused_field != pending_focus)
            commit_and_blur();
        focus_field((SettingsField)pending_focus);
        pending_focus = NO_FIELD;
        changed = true;
    }

    if (pending_provider >= 0) {
        MapProvider provider = (MapProvider)pending_provider;
        pending_provider = -1;
        // A keyed provider without a key would draw nothing at all. The button
        // is already drawn as unavailable; this is what makes it so.
        if (!map_provider_needs_key(provider) || map_has_api_key()) {
            switch_provider(appl, provider);
            pending_save = true;
            changed = true;
        }
    }

    if (pending_show_key) {
        pending_show_key = false;
        show_api_key = !show_api_key;
        changed = true;
    }

    if (pending_show_profiler) {
        pending_show_profiler = false;
        settings.show_profiler = !settings.show_profiler;
        pending_save = true;
        changed = true;
    }

    if (pending_show_heat_tooltip) {
        pending_show_heat_tooltip = false;
        settings.show_heat_tooltip = !settings.show_heat_tooltip;
        pending_save = true;
        changed = true;
    }

    if (pending_clear_cache) {
        pending_clear_cache = false;
        // Only what is in video memory. The tiles on disk are the point of a
        // cache; a button that deleted them would be a button that spends
        // somebody's bandwidth.
        tile_cache_clear(&appl->tile_cache);
        map_reset_pending();
        changed = true;
    }

    if (pending_reload_library) {
        pending_reload_library = false;
        // Declines while a job is already running, which is also what stops a
        // second press stacking one.
        if (background_start_load(&appl->background, collection)) {
            applied_heat_radius = settings.heat_radius_pixels;
            changed = true;
        }
    }

    if (pending_recalculate) {
        pending_recalculate = false;
        if (background_start_heat(&appl->background, collection)) {
            applied_heat_radius = settings.heat_radius_pixels;
            changed = true;
        }
    }

    if (pending_capture_view) {
        pending_capture_view = false;
        settings.start_zoom = appl->zoom;
        settings.start_world_x = appl->world_x;
        settings.start_world_y = appl->world_y;
        pending_save = true;
        changed = true;
    }

    if (pending_reset_view) {
        pending_reset_view = false;
        settings.start_zoom = START_ZOOM;
        settings.start_world_x = START_WORLD_X;
        settings.start_world_y = START_WORLD_Y;
        pending_save = true;
        changed = true;
    }

    if (pending_drop_heat_tiles) {
        pending_drop_heat_tiles = false;
        // The rasterised tiles carry the old colours; the heat itself is
        // unchanged, so this is the cheap half of applying a filter rather
        // than a recalculation.
        tracks_invalidate_filtered_view(collection);
        changed = true;
    }

    if (pending_save) {
        pending_save = false;
        settings_save(&settings, SETTINGS_FILE);
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

// A label above a group of rows, so the sections read as separate groups.
static void draw_section_header(const char *title, int id) {
    CLAY(CLAY_IDI("SettingsSection", id),
         {.layout = {.padding = {.left = GAPS, .right = GAPS},
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(SETTINGS_SECTION_HEADER_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(bg6),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        ui_draw_text_unwrapped(title, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_LEFT);
    }
}

// One editable box. `width` of zero lets it share the row evenly with its
// siblings, which is what the six setpoints do.
static void draw_field(SettingsField field, int width, bool masked) {
    bool editing = (focused_field == (int)field);

    char value[SETTINGS_EDIT_MAX];
    if (editing)
        snprintf(value, sizeof(value), "%s", edit_buffer);
    else
        field_text(field, value, sizeof(value));

    // The key is a secret. Drawn as its length in bullets with the last few
    // characters showing, which is enough to tell one key from another without
    // putting it on screen for whoever is behind you.
    char masked_text[SETTINGS_EDIT_MAX];
    if (masked && !show_api_key && value[0] != '\0') {
        size_t length = strlen(value);
        size_t shown = length > 4 ? 4 : 0;
        size_t hidden = length - shown;
        if (hidden > sizeof(masked_text) - 8)
            hidden = sizeof(masked_text) - 8;

        size_t at = 0;
        for (size_t i = 0; i < hidden; i++)
            masked_text[at++] = '*';
        snprintf(masked_text + at, sizeof(masked_text) - at, "%s", value + (length - shown));
        snprintf(value, sizeof(value), "%s", masked_text);
    }

    bool unset = (value[0] == '\0');

    // Into this frame's arena before Clay is given it. Clay keeps the pointer
    // rather than copying the characters, and reads them during the render
    // after Clay_EndLayout -- by which time a local buffer here has gone.
    const char *text = unset ? "–" : ui_frame_printf("%s", value);

    CLAY(CLAY_IDI("SettingsField", field),
         {
             .border = {.color = ui_fade(accent_color), .width = editing ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(2) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.padding = {.left = GAPS, .right = GAPS},
                        .sizing = {.width = width > 0 ? CLAY_SIZING_FIXED(width) : CLAY_SIZING_GROW(0),
                                   .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = ui_fade(Clay_Hovered() ? blue : dark_blue),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(clicked_field, (intptr_t)field);

        ui_draw_text_unwrapped(text, FILTER_TEXT_FONT_SIZE,
                               unset ? bg5 : bg1, CLAY_TEXT_ALIGN_CENTER);

        if (editing && caret_shown()) {
            CLAY(CLAY_IDI("SettingsCaret", field),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(FILTER_CARET_WIDTH), .height = CLAY_SIZING_FIXED(FILTER_TEXT_FONT_SIZE)}},
                  .backgroundColor = ui_fade(bg1)}) {
            }
        }
    }
}

// A label on the left and whatever the row holds on the right.
static void draw_labelled_row(const char *label, int id) {
    CLAY(CLAY_IDI("SettingsRowLabel", id),
         {.layout = {.padding = {.left = GAPS},
                     .sizing = {.width = CLAY_SIZING_FIXED(SETTINGS_LABEL_WIDTH), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        ui_draw_text_unwrapped(label, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_LEFT);
    }
}

static void draw_button(int id, const char *label, bool highlighted, bool enabled,
                        void (*on_click)(Clay_ElementId, Clay_PointerData, intptr_t)) {
    Clay_Color face = highlighted ? dark_aqua : bg_d;
    Clay_Color hover = highlighted ? aqua : bg_l;
    Clay_Color text = highlighted ? bg : dark_aqua;

    if (!enabled) {
        face = bg2;
        hover = bg2;
        text = bg6;
    }

    CLAY(CLAY_IDI("SettingsButton", id),
         {
             .layout = {.padding = CLAY_PADDING_ALL(GAPS),
                        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
             .backgroundColor = ui_fade(Clay_Hovered() ? hover : face),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        // A disabled button takes no callback at all, so it cannot be pressed
        // rather than being pressed and ignored.
        if (enabled)
            Clay_OnHover(on_click, 0);
        ui_draw_text_unwrapped(label, LABEL_FONT_SIZE, text, CLAY_TEXT_ALIGN_CENTER);
    }
}

// A row of children laid left to right, which is what most of the panel is.
#define SETTINGS_ROW(id_index)                                                     \
    CLAY(CLAY_IDI("SettingsRow", id_index),                                        \
         {.layout = {.childGap = GAPS,                                             \
                     .sizing = {.width = CLAY_SIZING_GROW(0),                      \
                                .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)}, \
                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})

// The ramp as the setpoints currently shape it, drawn in slices across the
// panel: the left edge is a point with none of the maximum heat and the right
// edge one with all of it. It is the same table the map is drawn from, so what
// this shows is what the tiles will do.
static void draw_gradient_preview(void) {
    CLAY(CLAY_ID("SettingsGradient"),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(SETTINGS_GRADIENT_HEIGHT)},
                     .layoutDirection = CLAY_LEFT_TO_RIGHT},
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        for (int i = 0; i < SETTINGS_GRADIENT_STEPS; i++) {
            // The heat this slice stands for, as a count out of a maximum of
            // 100 -- which is what makes the position along the bar a
            // percentage of the maximum heat.
            int percent = i * 100 / (SETTINGS_GRADIENT_STEPS - 1);
            SDL_Color color = heat_ramp_color(heat_normalized(&settings, percent, 100));

            CLAY(CLAY_IDI("SettingsGradientStep", i),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = ui_fade(clay_from_sdl(color))}) {
            }
        }
    }
}

static void draw_heat_range_section(void) {
    draw_section_header("Heat colours", 0);

    draw_gradient_preview();

    // One field per setpoint, coldest on the left. Each says what share of the
    // maximum heat arrives at its own place along the ramp above.
    SETTINGS_ROW(0) {
        for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
            draw_field((SettingsField)(FIELD_SETPOINT_0 + i), 0, false);
    }

    // The caption shares its row with the toggle for the other way of reading a
    // heat: as the number itself, for whichever point the cursor is over. Here
    // rather than under "Heat calculation" because it changes how heat is
    // shown, not what it comes to -- and beside the caption rather than on a
    // row of its own because the sections below already reach past the bottom
    // of an 800px window, which another row would only make worse.
    SETTINGS_ROW(1) {
        CLAY(CLAY_ID("SettingsSetpointCaption"),
             {.layout = {.padding = {.left = GAPS},
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            ui_draw_text_unwrapped("% of max heat, cold to hot", FILTER_TEXT_FONT_SIZE,
                                   grey2, CLAY_TEXT_ALIGN_LEFT);
        }

        draw_button(7,
                    settings.show_heat_tooltip ? "Hide heat tooltip" : "Show heat tooltip",
                    settings.show_heat_tooltip, true, clicked_show_heat_tooltip);
    }
}

static void draw_map_section(struct application *appl) {
    draw_section_header("Map", 1);

    const bool have_key = map_has_api_key();
    const MapProvider current = map_current_provider();

    // Two to a row rather than five stacked, which is what the activity
    // toggles do and for the same reason: the room is worth more elsewhere.
    for (int row = 0; row * 2 < MAP_PROVIDER_COUNT; row++) {
        SETTINGS_ROW(10 + row) {
            for (int column = 0; column < 2; column++) {
                int index = row * 2 + column;
                if (index >= MAP_PROVIDER_COUNT)
                    break;

                MapProvider provider = (MapProvider)index;
                bool selected = (provider == current);
                bool usable = !map_provider_needs_key(provider) || have_key;

                Clay_Color face = selected ? dark_green : bg5;
                Clay_Color hover = selected ? green : bg8;
                Clay_Color text = bg;
                if (!usable) {
                    face = bg2;
                    hover = bg2;
                    text = bg6;
                }

                CLAY(CLAY_IDI("SettingsProvider", index),
                     {.layout = {.padding = {.left = GAPS, .right = GAPS},
                                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                      .backgroundColor = ui_fade(Clay_Hovered() ? hover : face),
                      .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
                    if (usable)
                        Clay_OnHover(clicked_provider, (intptr_t)index);
                    ui_draw_text_unwrapped(map_provider_label(provider), FILTER_TEXT_FONT_SIZE,
                                           text, CLAY_TEXT_ALIGN_CENTER);
                }
            }
        }
    }

    SETTINGS_ROW(20) {
        draw_labelled_row("Stadia API key", 0);
        draw_field(FIELD_API_KEY, 0, true);
    }

    SETTINGS_ROW(21) {
        draw_button(0, show_api_key ? "Hide key" : "Show key", false, true, clicked_show_key);
        draw_button(1, "Clear tile cache", false, true, clicked_clear_cache);
    }

    // Said plainly rather than left to be worked out from a map that will not
    // draw: the keyed providers are greyed out above and this is why.
    if (!have_key) {
        CLAY(CLAY_ID("SettingsKeyHint"),
             {.layout = {.padding = {.left = GAPS, .right = GAPS},
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            ui_draw_text_unwrapped("Stadia maps need a key", FILTER_TEXT_FONT_SIZE,
                                   grey2, CLAY_TEXT_ALIGN_LEFT);
        }
    }

    (void)appl;
}

static void draw_heat_calculation_section(struct application *appl) {
    draw_section_header("Heat calculation", 2);

    SETTINGS_ROW(30) {
        draw_labelled_row("Overlap radius", 1);
        draw_field(FIELD_HEAT_RADIUS, 0, false);
    }

    SETTINGS_ROW(31) {
        draw_labelled_row("Track point size", 2);
        draw_field(FIELD_POINT_SIZE, 0, false);
    }

    // Lit while the setting and the heat on screen disagree, which is exactly
    // when pressing it would change anything.
    bool stale = (applied_heat_radius >= 0.0f &&
                  applied_heat_radius != settings.heat_radius_pixels);

    SETTINGS_ROW(32) {
        draw_button(2, "Recalculate heat", stale,
                    !background_busy(&appl->background), clicked_recalculate);
    }
}

static void draw_library_section(struct application *appl) {
    draw_section_header("Library", 3);

    SETTINGS_ROW(40) {
        draw_labelled_row("GPX folder", 3);
        draw_field(FIELD_GPX_DIR, 0, false);
    }

    SETTINGS_ROW(41) {
        draw_button(3, "Reload library", false,
                    !background_busy(&appl->background), clicked_reload_library);
    }
}

static void draw_startup_view_section(void) {
    draw_section_header("Startup view", 4);

    SETTINGS_ROW(50) {
        draw_button(4, "Use current view", false, true, clicked_capture_view);
        draw_button(5, "Reset", false, true, clicked_reset_view);
    }

    CLAY(CLAY_ID("SettingsViewCaption"),
         {.layout = {.padding = {.left = GAPS, .right = GAPS},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        ui_draw_text_unwrapped(ui_frame_printf("Opens at zoom %d", settings.start_zoom),
                               FILTER_TEXT_FONT_SIZE, grey2, CLAY_TEXT_ALIGN_LEFT);
    }
}

// Where the frame-time overlay is switched on. Its own section rather than a
// row under one of the others: nothing here is about what the map shows.
static void draw_diagnostics_section(void) {
    draw_section_header("Diagnostics", 5);

    SETTINGS_ROW(60) {
        draw_button(6,
                    settings.show_profiler ? "Hide frame times" : "Show frame times",
                    settings.show_profiler, true, clicked_show_profiler);
    }
}

static void draw_settings_header(void) {
    CLAY(CLAY_ID("SettingsHeader"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(accent_color),
          .cornerRadius = {.topLeft = CORNER_RADIUS, .topRight = CORNER_RADIUS, .bottomLeft = 0, .bottomRight = 0}}) {
        ui_draw_text("Settings", HEADING_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

void ui_draw_settings_panel(struct application *appl) {
    // Nothing to draw while it is shut. This has to be decided before the
    // CLAY() block rather than inside it: CLAY() expands to a for loop, and
    // returning out of the middle of one leaves the layout unclosed.
    if (anim_value(&ui.panels[PANEL_SETTINGS]) <= 0.0f)
        return;

    CLAY(CLAY_ID("SettingsPanel"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {.x = ui_panel_offset_x(PANEL_SETTINGS, SETTINGS_WIDTH),
                         .y = PANEL_ORIGIN_Y},
          },
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(SETTINGS_WIDTH), .height = CLAY_SIZING_FIXED(PANEL_HEIGHT(appl->window_height))}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = ui_fade(bg),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_settings_header();

        CLAY(CLAY_ID("SettingsBody"),
             {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                         .childGap = GAPS,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            draw_heat_range_section();
            draw_map_section(appl);
            draw_heat_calculation_section(appl);
            draw_library_section(appl);
            draw_startup_view_section();
            draw_diagnostics_section();
        }
    }
}
