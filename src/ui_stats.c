#include "ui_internal.h"

#include <math.h>

#include "background.h"
#include "colors.h"
#include "stats.h"
#include "time_util.h"

#include "clay.h"

// The statistics panel: a bar per period across the window, one or two metrics
// at a time with an axis each, the buttons that choose them, and the pan that
// walks the plot back into the past.
//
// The aggregation itself is stats.c. Everything here is either layout or the
// state the layout reads, and none of it is written during a layout pass: the
// buttons record what was pressed and ui_stats_update acts on it, the same way
// the run list's row clicks are consumed.

// --- State ---

static StatsSeries series;
static StatsPeriod period = STATS_PERIOD_WEEK;

// Slot 0 is drawn against the left axis, slot 1 against the right.
// STATS_METRIC_COUNT marks a slot as empty, so slot 1 empty is the one-metric
// case and slot 0 is never empty -- a plot with nothing in it is not a state
// worth being able to reach.
static StatsMetric selected[2] = {STATS_METRIC_TOTAL_DISTANCE, STATS_METRIC_COUNT};

// The series is rebuilt on the next update rather than wherever the tracks
// changed: that is sometimes a worker's results landing and sometimes the
// middle of a layout pass, and neither is a place to walk the collection.
static bool series_dirty = true;

// How far back the plot is panned, in periods from the newest. The same three
// values the run list's scroll keeps: where the wheel has put it, what the
// layout draws, and the limit the layout works out from the series.
static float pan_target = 0.0f;
static float pan_current = 0.0f;
static float pan_max = 0.0f;

// Set by a button's hover callback and consumed by the next update. -1 is
// "nothing pressed"; every valid value is a non-negative enumerator.
static int pending_period = -1;
static int pending_metric = -1;

void ui_stats_invalidate(void) {
    series_dirty = true;
}

void ui_stats_free_scratch(void) {
    stats_free(&series);
}

static int metric_count(void) {
    return (selected[1] < STATS_METRIC_COUNT) ? 2 : 1;
}

// --- Geometry ---

// The one place the panel's space is divided up, so the axis columns, the bars
// and the labels cannot each work out a width of their own and disagree.
typedef struct StatsLayout {
    int panel_w, panel_h;
    int plot_w, plot_h;
    int visible;        // periods that fit across the plot
    int group_w, bar_w; // one period, and one bar within it
    int pitch;          // group_w plus the gap after it
    int metrics;        // 1 or 2
} StatsLayout;

static StatsLayout stats_layout(const struct application *appl) {
    StatsLayout l = {0};
    l.metrics = metric_count();

    l.panel_w = ui_panel_width(appl, PANEL_STATISTICS);
    l.panel_h = PANEL_HEIGHT(appl->window_height);

    l.plot_w = l.panel_w - 2 * GAPS - STATS_AXIS_WIDTH * l.metrics;
    l.plot_h = l.panel_h - HEADER_HEIGHT - STATS_XLABEL_HEIGHT -
               2 * STATS_BUTTON_ROW_HEIGHT - PANEL_FOOTER_HEIGHT - 2 * GAPS;
    if (l.plot_w < 1)
        l.plot_w = 1;
    if (l.plot_h < 1)
        l.plot_h = 1;

    // How many periods fit is decided at the narrowest a bar is ever drawn.
    // The last one needs no trailing gap, which is what the extra one adds.
    int min_group = l.metrics * STATS_BAR_MIN_WIDTH +
                    (l.metrics - 1) * STATS_BAR_PAIR_GAP;
    l.visible = (l.plot_w + STATS_BAR_GAP) / (min_group + STATS_BAR_GAP);
    if (l.visible < 1)
        l.visible = 1;
    if (series.count > 0 && l.visible > series.count)
        l.visible = series.count;

    // Then widened back out to use the room that is there, up to the point
    // where five bars would be five slabs.
    int max_group = l.metrics * STATS_BAR_MAX_WIDTH +
                    (l.metrics - 1) * STATS_BAR_PAIR_GAP;
    l.group_w = (l.plot_w - (l.visible - 1) * STATS_BAR_GAP) / l.visible;
    if (l.group_w > max_group)
        l.group_w = max_group;
    if (l.group_w < min_group)
        l.group_w = min_group;

    l.bar_w = (l.group_w - (l.metrics - 1) * STATS_BAR_PAIR_GAP) / l.metrics;
    if (l.bar_w < 1)
        l.bar_w = 1;
    l.pitch = l.group_w + STATS_BAR_GAP;
    return l;
}

