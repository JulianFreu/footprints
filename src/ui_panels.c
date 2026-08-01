#include "ui_internal.h"

#include "clay.h"
#include "colors.h"

// The panels that have a button but nothing in them yet: statistics, records
// and settings. All three are the same shape -- a titled header over an empty
// body -- so they share one drawer and differ only by which anim moves them and
// what the header says.

static void draw_simple_panel_header(const char *title) {
    CLAY(CLAY_ID_LOCAL("SimplePanelHeader"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .layoutDirection = CLAY_LEFT_TO_RIGHT},
          .backgroundColor = accent_color,
          .cornerRadius = {
              .topLeft = CORNER_RADIUS,
              .topRight = CORNER_RADIUS,
              .bottomLeft = 0,
              .bottomRight = 0}}) {
        ui_draw_text(title, HEADING_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

void ui_draw_simple_panel(struct application *appl, MenuPanel panel,
                          const char *title) {
    // Nothing to draw while it is shut. This has to be decided before the
    // CLAY() block rather than inside it: CLAY() expands to a for loop, and
    // returning out of the middle of one leaves the layout unclosed.
    if (anim_value(&ui.panels[panel]) <= 0.0f)
        return;

    CLAY(CLAY_IDI("SimplePanel", panel),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {.x = ui_panel_offset_x(panel, PANEL_WIDTH),
                         .y = PANEL_ORIGIN_Y},
          },
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(PANEL_WIDTH), .height = CLAY_SIZING_FIXED(PANEL_HEIGHT(appl->window_height))}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_simple_panel_header(title);

        // The body, waiting to be filled.
        CLAY(CLAY_IDI("SimplePanelBody", panel),
             {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                         .childGap = GAPS,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        }
    }
}
