#include "ui_internal.h"

#include "background.h"
#include "colors.h"
#include "timelapse.h"
#include "tracks.h"

// The timelapse's controls: a bar along the bottom of the map with play, a
// trough to scrub along, the clock, the fast button and the way out. The clock
// itself is timelapse.c and the picture is tracks.c; this file holds whether
// the timelapse is on and turns presses into what they ask the clock to do.

// --- State ---

typedef enum TimelapseAction {
    ACTION_TOGGLE, // switch the timelapse on or off
    ACTION_PLAY,   // play or pause
    ACTION_RESTART,
    ACTION_FAST,
    ACTION_SKIP,
    ACTION_BACK,
    ACTION_FORWARD,
    ACTION_SCRUB, // a press on the trough, followed until the button is let go
} TimelapseAction;

static bool active = false;
static TimelapseClock playback;
// The visible set the span was measured from. A filter change mid-play
// stretches or shrinks the timeline without stopping it.
static int span_generation = -1;
// Switched on but not started yet: the span has to be measured first, and the
// tracks may still be with a worker.
static bool start_pending = false;
static bool scrubbing = false;

// Presses, from clicks during the layout and from keys, taken by the next
// update rather than acted on where they happen.
static unsigned pending = 0;

static void request(TimelapseAction action) {
    pending |= 1u << action;
}

bool ui_timelapse_active(void) {
    return active;
}

float ui_timelapse_time(void) {
    return timelapse_clock_time(&playback);
}

void ui_timelapse_request_toggle(void) {
    request(ACTION_TOGGLE);
}

bool ui_timelapse_handle_key(SDL_Keycode key) {
    if (!active)
        return false;

    switch (key) {
    case SDLK_SPACE:
        request(ACTION_PLAY);
        return true;
    case SDLK_ESCAPE:
        request(ACTION_TOGGLE);
        return true;
    case SDLK_LEFT:
        request(ACTION_BACK);
        return true;
    case SDLK_RIGHT:
        request(ACTION_FORWARD);
        return true;
    default:
        return false;
    }
}

// --- Update ---

static bool taken(unsigned actions, TimelapseAction action) {
    return (actions & (1u << action)) != 0;
}

// The clock to wherever the pointer is along the trough. Asked of last frame's
// box, which is what Clay hit-tests against too. The trough spans the time the
// typical track takes rather than the longest one, which can be days.
static void scrub_to_pointer(const struct application *appl) {
    Clay_ElementData trough = Clay_GetElementData(CLAY_ID("TimelapseTrough"));
    if (!trough.found || trough.boundingBox.width <= 0.0f)
        return;

    float share = ((float)appl->mouse_x - trough.boundingBox.x) / trough.boundingBox.width;
    if (share < 0.0f)
        share = 0.0f;
    if (share > 1.0f)
        share = 1.0f;
    timelapse_clock_seek(&playback, share * playback.typical_secs);
}

bool ui_timelapse_update(struct application *appl, GpxCollection *collection) {
    const unsigned actions = pending;
    pending = 0;
    bool changed = actions != 0;

    if (taken(actions, ACTION_TOGGLE)) {
        active = !active;
        timelapse_clock_pause(&playback);
        scrubbing = false;
        start_pending = active;
        span_generation = -1;
        if (!active)
            tracks_release_timelapse();
    }
    if (!active)
        return changed;

    // The tracks are the worker's while it runs, and the picture is not drawn
    // meanwhile, so the clock holds still until they are back.
    if (background_busy(&appl->background)) {
        timelapse_clock_pause(&playback);
        return changed;
    }

    if (collection->view_generation != span_generation) {
        float typical, end;
        timelapse_span(collection, &typical, &end);
        timelapse_clock_set_span(&playback, typical, end);
        span_generation = collection->view_generation;
        changed = true;
    }

    if (start_pending) {
        start_pending = false;
        timelapse_clock_seek(&playback, 0.0f);
        timelapse_clock_play(&playback);
    }

    const float now = timelapse_clock_time(&playback);
    const float step = TIMELAPSE_STEP_SHARE * playback.typical_secs;

    if (taken(actions, ACTION_PLAY)) {
        if (timelapse_clock_playing(&playback))
            timelapse_clock_pause(&playback);
        else
            timelapse_clock_play(&playback);
    }
    if (taken(actions, ACTION_RESTART)) {
        timelapse_clock_seek(&playback, 0.0f);
        timelapse_clock_play(&playback);
    }
    if (taken(actions, ACTION_FAST))
        timelapse_clock_set_fast(&playback, !playback.fast);
    if (taken(actions, ACTION_SKIP))
        timelapse_clock_seek(&playback, playback.end_secs);
    if (taken(actions, ACTION_BACK))
        timelapse_clock_seek(&playback, now - step);
    if (taken(actions, ACTION_FORWARD))
        timelapse_clock_seek(&playback, now + step);

    if (taken(actions, ACTION_SCRUB))
        scrubbing = true;
    if (scrubbing && !appl->left_mouse_button_pressed)
        scrubbing = false;
    if (scrubbing) {
        scrub_to_pointer(appl);
        changed = true;
    }

    changed |= timelapse_clock_tick(&playback, appl->delta_time);
    return changed;
}