// --- Panning ---

static void clamp_pan(int visible) {
    pan_max = (float)(series.count > visible ? series.count - visible : 0);
    if (pan_target > pan_max)
        pan_target = pan_max;
    if (pan_target < 0.0f)
        pan_target = 0.0f;
    if (pan_current > pan_max)
        pan_current = pan_max;
    if (pan_current < 0.0f)
        pan_current = 0.0f;
}

bool ui_stats_pan_by_wheel(int mouse_x, int mouse_y, int detents) {
    // Last frame's box. The wheel is the pointer's, and the pointer was over
    // whatever was drawn last, which is the same thing Clay resolves hover
    // against. On the very first frame the panel is drawn there is no box yet
    // and the detent is declined; nothing else follows from that.
    Clay_ElementData plot = Clay_GetElementData(CLAY_ID("StatsPlot"));
    if (!plot.found)
        return false;

    Clay_BoundingBox box = plot.boundingBox;
    if (mouse_x < box.x || mouse_x >= box.x + box.width ||
        mouse_y < box.y || mouse_y >= box.y + box.height)
        return false;

    // SDL reports a wheel turned away from the hand as positive, which here is
    // a move backwards in time.
    pan_target += (float)detents * STATS_PAN_PERIODS_PER_STEP;
    if (pan_target > pan_max)
        pan_target = pan_max;
    if (pan_target < 0.0f)
        pan_target = 0.0f;
    return true;
}

static bool pan_tick(float dt) {
    if (pan_current == pan_target)
        return false;

    pan_current = anim_approach(pan_current, pan_target, STATS_PAN_TAU, dt);
    // Arriving exactly is what lets the panel stop asking for frames; an
    // asymptote never would.
    if (fabsf(pan_target - pan_current) < 0.001f)
        pan_current = pan_target;
    return true;
}

// --- Button presses ---

// Slot 0 is the left axis, slot 1 the right. A third selection pushes the
// oldest out rather than being refused: a button that does nothing when pressed
// reads as broken, and refusing would leave the user to work out which of the
// other two to switch off first. Evicting in the order they were chosen is
// predictable, and keeps "the one just pressed is on the right" true.
static void apply_metric_click(StatsMetric metric) {
    if (selected[0] == metric) {
        // Never leave the plot with nothing in it: with only one metric
        // selected, pressing it again is the one press that does nothing.
        if (selected[1] == STATS_METRIC_COUNT)
            return;
        selected[0] = selected[1];
        selected[1] = STATS_METRIC_COUNT;
    } else if (selected[1] == metric) {
        selected[1] = STATS_METRIC_COUNT;
    } else if (selected[1] == STATS_METRIC_COUNT) {
        selected[1] = metric;
    } else {
        selected[0] = selected[1];
        selected[1] = metric;
    }
}

