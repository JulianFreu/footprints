#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "background.h"
#include "colors.h"
#include "import_job.h"
#include "settings.h"

// The import panel: the accounts the activities are fetched from, the buttons
// that fetch them, and what each import is doing while it runs. Garmin Connect
// and Strava are the same four rows of sections stacked one after the other,
// since they are the same job with a different helper behind it.
//
// The same two-phase rule the settings panel keeps: nothing in the layout
// writes to the model, a press only records what was pressed, and
// ui_import_update acts on it before the next layout.
//
// The Garmin password and the Strava client secret are the two things here that
// are never saved. They live in this file for as long as it takes to hand them
// to a helper, which mints a token from them; every import after that runs off
// the token.

// --- Fields ---

#define NO_FIELD (-1)

typedef enum ImportField {
    FIELD_GARMIN_EMAIL = 0,
    FIELD_GARMIN_PASSWORD,
    FIELD_GARMIN_MFA,
    FIELD_STRAVA_CLIENT_ID,
    FIELD_STRAVA_SECRET,
    FIELD_COUNT
} ImportField;

// What a press asked for, decided in the layout and acted on by the next
// update. One at a time: a frame carries a single click.
#define NO_ACTION (-1)

typedef enum ImportAction {
    ACTION_SHOW_GARMIN_PASSWORD = 0,
    ACTION_SHOW_STRAVA_SECRET,
    ACTION_GARMIN_LOGIN,
    ACTION_GARMIN_IMPORT,
    ACTION_STRAVA_LOGIN,
    ACTION_STRAVA_IMPORT,
} ImportAction;

// --- State ---

static int focused_field = NO_FIELD;
// What is being typed. The field itself is not written until the edit is
// committed, so an abandoned edit leaves nothing behind.
static char edit_buffer[SETTINGS_EDIT_MAX];
// What the field held when it was focused, so ESC can put it back.
static char focus_undo[SETTINGS_EDIT_MAX];

// None of these is a setting: the password and the client secret are never
// written to disk, and the code is good for one login.
static char garmin_password[IMPORT_CREDENTIAL_MAX];
static char garmin_mfa[IMPORT_CREDENTIAL_MAX];
static char strava_secret[IMPORT_CREDENTIAL_MAX];
// One per section, so uncovering one secret to check it does not put the other
// on screen alongside it.
static bool show_garmin_password = false;
static bool show_strava_secret = false;

// Set by a hover callback, consumed by the next update.
static int pending_focus = NO_FIELD;
static int pending_action = NO_ACTION;
static bool pending_save = false;

static float caret_phase = 0.0f;

// The panel owns its scroll offset for the reason ui_runlist.c gives: Clay drops
// a scroll container's position after two updates without a layout, and this
// application lays out only on the frames it draws.
static float scroll_target = 0.0f;  // where the wheel has put it, in pixels
static float scroll_current = 0.0f; // what the layout draws
static float scroll_max = 0.0f;     // from the content and the viewport

static bool caret_shown(void) {
    return caret_phase < FILTER_CARET_BLINK_SECONDS / 2.0f;
}

// The code box is asked for only once Garmin has asked for one, so an account
// without two-factor set up never sees it.
static bool mfa_wanted(const struct application *appl) {
    return import_stage(&appl->garmin) == IMPORT_MFA_REQUIRED || garmin_mfa[0] != '\0';
}

// Whether a job has anything to draw a bar for: one that is running, or one
// that has finished and left its counts behind.
static bool bar_wanted(const ImportJob *job) {
    return import_busy(job) || import_total(job) > 0;
}

// Whether it has anything to say. A login that worked is reported by the
// account section flipping to "Signed in", and an idle job has never run.
static bool status_wanted(const ImportJob *job) {
    if (import_stage(job) == IMPORT_IDLE)
        return false;
    return !(import_stage(job) == IMPORT_DONE && import_was_login(job));
}

// Whether a box has anything in it, counting what is still being typed.
//
// The button that reads these is drawn from this rather than from the committed
// value on its own: the click that presses it is also the click that commits
// the box it was typed into, so a button asking only about the committed value
// would be disabled on exactly the frame it was pressed -- and would need
// pressing twice.
static bool field_filled(ImportField field, const char *committed) {
    if (focused_field == (int)field)
        return edit_buffer[0] != '\0';
    return committed[0] != '\0';
}