// --- Layout ---

static void clicked_action(Clay_ElementId element_id, Clay_PointerData pointer, intptr_t action) {
    if (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        request((TimelapseAction)action);
}

static void draw_button(const char *label, TimelapseAction action, bool lit) {
    CLAY(CLAY_IDI("TimelapseButton", action),
         {.layout = {.padding = {2 * GAPS, 2 * GAPS, GAPS, GAPS},
                     .sizing = {.width = CLAY_SIZING_FIT(),
                                .height = CLAY_SIZING_FIXED(TIMELAPSE_BUTTON_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = Clay_Hovered() ? bg_l : (lit ? sel_green : bg_d),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        Clay_OnHover(clicked_action, action);
        ui_draw_text(label, LABEL_FONT_SIZE, lit ? aqua : dark_aqua, CLAY_TEXT_ALIGN_CENTER);
    }
}

static const char *clock_text(float secs) {
    const int whole = (int)secs;
    return ui_frame_printf("%d:%02d:%02d", whole / 3600, (whole % 3600) / 60, whole % 60);
}

void ui_draw_timelapse_bar(struct application *appl) {
    if (!active)
        return;

    const float now = timelapse_clock_time(&playback);
    // Full once the typical track has finished; the few still going after it
    // are what "Skip to end" is for.
    float share = playback.typical_secs > 0.0f ? now / playback.typical_secs : 1.0f;
    if (share > 1.0f)
        share = 1.0f;

    CLAY(CLAY_ID("TimelapseBar"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_BOTTOM,
                                        .parent = CLAY_ATTACH_POINT_CENTER_BOTTOM},
                       .offset = {.y = -TIMELAPSE_BAR_LIFT},
                       // Over a panel it overlaps, like the menu bar: the
                       // controls must stay in reach whatever is open.
                       .zIndex = 1},
          .layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .childGap = GAPS,
                     .sizing = {.width = CLAY_SIZING_FIT(), .height = CLAY_SIZING_FIT()},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .layoutDirection = CLAY_LEFT_TO_RIGHT},
          .backgroundColor = bg,
          .border = {.color = dark_aqua, .width = CLAY_BORDER_OUTSIDE(2)},
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_button(timelapse_clock_playing(&playback) ? "Pause" : "Play", ACTION_PLAY, false);
        draw_button("Restart", ACTION_RESTART, false);

        // Taller than the bar drawn in it, so it is not a thin line to aim at.
        CLAY(CLAY_ID("TimelapseTrough"),
             {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(TIMELAPSE_TROUGH_WIDTH),
                                    .height = CLAY_SIZING_FIXED(TIMELAPSE_BUTTON_HEIGHT)},
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            Clay_OnHover(clicked_action, ACTION_SCRUB);
            CLAY(CLAY_ID("TimelapseTroughBar"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_FIXED(TIMELAPSE_TROUGH_HEIGHT)}},
                  .backgroundColor = bg4,
                  .cornerRadius = CLAY_CORNER_RADIUS(TIMELAPSE_TROUGH_HEIGHT / 2)}) {
                CLAY(CLAY_ID("TimelapseTroughFill"),
                     {.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(share),
                                            .height = CLAY_SIZING_GROW(0)}},
                      .backgroundColor = dark_aqua,
                      .cornerRadius = CLAY_CORNER_RADIUS(TIMELAPSE_TROUGH_HEIGHT / 2)}) {
                }
            }
        }

        // A box of its own, so the bar does not change width as the digits do.
        CLAY(CLAY_ID("TimelapseClock"),
             {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(TIMELAPSE_CLOCK_WIDTH)},
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
            ui_draw_text_unwrapped(clock_text(now), LABEL_FONT_SIZE, fg, CLAY_TEXT_ALIGN_CENTER);
        }

        draw_button("3x", ACTION_FAST, playback.fast);
        draw_button("Skip to end", ACTION_SKIP, false);
        draw_button("Close", ACTION_TOGGLE, false);
    }
}
