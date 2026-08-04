#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "background.h"
#include "colors.h"
#include "garmin.h"
#include "settings.h"

// The Garmin Connect panel: the account the activities are fetched from, the
// button that fetches them, and what the import is doing while it runs.
//
// The same two-phase rule the settings panel keeps: nothing in the layout
// writes to the model, a press only records what was pressed, and
// ui_garmin_update acts on it before the next layout.
//
// The password is the one thing here that is never saved. It lives in this file
// for as long as it takes to hand it to the import helper, which mints a token
// from it; every import after that runs off the token.

// --- Fields ---

#define NO_FIELD (-1)

typedef enum GarminField {
    FIELD_EMAIL = 0,
    FIELD_PASSWORD,
    FIELD_MFA,
    FIELD_COUNT
} GarminField;

// --- State ---

static int focused_field = NO_FIELD;
// What is being typed. The field itself is not written until the edit is
// committed, so an abandoned edit leaves nothing behind.
static char edit_buffer[SETTINGS_EDIT_MAX];
// What the field held when it was focused, so ESC can put it back.
static char focus_undo[SETTINGS_EDIT_MAX];

// Neither of these is a setting: the password is never written to disk, and the
// code is good for one login.
static char password[GARMIN_CREDENTIAL_MAX];
static char mfa_code[GARMIN_CREDENTIAL_MAX];
static bool show_password = false;

// Set by a hover callback, consumed by the next update.
static int pending_focus = NO_FIELD;
static bool pending_show_password = false;
static bool pending_login = false;
static bool pending_import = false;
static bool pending_save = false;

static float caret_phase = 0.0f;

static bool caret_shown(void) {
    return caret_phase < FILTER_CARET_BLINK_SECONDS / 2.0f;
}

// The code box is asked for only once Garmin has asked for one, so an account
// without two-factor set up never sees it.
static bool mfa_wanted(const struct application *appl) {
    return garmin_stage(&appl->garmin) == GARMIN_MFA_REQUIRED || mfa_code[0] != '\0';
}

// Whether a box has anything in it, counting what is still being typed.
//
// The button that reads these is drawn from this rather than from the committed
// value on its own: the click that presses it is also the click that commits
// the box it was typed into, so a button asking only about the committed value
// would be disabled on exactly the frame it was pressed -- and would need
// pressing twice.
static bool field_filled(GarminField field, const char *committed) {
    if (focused_field == (int)field)
        return edit_buffer[0] != '\0';
    return committed[0] != '\0';
}

// Where the helper writes, under whatever folder the library is read from -- so
// moving the library moves the imports with it.
static void import_dir(char *out, size_t size) {
    snprintf(out, size, "%s/%s", settings.gpx_dir, GARMIN_IMPORT_SUBDIR);
}

// --- Presses ---

static void clicked_field(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_focus = (int)user_data;
}

static void clicked_show_password(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_show_password = true;
}

static void clicked_login(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_login = true;
}

static void clicked_import(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_import = true;
}

// --- Reading and writing a field ---

static void field_text(GarminField field, char *out, size_t size) {
    switch (field) {
    case FIELD_EMAIL:
        snprintf(out, size, "%s", settings.garmin_email);
        break;
    case FIELD_PASSWORD:
        snprintf(out, size, "%s", password);
        break;
    case FIELD_MFA:
        snprintf(out, size, "%s", mfa_code);
        break;
    default:
        snprintf(out, size, "%s", "");
        break;
    }
}

// Writes `edit_buffer` into whatever the focused field stands for. Only the
// email is a setting; the other two are held here and nowhere else.
static void commit_field(GarminField field) {
    switch (field) {
    case FIELD_EMAIL:
        // The precision is spelled out rather than left to snprintf's own
        // truncation: the edit buffer is the longest a field can be and an
        // address is shorter, and without it the compiler warns about a
        // truncation that is exactly what is wanted here.
        snprintf(settings.garmin_email, sizeof(settings.garmin_email), "%.*s",
                 (int)sizeof(settings.garmin_email) - 1, edit_buffer);
        pending_save = true;
        break;
    case FIELD_PASSWORD:
        snprintf(password, sizeof(password), "%.*s",
                 (int)sizeof(password) - 1, edit_buffer);
        break;
    case FIELD_MFA:
        snprintf(mfa_code, sizeof(mfa_code), "%.*s",
                 (int)sizeof(mfa_code) - 1, edit_buffer);
        break;
    default:
        break;
    }
}

static void focus_field(GarminField field) {
    focused_field = field;
    field_text(field, edit_buffer, sizeof(edit_buffer));
    snprintf(focus_undo, sizeof(focus_undo), "%s", edit_buffer);
    caret_phase = 0.0f; // a caret that has just arrived is a caret that is shown

    // Every box here takes whole words rather than digits alone, so all three
    // want SDL assembling characters for them.
    SDL_StartTextInput();
}