// Where a provider's helper writes, under whatever folder the library is read
// from -- so moving the library moves the imports with it.
static void import_dir(const ImportProvider *provider, char *out, size_t size) {
    snprintf(out, size, "%s/%s", settings.gpx_dir, provider->import_subdir);
}

// --- Presses ---

static void clicked_field(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_focus = (int)user_data;
}

static void clicked_action(Clay_ElementId id, Clay_PointerData pointer, intptr_t user_data) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_action = (int)user_data;
}

// --- Reading and writing a field ---

static void field_text(ImportField field, char *out, size_t size) {
    switch (field) {
    case FIELD_GARMIN_EMAIL:
        snprintf(out, size, "%s", settings.garmin_email);
        break;
    case FIELD_GARMIN_PASSWORD:
        snprintf(out, size, "%s", garmin_password);
        break;
    case FIELD_GARMIN_MFA:
        snprintf(out, size, "%s", garmin_mfa);
        break;
    case FIELD_STRAVA_CLIENT_ID:
        snprintf(out, size, "%s", settings.strava_client_id);
        break;
    case FIELD_STRAVA_SECRET:
        snprintf(out, size, "%s", strava_secret);
        break;
    default:
        snprintf(out, size, "%s", "");
        break;
    }
}

// Writes `edit_buffer` into whatever the focused field stands for. Only the
// email and the application id are settings; the rest are held here and nowhere
// else.
//
// The precision each is written with is spelled out rather than left to
// snprintf's own truncation: the edit buffer is the longest a field can be and
// every target here is shorter, and without it the compiler warns about a
// truncation that is exactly what is wanted.
static void commit_field(ImportField field) {
    switch (field) {
    case FIELD_GARMIN_EMAIL:
        snprintf(settings.garmin_email, sizeof(settings.garmin_email), "%.*s",
                 (int)sizeof(settings.garmin_email) - 1, edit_buffer);
        pending_save = true;
        break;
    case FIELD_GARMIN_PASSWORD:
        snprintf(garmin_password, sizeof(garmin_password), "%.*s",
                 (int)sizeof(garmin_password) - 1, edit_buffer);
        break;
    case FIELD_GARMIN_MFA:
        snprintf(garmin_mfa, sizeof(garmin_mfa), "%.*s",
                 (int)sizeof(garmin_mfa) - 1, edit_buffer);
        break;
    case FIELD_STRAVA_CLIENT_ID:
        snprintf(settings.strava_client_id, sizeof(settings.strava_client_id), "%.*s",
                 (int)sizeof(settings.strava_client_id) - 1, edit_buffer);
        pending_save = true;
        break;
    case FIELD_STRAVA_SECRET:
        snprintf(strava_secret, sizeof(strava_secret), "%.*s",
                 (int)sizeof(strava_secret) - 1, edit_buffer);
        break;
    default:
        break;
    }
}

static void focus_field(ImportField field) {
    focused_field = field;
    field_text(field, edit_buffer, sizeof(edit_buffer));
    snprintf(focus_undo, sizeof(focus_undo), "%s", edit_buffer);
    caret_phase = 0.0f; // a caret that has just arrived is a caret that is shown

    // Every box here takes whole words rather than digits alone, so all of them
    // want SDL assembling characters for them.
    SDL_StartTextInput();
}

static void commit_and_blur(void) {
    if (focused_field == NO_FIELD)
        return;

    commit_field((ImportField)focused_field);
    focused_field = NO_FIELD;
    SDL_StopTextInput();
}

// --- Input ---

bool ui_import_input_active(void) {
    return focused_field != NO_FIELD;
}

void ui_import_blur(void) {
    // Committing rather than discarding: clicking away from a field is the same
    // as pressing return on it, which is what makes an edit that was finished
    // but not confirmed still count.
    commit_and_blur();
}

static void buffer_append(const char *text) {
    size_t used = strlen(edit_buffer);
    snprintf(edit_buffer + used, sizeof(edit_buffer) - used, "%s", text);
}

void ui_import_handle_text(const char *text) {
    if (focused_field == NO_FIELD)
        return;

    buffer_append(text);
    caret_phase = 0.0f;
}