bool ui_stats_update(struct application *appl, GpxCollection *collection) {
    // While a job is in flight the collection belongs to its worker, and
    // stats_build walks the array it may be reallocating.
    if (background_busy(&appl->background))
        return false;

    bool changed = false;

    if (pending_period >= 0) {
        period = (StatsPeriod)pending_period;
        pending_period = -1;
        series_dirty = true;
        // A pan measured in weeks means nothing in years: the number of periods
        // changes by an order of magnitude between the scales, so switching
        // goes back to the newest rather than somewhere arbitrary.
        pan_target = 0.0f;
        pan_current = 0.0f;
        changed = true;
    }

    if (pending_metric >= 0) {
        // No rebuild: stats_build fills every metric in the same pass, so
        // which of them are drawn is purely a question for the layout.
        apply_metric_click((StatsMetric)pending_metric);
        pending_metric = -1;
        changed = true;
    }

    // Nothing to compute for a panel that is neither open nor on its way there.
    bool showing = anim_value(&ui.panels[PANEL_STATISTICS]) > 0.0f ||
                   anim_target(&ui.panels[PANEL_STATISTICS]) > 0.5f;
    if (series_dirty && showing) {
        stats_build(&series, collection, period);
        series_dirty = false;
        // The series may have shrunk out from under the view: a filter that
        // hides the oldest years leaves the pan pointing past the end.
        clamp_pan(stats_layout(appl).visible);
        changed = true;
    }

    return pan_tick(appl->delta_time) || changed;
}

// --- Text ---

// A metric's value written the way its axis shows it, in this frame's text.
static const char *value_text(StatsMetric metric, float value) {
    char buffer[STATS_VALUE_MAX];
    stats_format_value(metric, value, buffer, sizeof(buffer));
    return ui_frame_printf("%s", buffer);
}

// What a period is called on the x axis. Derived from the bucket's own
// timestamp, so the week that begins in December and belongs to the next year's
// week 1 needs no arithmetic of its own -- strftime already knows.
static const char *period_text(time_t start, StatsPeriod scale) {
    struct tm tm;
    if (!utc_to_tm(start, &tm))
        return "";

    const char *format = "%d.%m";
    switch (scale) {
    case STATS_PERIOD_DAY:
        format = "%d.%m";
        break;
    case STATS_PERIOD_WEEK:
        // %V is the ISO-8601 week number, the one that pairs with a week
        // starting on Monday. %U and %W count from Sunday and from the first
        // Monday, and neither agrees with stats_period_start.
        format = "KW %V\n%Y";
        break;
    case STATS_PERIOD_MONTH:
        format = "%b\n%Y";
        break;
    case STATS_PERIOD_YEAR:
        format = "%Y";
        break;
    default:
        break;
    }

    char buffer[STATS_VALUE_MAX];
    if (strftime(buffer, sizeof(buffer), format, &tm) == 0)
        return "";
    return ui_frame_printf("%s", buffer);
}

// The colour a metric's bars and its axis are drawn in. Matching the two is
// what makes a plot with two axes readable without a legend.
static Clay_Color slot_color(int slot) {
    return (slot == 0) ? dark_aqua : dark_orange;
}

static Clay_Color slot_color_hl(int slot) {
    return (slot == 0) ? aqua : orange;
}

// --- Clicks ---

static void clicked_timescale(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_period = (int)userData;
}

static void clicked_metric(
    Clay_ElementId elementId,
    Clay_PointerData pointerData,
    intptr_t userData) {
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME)
        pending_metric = (int)userData;
}

// --- Layout ---