static void commit_and_blur(void) {
    if (focused_field == NO_FIELD)
        return;

    commit_field((GarminField)focused_field);
    focused_field = NO_FIELD;
    SDL_StopTextInput();
}

// --- Input ---

bool ui_garmin_input_active(void) {
    return focused_field != NO_FIELD;
}

void ui_garmin_blur(void) {
    // Committing rather than discarding: clicking away from a field is the same
    // as pressing return on it, which is what makes an edit that was finished
    // but not confirmed still count.
    commit_and_blur();
}

static void buffer_append(const char *text) {
    size_t used = strlen(edit_buffer);
    snprintf(edit_buffer + used, sizeof(edit_buffer) - used, "%s", text);
}

void ui_garmin_handle_text(const char *text) {
    if (focused_field == NO_FIELD)
        return;

    buffer_append(text);
    caret_phase = 0.0f;
}

bool ui_garmin_handle_key(SDL_Keycode key, bool shift_held) {
    if (focused_field == NO_FIELD)
        return false;

    switch (key) {
    case SDLK_BACKSPACE: {
        size_t used = strlen(edit_buffer);
        if (used > 0)
            edit_buffer[used - 1] = '\0';
        caret_phase = 0.0f;
        return true;
    }

    case SDLK_DELETE:
        edit_buffer[0] = '\0';
        caret_phase = 0.0f;
        return true;

    case SDLK_ESCAPE:
        // Back to whatever the field held when it was focused, and out of it.
        // Nothing was written on the way in, so there is nothing to undo.
        snprintf(edit_buffer, sizeof(edit_buffer), "%s", focus_undo);
        focused_field = NO_FIELD;
        SDL_StopTextInput();
        return true;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        commit_and_blur();
        return true;

    case SDLK_TAB: {
        // Commits without leaving edit mode: the next field is focused rather
        // than nothing being.
        commit_field((GarminField)focused_field);
        int step = shift_held ? -1 : 1;
        focus_field((GarminField)((focused_field + step + FIELD_COUNT) % FIELD_COUNT));
        return true;
    }

    default:
        // Everything else is somebody else's. A key this panel has no use for
        // must not throw the field out of edit mode.
        return false;
    }
}

// --- Update ---

bool ui_garmin_update(struct application *appl, GpxCollection *collection) {
    // A field cannot stay focused on a panel that is on its way out: the keys
    // would be going to a box nobody can see.
    if (focused_field != NO_FIELD && anim_target(&ui.panels[PANEL_GARMIN]) < 0.5f)
        ui_garmin_blur();

    bool changed = false;

    if (pending_focus != NO_FIELD) {
        // Moving between two boxes commits the one being left, the same as
        // pressing return in it would.
        if (focused_field != NO_FIELD && focused_field != pending_focus)
            commit_and_blur();
        focus_field((GarminField)pending_focus);
        pending_focus = NO_FIELD;
        changed = true;
    }

    if (pending_show_password) {
        pending_show_password = false;
        show_password = !show_password;
        changed = true;
    }

    if (pending_login) {
        pending_login = false;
        if (garmin_start_login(&appl->garmin, settings.garmin_email, password, mfa_code))
            changed = true;
    }

    if (pending_import) {
        pending_import = false;
        char folder[GPX_PATH_MAX];
        import_dir(folder, sizeof(folder));
        if (garmin_start_sync(&appl->garmin, folder))
            changed = true;
    }

    if (garmin_collect(&appl->garmin)) {
        if (garmin_stage(&appl->garmin) == GARMIN_DONE) {
            // Neither is needed again: the helper has traded them for a token.
            memset(password, 0, sizeof(password));
            memset(mfa_code, 0, sizeof(mfa_code));
        }

        // Only an import puts files on disk, and only then is there anything to
        // read. A login, or an import that found nothing new, leaves the
        // library exactly as it was.
        if (garmin_imported(&appl->garmin) > 0)
            background_start_load(&appl->background, collection);

        changed = true;
    }

    if (pending_save) {
        pending_save = false;
        settings_save(&settings, SETTINGS_FILE);
        changed = true;
    }

    // The bar and the counts move on their own while the helper runs, and this
    // is what keeps the frames coming for them.
    if (garmin_busy(&appl->garmin))
        changed = true;

    if (focused_field != NO_FIELD) {
        caret_phase += appl->delta_time;
        if (caret_phase >= FILTER_CARET_BLINK_SECONDS)
            caret_phase -= FILTER_CARET_BLINK_SECONDS;
        changed = true;
    }

    return changed;
}