bool ui_import_handle_key(SDL_Keycode key, bool shift_held) {
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
        commit_field((ImportField)focused_field);
        int step = shift_held ? -1 : 1;
        focus_field((ImportField)((focused_field + step + FIELD_COUNT) % FIELD_COUNT));
        return true;
    }

    default:
        // Everything else is somebody else's. A key this panel has no use for
        // must not throw the field out of edit mode.
        return false;
    }
}

// --- Geometry and scrolling ---

// Adds one child of the body to the running total, counting the gap that
// precedes every child but the first. The body's children are the rows,
// headers and captions the sections below emit, all siblings under one childGap.
static void add_child(float *height, int *count, float child) {
    if ((*count)++ > 0)
        *height += GAPS;
    *height += child;
}

// The height the sections come to, written from the same numbers and the same
// conditions Clay lays them out with. Working it out here rather than measuring
// afterwards is what keeps the scroll limit and the rows in step.
static float content_height(const struct application *appl) {
    float height = 2 * GAPS; // the container's padding, top and bottom
    int count = 0;

    // Garmin account: header, email, password, the code box when it is wanted
    // and the line that explains it, the buttons, and the line under them.
    add_child(&height, &count, SETTINGS_SECTION_HEADER_HEIGHT);
    add_child(&height, &count, SETTINGS_ROW_HEIGHT);
    add_child(&height, &count, SETTINGS_ROW_HEIGHT);
    if (mfa_wanted(appl)) {
        add_child(&height, &count, SETTINGS_ROW_HEIGHT);
        add_child(&height, &count, SETTINGS_ROW_HEIGHT);
    }
    add_child(&height, &count, SETTINGS_ROW_HEIGHT);
    add_child(&height, &count, SETTINGS_ROW_HEIGHT);

    // Strava account: header, id, secret, the button, and the line under it.
    add_child(&height, &count, SETTINGS_SECTION_HEADER_HEIGHT);
    add_child(&height, &count, SETTINGS_ROW_HEIGHT);
    add_child(&height, &count, SETTINGS_ROW_HEIGHT);
    add_child(&height, &count, SETTINGS_ROW_HEIGHT);
    add_child(&height, &count, SETTINGS_ROW_HEIGHT);

    // One import section per provider: header, button, the bar once there is
    // something to fill it, the status line when there is one, and the folder.
    const ImportJob *jobs[] = {&appl->garmin, &appl->strava};
    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
        add_child(&height, &count, SETTINGS_SECTION_HEADER_HEIGHT);
        add_child(&height, &count, SETTINGS_ROW_HEIGHT);
        if (bar_wanted(jobs[i]))
            add_child(&height, &count, PROGRESS_BAR_HEIGHT);
        if (status_wanted(jobs[i]))
            add_child(&height, &count, SETTINGS_ROW_HEIGHT);
        add_child(&height, &count, SETTINGS_ROW_HEIGHT);
    }

    return height;
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

// How far the panel can be scrolled, given the room the body has. Re-asked
// every frame: a code box appearing or an import finishing changes the content,
// and the view must not be left hanging past the end of it.
static void update_scroll_max(const struct application *appl, int viewport_h) {
    scroll_max = content_height(appl) - (float)viewport_h;
    if (scroll_max < 0.0f)
        scroll_max = 0.0f;
    clamp_scroll();
}