static void draw_stats_header(void) {
    CLAY(CLAY_ID("StatsHeader"),
         {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                     .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(HEADER_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(accent_color),
          .cornerRadius = {.topLeft = CORNER_RADIUS, .topRight = CORNER_RADIUS, .bottomLeft = 0, .bottomRight = 0}}) {
        ui_draw_text("Statistics", HEADING_FONT_SIZE, fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

// One value axis: the tick values down the side of the plot, in the colour of
// the bars they measure, plus a foot the width of the x-label row so the two
// columns stay square with the plot between them.
static void draw_stats_axis(int slot, const StatsLayout *l, float max) {
    StatsMetric metric = selected[slot];
    bool on_left = (slot == 0);
    Clay_TextAlignment align = on_left ? CLAY_TEXT_ALIGN_RIGHT : CLAY_TEXT_ALIGN_LEFT;
    Clay_LayoutAlignmentX child_x = on_left ? CLAY_ALIGN_X_RIGHT : CLAY_ALIGN_X_LEFT;

    CLAY(CLAY_IDI("StatsAxis", slot),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(STATS_AXIS_WIDTH), .height = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
        CLAY(CLAY_IDI("StatsAxisTicks", slot),
             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(l->plot_h)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            for (int tick = 0; tick < STATS_AXIS_TICKS; tick++) {
                // Band `tick` is labelled with the value at its top edge, which
                // is the height a bar of that value would reach.
                float value = max * (float)(STATS_AXIS_TICKS - tick) / (float)STATS_AXIS_TICKS;
                CLAY(CLAY_IDI_LOCAL("StatsAxisTick", tick),
                     {.layout = {.padding = {.left = 2, .right = 2},
                                 .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(l->plot_h / STATS_AXIS_TICKS)},
                                 .childAlignment = {.x = child_x, .y = CLAY_ALIGN_Y_TOP}}}) {
                    ui_draw_text(value_text(metric, value), FILTER_TEXT_FONT_SIZE,
                                 slot_color(slot), align);
                }
            }
        }
        // The unit, level with the x-axis labels on the other side of the plot.
        CLAY(CLAY_IDI("StatsAxisUnit", slot),
             {.layout = {.padding = {.left = 2, .right = 2},
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(STATS_XLABEL_HEIGHT)},
                         .childAlignment = {.x = child_x, .y = CLAY_ALIGN_Y_CENTER}}}) {
            ui_draw_text(stats_metric_unit(metric), FILTER_TEXT_FONT_SIZE,
                         slot_color(slot), align);
        }
    }
}

// One period: one bar per selected metric, grown from the bottom. Each metric
// is scaled by its own maximum, which is the whole reason two of them can share
// a plot and still be read off an axis each.
static void draw_stats_group(int position, const StatsBucket *bucket,
                             const StatsLayout *l, const float *max) {
    CLAY(CLAY_IDI("StatsGroup", position),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(l->group_w), .height = CLAY_SIZING_GROW()},
                     .childGap = STATS_BAR_PAIR_GAP,
                     .childAlignment = {.y = CLAY_ALIGN_Y_BOTTOM},
                     .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
        for (int slot = 0; slot < l->metrics; slot++) {
            float fraction = 0.0f;
            if (max[slot] > 0.0f)
                fraction = bucket->value[selected[slot]] / max[slot];
            // Clay treats a percentage above 1 as an error rather than
            // clamping it, and rounding on the way through the float could put
            // the tallest bar a hair over.
            if (fraction > 1.0f)
                fraction = 1.0f;
            if (fraction < 0.0f)
                fraction = 0.0f;

            // A period with nothing in it keeps its bar so the group keeps its
            // width; Clay emits no rectangle at all for a fully transparent
            // one, so an empty week costs nothing to leave in.
            //
            // Indexed by position and slot together rather than with a _LOCAL
            // id: _LOCAL seeds the hash from the parent of the element being
            // opened, which here is the row every group shares, so slot 0 of
            // every group would be the same id. The run list numbers its cells
            // the same way and for the same reason.
            CLAY(CLAY_IDI("StatsBar", position * 2 + slot),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(l->bar_w), .height = CLAY_SIZING_PERCENT(fraction)}},
                  .backgroundColor = ui_fade(slot_color(slot)),
                  .cornerRadius = {.topLeft = 2, .topRight = 2, .bottomLeft = 0, .bottomRight = 0}}) {
            }
        }
    }
}