// --- Layout ---

static void draw_section_header(const char *title, int id) {
    CLAY(CLAY_IDI("GarminSection", id),
         {.layout = {.padding = {.left = GAPS, .right = GAPS},
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(SETTINGS_SECTION_HEADER_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(bg6),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        ui_draw_text_unwrapped(title, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_LEFT);
    }
}

static void draw_labelled_row(const char *label, int id) {
    CLAY(CLAY_IDI("GarminRowLabel", id),
         {.layout = {.padding = {.left = GAPS},
                     .sizing = {.width = CLAY_SIZING_FIXED(GARMIN_LABEL_WIDTH), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        ui_draw_text_unwrapped(label, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_LEFT);
    }
}

// One editable box. The password is drawn as its length in bullets, which is
// enough to tell an empty box from a full one without putting it on screen for
// whoever is behind you.
static void draw_field(GarminField field, bool masked) {
    bool editing = (focused_field == (int)field);

    char value[SETTINGS_EDIT_MAX];
    if (editing)
        snprintf(value, sizeof(value), "%s", edit_buffer);
    else
        field_text(field, value, sizeof(value));

    if (masked && !show_password && value[0] != '\0') {
        size_t length = strlen(value);
        if (length > sizeof(value) - 1)
            length = sizeof(value) - 1;
        memset(value, '*', length);
        value[length] = '\0';
    }

    bool unset = (value[0] == '\0');

    // Into this frame's arena before Clay is given it. Clay keeps the pointer
    // rather than copying the characters, and reads them during the render
    // after Clay_EndLayout -- by which time a local buffer here has gone.
    const char *text = unset ? "–" : ui_frame_printf("%s", value);

    CLAY(CLAY_IDI("GarminField", field),
         {
             .border = {.color = ui_fade(accent_color), .width = editing ? (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(2) : (Clay_BorderWidth)CLAY_BORDER_OUTSIDE(0)},
             .layout = {.padding = {.left = GAPS, .right = GAPS},
                        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
             .backgroundColor = ui_fade(Clay_Hovered() ? blue : dark_blue),
             .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS),
         }) {
        Clay_OnHover(clicked_field, (intptr_t)field);

        ui_draw_text_unwrapped(text, FILTER_TEXT_FONT_SIZE,
                               unset ? bg5 : bg1, CLAY_TEXT_ALIGN_CENTER);

        if (editing && caret_shown()) {
            CLAY(CLAY_IDI("GarminCaret", field),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(FILTER_CARET_WIDTH), .height = CLAY_SIZING_FIXED(FILTER_TEXT_FONT_SIZE)}},
                  .backgroundColor = ui_fade(bg1)}) {
            }
        }
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

    CLAY(CLAY_IDI("GarminButton", id),
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

// A line of explanation under a group of rows.
static void draw_caption(const char *text, Clay_Color color, int id) {
    CLAY(CLAY_IDI("GarminCaption", id),
         {.layout = {.padding = {.left = GAPS, .right = GAPS},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        ui_draw_text_unwrapped(text, FILTER_TEXT_FONT_SIZE, color, CLAY_TEXT_ALIGN_LEFT);
    }
}

#define GARMIN_ROW(id_index)                                                       \
    CLAY(CLAY_IDI("GarminRow", id_index),                                          \
         {.layout = {.childGap = GAPS,                                             \
                     .sizing = {.width = CLAY_SIZING_GROW(0),                      \
                                .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)}, \
                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})

// The bar the import fills, drawn the same way the progress panel's is: a
// full-width trough with the finished part over it.
static void draw_progress_bar(float fraction) {
    CLAY(CLAY_ID("GarminTrough"),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = CLAY_SIZING_FIXED(PROGRESS_BAR_HEIGHT)}},
          .backgroundColor = ui_fade(bg4),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        CLAY(CLAY_ID("GarminProgressFill"),
             {.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(fraction),
                                    .height = CLAY_SIZING_GROW(0)}},
              .backgroundColor = ui_fade(dark_aqua),
              .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        }
    }
}

static void draw_account_section(struct application *appl) {
    draw_section_header("Garmin account", 0);

    bool busy = garmin_busy(&appl->garmin);

    GARMIN_ROW(0) {
        draw_labelled_row("Email", 0);
        draw_field(FIELD_EMAIL, false);
    }

    GARMIN_ROW(1) {
        draw_labelled_row("Password", 1);
        draw_field(FIELD_PASSWORD, true);
    }

    if (mfa_wanted(appl)) {
        GARMIN_ROW(2) {
            draw_labelled_row("Code", 2);
            draw_field(FIELD_MFA, false);
        }
        draw_caption("Garmin sent a code. Enter it and log in again.", yellow, 0);
    }

    GARMIN_ROW(3) {
        draw_button(0, show_password ? "Hide password" : "Show password", false,
                    !busy, clicked_show_password);
        draw_button(1, "Log in", false,
                    !busy && field_filled(FIELD_EMAIL, settings.garmin_email) &&
                        field_filled(FIELD_PASSWORD, password),
                    clicked_login);
    }

    // Said plainly rather than left to be worked out from a greyed-out import
    // button: this is what the password is for, and once it is done it is done.
    if (garmin_have_session())
        draw_caption("Signed in. The password is not needed again.", grey2, 1);
    else
        draw_caption("Log in once; only the token is saved.", grey2, 1);
}

// What the panel says about the job that is running, or the one that just
// finished. Everything the helper reports arrives as one of these.
static const char *import_status(const struct application *appl) {
    const GarminJob *job = &appl->garmin;
    const char *message = garmin_message(job);

    switch (garmin_stage(job)) {
    case GARMIN_LOGGING_IN:
        return "Logging in…";
    case GARMIN_IMPORTING:
        // The total is unknown until the helper has finished asking Garmin what
        // there is, so until then there is nothing to count.
        if (garmin_total(job) <= 0)
            return "Looking for new activities…";
        return ui_frame_printf("Downloading %d of %d…", garmin_completed(job),
                               garmin_total(job));
    case GARMIN_DONE:
        // A login that worked is reported by the account section flipping to
        // "Signed in", so there is nothing for this line to add -- and every
        // answer it could give here would be about an import that never ran.
        if (garmin_was_login(job))
            return "";
        if (garmin_imported(job) > 0)
            return ui_frame_printf("Imported %d activities.", garmin_imported(job));
        if (message[0] != '\0')
            return ui_frame_printf("%s", message);
        return "Up to date — nothing new to import.";
    case GARMIN_MFA_REQUIRED:
        return "Garmin wants a verification code.";
    case GARMIN_FAILED:
        return message[0] != '\0' ? ui_frame_printf("%s", message) : "The import failed.";
    case GARMIN_IDLE:
        break;
    }
    return "";
}

static Clay_Color status_color(const struct application *appl) {
    switch (garmin_stage(&appl->garmin)) {
    case GARMIN_FAILED:
        return red;
    case GARMIN_MFA_REQUIRED:
        return yellow;
    case GARMIN_DONE:
        return aqua;
    default:
        return grey2;
    }
}

static void draw_import_section(struct application *appl) {
    draw_section_header("Import", 1);

    // Nothing to import without a token, and nothing to stack a second job on
    // top of. The library load that follows an import owns the collection while
    // it runs, so that counts as busy too.
    bool ready = garmin_have_session() &&
                 !garmin_busy(&appl->garmin) &&
                 !background_busy(&appl->background);

    GARMIN_ROW(4) {
        draw_button(2, "Import new activities", ready, ready, clicked_import);
    }

    if (garmin_busy(&appl->garmin) || garmin_total(&appl->garmin) > 0)
        draw_progress_bar(garmin_fraction(&appl->garmin));

    const char *status = import_status(appl);
    if (status[0] != '\0')
        draw_caption(status, status_color(appl), 2);

    char folder[GPX_PATH_MAX];
    import_dir(folder, sizeof(folder));
    draw_caption(ui_frame_printf("Into %s", folder), grey2, 3);
}

static void draw_garmin_header(void) {
    CLAY(CLAY_ID("GarminHeader"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(accent_color),
          .cornerRadius = {.topLeft = CORNER_RADIUS, .topRight = CORNER_RADIUS, .bottomLeft = 0, .bottomRight = 0}}) {
        ui_draw_text("Garmin Connect", HEADING_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

void ui_draw_garmin_panel(struct application *appl) {
    // Nothing to draw while it is shut. This has to be decided before the
    // CLAY() block rather than inside it: CLAY() expands to a for loop, and
    // returning out of the middle of one leaves the layout unclosed.
    if (anim_value(&ui.panels[PANEL_GARMIN]) <= 0.0f)
        return;

    CLAY(CLAY_ID("GarminPanel"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {.x = ui_panel_offset_x(PANEL_GARMIN, GARMIN_WIDTH),
                         .y = PANEL_ORIGIN_Y},
          },
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(GARMIN_WIDTH), .height = CLAY_SIZING_FIXED(PANEL_HEIGHT(appl->window_height))}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = ui_fade(bg),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_garmin_header();

        CLAY(CLAY_ID("GarminBody"),
             {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                         .childGap = GAPS,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            draw_account_section(appl);
            draw_import_section(appl);
        }
    }
}