bool ui_import_scroll_by_wheel(int mouse_x, int mouse_y, int detents) {
    // Last frame's box. The wheel is the pointer's, and the pointer was over
    // whatever was drawn last -- the same thing Clay resolves hover against.
    Clay_ElementData container = Clay_GetElementData(CLAY_ID("ImportScrollContainer"));
    if (!container.found)
        return false;

    Clay_BoundingBox box = container.boundingBox;
    if (mouse_x < box.x || mouse_x >= box.x + box.width ||
        mouse_y < box.y || mouse_y >= box.y + box.height)
        return false;

    // The same feel as the run list, in the same units: SDL reports a wheel
    // turned away from the hand as positive, which is a move toward the top.
    // A row and the gap under it, which is the pitch the body lays them out at.
    scroll_target -= (float)detents *
                     (RUN_LIST_SCROLL_ROWS_PER_STEP * (SETTINGS_ROW_HEIGHT + GAPS));
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

// --- Update ---

// The press the last layout recorded. Each of these is the one thing that frame
// asked for, so it is consumed whether or not the job it names can be started.
static bool apply_action(struct application *appl, int action) {
    char folder[GPX_PATH_MAX];

    switch (action) {
    case ACTION_SHOW_GARMIN_PASSWORD:
        show_garmin_password = !show_garmin_password;
        return true;
    case ACTION_SHOW_STRAVA_SECRET:
        show_strava_secret = !show_strava_secret;
        return true;
    case ACTION_GARMIN_LOGIN:
        return import_start_login(&appl->garmin, settings.garmin_email,
                                  garmin_password, garmin_mfa);
    case ACTION_STRAVA_LOGIN:
        // No third credential: Strava's second step happens in the browser
        // rather than in a box here.
        return import_start_login(&appl->strava, settings.strava_client_id,
                                  strava_secret, "");
    case ACTION_GARMIN_IMPORT:
        import_dir(&import_garmin, folder, sizeof(folder));
        return import_start_sync(&appl->garmin, folder);
    case ACTION_STRAVA_IMPORT:
        import_dir(&import_strava, folder, sizeof(folder));
        return import_start_sync(&appl->strava, folder);
    default:
        return false;
    }
}

// Joins a worker that has finished, and wipes what it has by then traded for a
// token. Only on the frame the result lands: after that the boxes are the
// user's again, and a password typed for the next attempt must survive.
static bool collect_job(ImportJob *job, char *secret, size_t secret_size,
                        char *extra, size_t extra_size) {
    if (!import_collect(job))
        return false;

    if (import_stage(job) == IMPORT_DONE && import_was_login(job)) {
        memset(secret, 0, secret_size);
        if (extra)
            memset(extra, 0, extra_size);
    }
    return true;
}

bool ui_import_update(struct application *appl, GpxCollection *collection) {
    // A field cannot stay focused on a panel that is on its way out: the keys
    // would be going to a box nobody can see.
    if (focused_field != NO_FIELD && anim_target(&ui.panels[PANEL_IMPORT]) < 0.5f)
        ui_import_blur();

    bool changed = false;

    if (pending_focus != NO_FIELD) {
        // Moving between two boxes commits the one being left, the same as
        // pressing return in it would.
        if (focused_field != NO_FIELD && focused_field != pending_focus)
            commit_and_blur();
        focus_field((ImportField)pending_focus);
        pending_focus = NO_FIELD;
        changed = true;
    }

    if (pending_action != NO_ACTION) {
        int action = pending_action;
        pending_action = NO_ACTION;
        changed |= apply_action(appl, action);
    }

    bool reload = false;

    if (collect_job(&appl->garmin, garmin_password, sizeof(garmin_password),
                    garmin_mfa, sizeof(garmin_mfa))) {
        reload |= import_imported(&appl->garmin) > 0;
        changed = true;
    }

    // Strava's login has no third credential to forget: the browser carries
    // that half of it.
    if (collect_job(&appl->strava, strava_secret, sizeof(strava_secret), NULL, 0)) {
        reload |= import_imported(&appl->strava) > 0;
        changed = true;
    }

    // Only an import puts files on disk, and only then is there anything to
    // read. A login, or an import that found nothing new, leaves the library
    // exactly as it was. Asked for once even if both providers landed together,
    // since one load reads the whole library.
    if (reload)
        background_start_load(&appl->background, collection);

    if (pending_save) {
        pending_save = false;
        settings_save(&settings, settings_file_path());
        changed = true;
    }

    // The bars and the counts move on their own while a helper runs, and this
    // is what keeps the frames coming for them.
    if (import_busy(&appl->garmin) || import_busy(&appl->strava))
        changed = true;

    if (focused_field != NO_FIELD) {
        caret_phase += appl->delta_time;
        if (caret_phase >= FILTER_CARET_BLINK_SECONDS)
            caret_phase -= FILTER_CARET_BLINK_SECONDS;
        changed = true;
    }

    return scroll_tick(appl->delta_time) || changed;
}

// --- Layout ---

static void draw_section_header(const char *title, int id) {
    CLAY(CLAY_IDI("ImportSection", id),
         {.layout = {.padding = {.left = GAPS, .right = GAPS},
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(SETTINGS_SECTION_HEADER_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(bg6),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        ui_draw_text_unwrapped(title, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_LEFT);
    }
}

static void draw_labelled_row(const char *label, int id) {
    CLAY(CLAY_IDI("ImportRowLabel", id),
         {.layout = {.padding = {.left = GAPS},
                     .sizing = {.width = CLAY_SIZING_FIXED(IMPORT_LABEL_WIDTH), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        ui_draw_text_unwrapped(label, LABEL_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_LEFT);
    }
}

// One editable box. A secret is drawn as its length in bullets unless its own
// section's button says otherwise, which is enough to tell an empty box from a
// full one without putting it on screen for whoever is behind you.
static void draw_field(ImportField field, bool masked) {
    bool editing = (focused_field == (int)field);
    bool shown = (field == FIELD_STRAVA_SECRET) ? show_strava_secret
                                                : show_garmin_password;

    char value[SETTINGS_EDIT_MAX];
    if (editing)
        snprintf(value, sizeof(value), "%s", edit_buffer);
    else
        field_text(field, value, sizeof(value));

    if (masked && !shown && value[0] != '\0') {
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

    CLAY(CLAY_IDI("ImportField", field),
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
            CLAY(CLAY_IDI("ImportCaret", field),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(FILTER_CARET_WIDTH), .height = CLAY_SIZING_FIXED(FILTER_TEXT_FONT_SIZE)}},
                  .backgroundColor = ui_fade(bg1)}) {
            }
        }
    }
}

static void draw_button(ImportAction action, const char *label, bool highlighted,
                        bool enabled) {
    Clay_Color face = highlighted ? dark_aqua : bg_d;
    Clay_Color hover = highlighted ? aqua : bg_l;
    Clay_Color text = highlighted ? bg : dark_aqua;

    if (!enabled) {
        face = bg2;
        hover = bg2;
        text = bg6;
    }

    CLAY(CLAY_IDI("ImportButton", action),
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
            Clay_OnHover(clicked_action, (intptr_t)action);
        ui_draw_text_unwrapped(label, LABEL_FONT_SIZE, text, CLAY_TEXT_ALIGN_CENTER);
    }
}

// A line of explanation under a group of rows.
static void draw_caption(const char *text, Clay_Color color, int id) {
    CLAY(CLAY_IDI("ImportCaption", id),
         {.layout = {.padding = {.left = GAPS, .right = GAPS},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
        ui_draw_text_unwrapped(text, FILTER_TEXT_FONT_SIZE, color, CLAY_TEXT_ALIGN_LEFT);
    }
}

#define IMPORT_ROW(id_index)                                                       \
    CLAY(CLAY_IDI("ImportRow", id_index),                                          \
         {.layout = {.childGap = GAPS,                                             \
                     .sizing = {.width = CLAY_SIZING_GROW(0),                      \
                                .height = CLAY_SIZING_FIXED(SETTINGS_ROW_HEIGHT)}, \
                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})

// The bar an import fills, drawn the same way the progress panel's is: a
// full-width trough with the finished part over it.
static void draw_progress_bar(float fraction, int id) {
    CLAY(CLAY_IDI("ImportTrough", id),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = CLAY_SIZING_FIXED(PROGRESS_BAR_HEIGHT)}},
          .backgroundColor = ui_fade(bg4),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        CLAY(CLAY_IDI("ImportProgressFill", id),
             {.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(fraction),
                                    .height = CLAY_SIZING_GROW(0)}},
              .backgroundColor = ui_fade(dark_aqua),
              .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        }
    }
}

static void draw_garmin_account(struct application *appl) {
    draw_section_header("Garmin account", 0);

    bool busy = import_busy(&appl->garmin);

    IMPORT_ROW(0) {
        draw_labelled_row("Email", 0);
        draw_field(FIELD_GARMIN_EMAIL, false);
    }

    IMPORT_ROW(1) {
        draw_labelled_row("Password", 1);
        draw_field(FIELD_GARMIN_PASSWORD, true);
    }

    if (mfa_wanted(appl)) {
        IMPORT_ROW(2) {
            draw_labelled_row("Code", 2);
            draw_field(FIELD_GARMIN_MFA, false);
        }
        draw_caption("Garmin sent a code. Enter it and log in again.", yellow, 0);
    }

    IMPORT_ROW(3) {
        draw_button(ACTION_SHOW_GARMIN_PASSWORD,
                    show_garmin_password ? "Hide password" : "Show password",
                    false, !busy);
        draw_button(ACTION_GARMIN_LOGIN, "Log in", false,
                    !busy && field_filled(FIELD_GARMIN_EMAIL, settings.garmin_email) &&
                        field_filled(FIELD_GARMIN_PASSWORD, garmin_password));
    }

    // Said plainly rather than left to be worked out from a greyed-out import
    // button: this is what the password is for, and once it is done it is done.
    if (import_have_session(&import_garmin))
        draw_caption("Signed in. The password is not needed again.", grey2, 1);
    else
        draw_caption("Log in once; only the token is saved.", grey2, 1);
}

static void draw_strava_account(struct application *appl) {
    draw_section_header("Strava application", 1);

    bool busy = import_busy(&appl->strava);

    IMPORT_ROW(4) {
        draw_labelled_row("Client ID", 4);
        draw_field(FIELD_STRAVA_CLIENT_ID, false);
    }

    IMPORT_ROW(5) {
        draw_labelled_row("Secret", 5);
        draw_field(FIELD_STRAVA_SECRET, true);
    }

    IMPORT_ROW(6) {
        draw_button(ACTION_SHOW_STRAVA_SECRET,
                    show_strava_secret ? "Hide secret" : "Show secret",
                    false, !busy);
        draw_button(ACTION_STRAVA_LOGIN, "Connect", false,
                    !busy &&
                        field_filled(FIELD_STRAVA_CLIENT_ID, settings.strava_client_id) &&
                        field_filled(FIELD_STRAVA_SECRET, strava_secret));
    }

    // Strava has no password to give an application, so the two boxes above are
    // one the user creates for themselves and the browser does the rest.
    if (import_have_session(&import_strava))
        draw_caption("Connected. Only the token is saved.", grey2, 2);
    else
        draw_caption("Create an app at strava.com/settings/api, domain localhost.",
                     grey2, 2);
}

// What the panel says about the job that is running, or the one that just
// finished. Everything a helper reports arrives as one of these. Asked only of
// a job status_wanted() says has something to say.
static const char *import_status(const ImportJob *job) {
    const char *message = import_message(job);

    switch (import_stage(job)) {
    case IMPORT_LOGGING_IN:
        // Whatever the helper last said, which is how the Strava login reports
        // that it is the browser's turn. Garmin's says nothing and falls back.
        if (message[0] != '\0')
            return ui_frame_printf("%s", message);
        return "Logging in…";
    case IMPORT_IMPORTING:
        // The total is unknown until the helper has finished asking what there
        // is, so until then there is nothing to count.
        if (import_total(job) <= 0)
            return "Looking for new activities…";
        return ui_frame_printf("Downloading %d of %d…", import_completed(job),
                               import_total(job));
    case IMPORT_DONE:
        if (import_imported(job) > 0)
            return ui_frame_printf("Imported %d activities.", import_imported(job));
        if (message[0] != '\0')
            return ui_frame_printf("%s", message);
        return "Up to date — nothing new to import.";
    case IMPORT_MFA_REQUIRED:
        return ui_frame_printf("%s wants a verification code.", job->provider->name);
    case IMPORT_FAILED:
        return message[0] != '\0' ? ui_frame_printf("%s", message) : "The import failed.";
    case IMPORT_IDLE:
        break;
    }
    return "";
}

static Clay_Color status_color(const ImportJob *job) {
    switch (import_stage(job)) {
    case IMPORT_FAILED:
        return red;
    case IMPORT_MFA_REQUIRED:
        return yellow;
    case IMPORT_DONE:
        return aqua;
    default:
        return grey2;
    }
}

// One provider's import: the button, the bar it fills and what it has to say.
// `id` numbers the elements apart from the other provider's.
static void draw_import_section(struct application *appl, ImportJob *job,
                                ImportAction action, int id) {
    draw_section_header(ui_frame_printf("Import from %s", job->provider->name), 2 + id);

    // Nothing to import without a token, and nothing to stack a second job on
    // top of -- either provider's, since both write into the library. The load
    // that follows an import owns the collection while it runs, so that counts
    // as busy too.
    bool ready = import_have_session(job->provider) &&
                 !import_busy(&appl->garmin) &&
                 !import_busy(&appl->strava) &&
                 !background_busy(&appl->background);

    IMPORT_ROW(7 + id) {
        draw_button(action, "Import new activities", ready, ready);
    }

    if (bar_wanted(job))
        draw_progress_bar(import_fraction(job), id);

    if (status_wanted(job))
        draw_caption(import_status(job), status_color(job), 3 + 2 * id);

    char folder[GPX_PATH_MAX];
    import_dir(job->provider, folder, sizeof(folder));
    draw_caption(ui_frame_printf("Into %s", folder), grey2, 4 + 2 * id);
}

static void draw_import_header(void) {
    CLAY(CLAY_ID("ImportHeader"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(accent_color),
          .cornerRadius = {.topLeft = CORNER_RADIUS, .topRight = CORNER_RADIUS, .bottomLeft = 0, .bottomRight = 0}}) {
        ui_draw_text("Import activities", HEADING_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

// The indicator beside the sections. Always emitted, scrollable or not: a panel
// that dropped it would change width as an import finished.
static void draw_import_scrollbar(const struct application *appl, int viewport_h) {
    float thumb_h = 0.0f;
    float thumb_y = 0.0f;

    if (scroll_max > 0.0f) {
        const float track_h = (float)(viewport_h - 2 * GAPS);
        thumb_h = track_h * (float)viewport_h / content_height(appl);
        if (thumb_h < SCROLLBAR_MIN_THUMB)
            thumb_h = SCROLLBAR_MIN_THUMB;
        if (thumb_h > track_h)
            thumb_h = track_h;
        thumb_y = (scroll_current / scroll_max) * (track_h - thumb_h);
    }

    CLAY(CLAY_ID("ImportScrollbar"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_FIXED(SCROLLBAR_GUTTER_WIDTH), .height = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        if (thumb_h > 0.0f) {
            // A spacer above the thumb is how a plain element puts a child at
            // an offset; nothing here has to float.
            CLAY(CLAY_ID("ImportScrollbarSpacer"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(thumb_y)}}}) {
            }
            CLAY(CLAY_ID("ImportScrollbarThumb"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(thumb_h)}},
                  .backgroundColor = ui_fade(grey0),
                  .cornerRadius = CLAY_CORNER_RADIUS(SCROLLBAR_WIDTH / 2)}) {
            }
        }
    }
}

static void draw_import_body(struct application *appl, int height) {
    update_scroll_max(appl, height);

    CLAY(CLAY_ID("ImportBody"),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(height)},
                     .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
        CLAY(CLAY_ID("ImportScrollContainer"),
             {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                         .childGap = GAPS,
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .clip = {.vertical = true, .childOffset = {0, -scroll_current}}}) {
            // Both accounts first and both imports after them, so the one-time
            // setup reads as one thing and the button pressed every time as
            // another.
            draw_garmin_account(appl);
            draw_strava_account(appl);
            draw_import_section(appl, &appl->garmin, ACTION_GARMIN_IMPORT, 0);
            draw_import_section(appl, &appl->strava, ACTION_STRAVA_IMPORT, 1);
        }

        draw_import_scrollbar(appl, height);
    }
}

void ui_draw_import_panel(struct application *appl) {
    // Nothing to draw while it is shut. This has to be decided before the
    // CLAY() block rather than inside it: CLAY() expands to a for loop, and
    // returning out of the middle of one leaves the layout unclosed.
    if (anim_value(&ui.panels[PANEL_IMPORT]) <= 0.0f)
        return;

    int panel_h = PANEL_HEIGHT(appl->window_height);
    int body_height = panel_h - HEADER_HEIGHT;
    if (body_height < SETTINGS_ROW_HEIGHT)
        body_height = SETTINGS_ROW_HEIGHT;

    CLAY(CLAY_ID("ImportPanel"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {.x = ui_panel_offset_x(PANEL_IMPORT, IMPORT_WIDTH),
                         .y = PANEL_ORIGIN_Y},
          },
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(IMPORT_WIDTH), .height = CLAY_SIZING_FIXED(panel_h)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = ui_fade(bg),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_import_header();
        draw_import_body(appl, body_height);
    }
}