// The bars and their x labels, in one clipping container so a single offset
// slides both and they cannot drift apart while panning.
static void draw_stats_plot(const StatsLayout *l, const float *max) {
    // floor, not a cast: the pan is clamped at zero, but reading it as an index
    // is worth writing so that it cannot become one below.
    int back = (int)floorf(pan_current);
    if (back < 0)
        back = 0;
    float fraction = pan_current - (float)back;

    int last = series.count - 1 - back;
    if (last < 0)
        last = 0;
    int first = last - l->visible;
    if (first < 0)
        first = 0;
    int drawn = last - first + 1;

    // The newest period drawn sits with its right edge at the right edge of the
    // plot, and the fractional part of the pan slides everything right from
    // there -- so crossing from one period to the next is continuous even
    // though the window it is drawn from steps.
    float content_w = (float)(drawn * l->pitch - STATS_BAR_GAP);
    float offset_x = (float)l->plot_w + fraction * (float)l->pitch - content_w;

    // Only every nth period is labelled once the bars are narrower than the
    // text. Keyed off the period's own index rather than its place in the
    // window, so the labels stay on the same bars while the plot pans.
    int stride = (STATS_XLABEL_MIN_WIDTH + l->pitch - 1) / l->pitch;
    if (stride < 1)
        stride = 1;

    CLAY(CLAY_ID("StatsPlot"),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(l->plot_w), .height = CLAY_SIZING_GROW()},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .clip = {.horizontal = true, .childOffset = {offset_x, 0}},
          .border = {.color = ui_fade(bg7), .width = {.bottom = 1}}}) {
        CLAY(CLAY_ID("StatsBars"),
             {.layout = {.sizing = {.width = CLAY_SIZING_FIXED((int)content_w), .height = CLAY_SIZING_FIXED(l->plot_h)},
                         .childGap = STATS_BAR_GAP,
                         .childAlignment = {.y = CLAY_ALIGN_Y_BOTTOM},
                         .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
            for (int i = 0; i < drawn; i++)
                draw_stats_group(i, &series.buckets[first + i], l, max);
        }

        CLAY(CLAY_ID("StatsXLabels"),
             {.layout = {.sizing = {.width = CLAY_SIZING_FIXED((int)content_w), .height = CLAY_SIZING_FIXED(STATS_XLABEL_HEIGHT)},
                         .childGap = STATS_BAR_GAP,
                         .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
            for (int i = 0; i < drawn; i++) {
                CLAY(CLAY_IDI("StatsXLabel", i),
                     {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(l->group_w), .height = CLAY_SIZING_GROW()},
                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
                    // The unlabelled cells still exist: they are what keeps the
                    // row lined up with the bars above it, and they are also
                    // the room the label beside them spills into.
                    if ((first + i) % stride == 0)
                        ui_draw_text_unwrapped(period_text(series.buckets[first + i].start_utc, series.period),
                                               FILTER_TEXT_FONT_SIZE, fg_d, CLAY_TEXT_ALIGN_CENTER);
                }
            }
        }
    }
}

static void draw_timescale_button(StatsPeriod scale) {
    bool active = (scale == period);
    Clay_Color rest = active ? accent_color_hl : accent_color;

    CLAY(CLAY_IDI("StatsTimescale", scale),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(STATS_TIMESCALE_BUTTON_WIDTH), .height = CLAY_SIZING_FIXED(ELEMENTS_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(Clay_Hovered() ? big_button_color : rest),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        Clay_OnHover(clicked_timescale, scale);
        ui_draw_text(stats_period_label(scale), LABEL_FONT_SIZE,
                     Clay_Hovered() ? bg : fg_l, CLAY_TEXT_ALIGN_CENTER);
    }
}

static void draw_metric_button(StatsMetric metric) {
    int slot = -1;
    for (int i = 0; i < metric_count(); i++) {
        if (selected[i] == metric)
            slot = i;
    }

    Clay_Color rest = (slot >= 0) ? slot_color(slot) : bg5;
    Clay_Color hover = (slot >= 0) ? slot_color_hl(slot) : bg8;

    CLAY(CLAY_IDI("StatsMetric", metric),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(STATS_METRIC_BUTTON_WIDTH), .height = CLAY_SIZING_FIXED(ELEMENTS_HEIGHT)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = ui_fade(Clay_Hovered() ? hover : rest),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        Clay_OnHover(clicked_metric, metric);
        // Dark text on the colours a selected or hovered button wears, light on
        // the grey of an unselected one. Asked from inside the element, since
        // Clay_Hovered() answers about whichever one is open.
        Clay_Color text_color = (slot >= 0 || Clay_Hovered()) ? bg : fg_l;
        ui_draw_text(stats_metric_label(metric), FILTER_TEXT_FONT_SIZE,
                     text_color, CLAY_TEXT_ALIGN_CENTER);
    }
}

void ui_draw_statistics_panel(struct application *appl) {
    // Nothing to draw while it is shut. This has to be decided before the
    // CLAY() block rather than inside it: CLAY() expands to a for loop, and
    // returning out of the middle of one leaves the layout unclosed.
    if (anim_value(&ui.panels[PANEL_STATISTICS]) <= 0.0f)
        return;

    StatsLayout l = stats_layout(appl);
    clamp_pan(l.visible);

    // Each metric against its own maximum over the whole series rather than
    // over the periods on screen. Scaling to the window would rescale the axis
    // on every pan, so a quiet month and a record one would draw the same bar
    // and only the number beside it would say otherwise. Fixed to the tallest
    // period there is, a bar means the same thing wherever the plot is scrolled
    // to -- and the two metrics still get a maximum each, which is what lets
    // them share the plot.
    float max[2] = {0.0f, 0.0f};
    for (int slot = 0; slot < l.metrics; slot++)
        max[slot] = stats_max(&series, selected[slot], 0, series.count);

    CLAY(CLAY_ID("StatsPanel"),
         {.floating = {
              .attachTo = CLAY_ATTACH_TO_ROOT,
              .offset = {.x = ui_panel_offset_x(PANEL_STATISTICS, l.panel_w),
                         .y = PANEL_ORIGIN_Y},
          },
          .layout = {.sizing = {.width = CLAY_SIZING_FIXED(l.panel_w), .height = CLAY_SIZING_FIXED(l.panel_h)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = ui_fade(bg),
          .cornerRadius = CLAY_CORNER_RADIUS(CORNER_RADIUS)}) {
        // Without this the map behind a panel that now covers most of the
        // window would pan and zoom under the pointer.
        if (Clay_Hovered())
            appl->mouse_over_ui = true;

        draw_stats_header();

        CLAY(CLAY_ID("StatsBody"),
             {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(l.plot_h + STATS_XLABEL_HEIGHT + 2 * GAPS)},
                         .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
            if (series.count > 0) {
                draw_stats_axis(0, &l, max[0]);
                draw_stats_plot(&l, max);
                if (l.metrics == 2)
                    draw_stats_axis(1, &l, max[1]);
            } else {
                CLAY(CLAY_ID("StatsEmpty"),
                     {.layout = {.sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_GROW()},
                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
                    ui_draw_text("No activities to summarise", LABEL_FONT_SIZE, fg_d,
                                 CLAY_TEXT_ALIGN_CENTER);
                }
            }
        }

        CLAY(CLAY_ID("StatsTimescaleRow"),
             {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                         .childGap = GAPS,
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(STATS_BUTTON_ROW_HEIGHT)},
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            for (int scale = 0; scale < STATS_PERIOD_COUNT; scale++)
                draw_timescale_button((StatsPeriod)scale);
        }

        CLAY(CLAY_ID("StatsMetricRow"),
             {.layout = {.padding = CLAY_PADDING_ALL(GAPS),
                         .childGap = GAPS,
                         .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(STATS_BUTTON_ROW_HEIGHT)},
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
            for (int metric = 0; metric < STATS_METRIC_COUNT; metric++)
                draw_metric_button((StatsMetric)metric);
        }

        ui_draw_panel_footer(PANEL_STATISTICS);
    }
}
