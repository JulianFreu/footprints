#include "ui_internal.h"

#include <stdio.h>

#include <SDL2/SDL_ttf.h>

#include "colors.h"
#include "profiler.h"
#include "settings.h"

// The frame-time graph: one stacked column per recorded frame, oldest at the
// left, with a line across it at the frame budget.
//
// It draws straight to the renderer rather than through Clay, for the same
// reason draw_graph_cursor does -- Clay has no primitive for this, and the
// overlay belongs on top of the panels rather than among them. What it reads is
// src/profiler.c, which does the measuring and knows nothing about drawing.

// The phase colours, indexed by ProfilerPhase, so the order is the enum's.
//
// The seven working phases are saturated and the two waiting ones are the
// background greys. Because the stack is built in enum order, those two land on
// top, and the line where colour stops is one edge readable straight across the
// graph: under it the loop was working, over it the loop was waiting for the
// display or sleeping under the frame cap. That is also what makes a frame the
// loop declined to draw obvious -- it is a column of almost nothing but grey.
static const Clay_Color phase_colors[PROF_PHASE_COUNT] = {
    {0x80, 0xaa, 0x9e, 0xff}, // PROF_EVENTS  -- blue
    {0xd3, 0x86, 0x9b, 0xff}, // PROF_ADOPT   -- purple
    {0x8b, 0xba, 0x7f, 0xff}, // PROF_UPDATE  -- aqua
    {0xb0, 0xb8, 0x46, 0xff}, // PROF_MAP     -- green
    {0xe9, 0xb1, 0x43, 0xff}, // PROF_TRACKS  -- yellow
    {0xf2, 0x59, 0x4b, 0xff}, // PROF_HEAT    -- red
    {0xf2, 0x85, 0x34, 0xff}, // PROF_UI      -- orange
    {0x5a, 0x52, 0x4c, 0xff}, // PROF_PRESENT -- bg7
    {0x45, 0x40, 0x3d, 0xff}, // PROF_CAP     -- bg5
};

// Dark enough to read a thin coloured column against, and translucent so the
// map is still visible under it. colors.h carries no alpha variants and needs
// none for this, the same way tracks.c keeps its own graph greys.
static const Clay_Color profiler_backdrop = {0x19, 0x1a, 0x1a, 0xd8};
static const Clay_Color profiler_budget_line = {0xf2, 0x85, 0x34, 0xc0};
static const Clay_Color profiler_drawn_tick = {0x72, 0x96, 0x6c, 0xff};

// Scratch for one batched pass. Kept at file scope rather than on the stack
// because the frame loop's own stack already carries a 96 KB tile array, and
// these never change size.
//
// `stacked` is how far up each column the phases drawn so far have reached. It
// is carried across the phase loop so the prefix sum is walked once rather than
// once per phase, which is what lets the columns be batched by phase instead of
// drawn one segment at a time.
static float stacked[PROFILER_HISTORY_FRAMES];
static SDL_Rect rects[PROFILER_HISTORY_FRAMES];

// The legend, rasterised as one texture and kept until the numbers on it are
// stale. Nine TTF renders and texture uploads a frame would be a few hundred
// microseconds landing inside PROF_UI on every frame -- the overlay showing up
// in its own measurements -- and numbers changing sixty times a second cannot
// be read anyway.
static SDL_Texture *legend_texture = NULL;
static int legend_width = 0;
static int legend_height = 0;
static float legend_age = 0.0f;

void ui_profiler_free(struct application *appl) {
    (void)appl;
    if (legend_texture) {
        SDL_DestroyTexture(legend_texture);
        legend_texture = NULL;
    }
    legend_width = 0;
    legend_height = 0;
    // So a legend built before the renderer went away is not reused after it.
    legend_age = PROFILER_LEGEND_INTERVAL_SECONDS;
}

// Two lines of readings above one row per phase. Worked out rather than written
// down so that adding a phase moves the box instead of overflowing it.
#define PROFILER_LEGEND_HEADER_ROWS 2
#define PROFILER_LEGEND_ROWS (PROFILER_LEGEND_HEADER_ROWS + PROF_PHASE_COUNT)
#define PROFILER_LEGEND_HEIGHT (PROFILER_LEGEND_LINE_HEIGHT * PROFILER_LEGEND_ROWS)

// The overlay is as tall as whichever of the two sides needs more room. The
// graph is drawn at the top of it, which is where the strip saying which frames
// drew sits under it.
#define PROFILER_CONTENT_HEIGHT \
    (PROFILER_GRAPH_HEIGHT > PROFILER_LEGEND_HEIGHT ? PROFILER_GRAPH_HEIGHT : PROFILER_LEGEND_HEIGHT)

// One line of the legend, blitted into whatever target is current.
static void draw_legend_line(SDL_Renderer *renderer, TTF_Font *font, const char *text,
                             int x, int y, Clay_Color color) {
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, sdl_color(color));
    if (!surface)
        return;

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        SDL_RenderCopy(renderer, texture,
                       NULL, &(SDL_Rect){x, y, surface->w, surface->h});
        SDL_DestroyTexture(texture);
    }

    SDL_FreeSurface(surface);
}

// Rebuilds the cached legend: a header of the numbers worth watching, then one
// row per phase with its rolling average. The colour swatches are not baked in
// -- they are drawn live beside the texture, which keeps this text-only.
static void rebuild_legend(struct application *appl) {
    const Profiler *p = &appl->profiler;
    TTF_Font *font = appl->fonts[0].font;
    if (!font)
        return;

    // There is one font in the process and Clay's renderer resizes it per text
    // command, so it is at whatever the last label drawn happened to want.
    TTF_SetFontSize(font, PROFILER_LEGEND_FONT_SIZE);

    if (!legend_texture) {
        legend_texture = SDL_CreateTexture(appl->renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET,
                                           PROFILER_LEGEND_WIDTH,
                                           PROFILER_LEGEND_HEIGHT);
        if (!legend_texture)
            return;
        SDL_SetTextureBlendMode(legend_texture, SDL_BLENDMODE_BLEND);
        legend_width = PROFILER_LEGEND_WIDTH;
        legend_height = PROFILER_LEGEND_HEIGHT;
    }

    SDL_Texture *previous_target = SDL_GetRenderTarget(appl->renderer);
    if (SDL_SetRenderTarget(appl->renderer, legend_texture) != 0)
        return;

    // Transparent rather than filled: the backdrop is already under it, and
    // painting a second one would darken the graph it overlaps.
    SDL_SetRenderDrawBlendMode(appl->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(appl->renderer, 0, 0, 0, 0);
    SDL_RenderClear(appl->renderer);
    SDL_SetRenderDrawBlendMode(appl->renderer, SDL_BLENDMODE_BLEND);

    float average[PROF_PHASE_COUNT];
    profiler_average(p, PROFILER_AVERAGE_FRAMES, average);

    const ProfilerFrame *newest = profiler_frame(p, 0);
    float last_ms = newest ? newest->total_seconds * 1000.0f : 0.0f;
    float peak_ms = profiler_peak_total(p, PROFILER_HISTORY_FRAMES) * 1000.0f;
    int drawn = profiler_drawn_count(p, PROFILER_HISTORY_FRAMES);

    char line[64];
    // The newest frame's own length rather than a rate counted over a second,
    // so it says what the loop is doing now. Its own line, because the two
    // readings below it are the ones measured over the whole graph.
    snprintf(line, sizeof(line), "%.1f ms   %d fps", (double)last_ms,
             last_ms > 0.0f ? (int)(1000.0f / last_ms + 0.5f) : 0);
    draw_legend_line(appl->renderer, font, line, 0, 0, fg_l);

    // How bad the worst frame in the graph was, and how many of the iterations
    // behind it drew anything -- the duty cycle the strip under the graph
    // shows one column at a time.
    snprintf(line, sizeof(line), "peak %.1f ms   drew %d/%d", (double)peak_ms, drawn,
             profiler_frame_count(p));
    draw_legend_line(appl->renderer, font, line, 0, PROFILER_LEGEND_LINE_HEIGHT, grey2);

    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++) {
        int y = PROFILER_LEGEND_LINE_HEIGHT * (phase + PROFILER_LEGEND_HEADER_ROWS);
        Clay_Color color = profiler_phase_is_idle((ProfilerPhase)phase) ? grey2 : fg_l;

        // Indented past where the swatch is drawn beside this texture.
        draw_legend_line(appl->renderer, font, profiler_phase_name((ProfilerPhase)phase),
                         PROFILER_SWATCH_SIZE + 4, y, color);

        snprintf(line, sizeof(line), "%.2f ms", (double)(average[phase] * 1000.0f));
        draw_legend_line(appl->renderer, font, line, PROFILER_LEGEND_VALUE_X, y, color);
    }

    SDL_SetRenderTarget(appl->renderer, previous_target);
}

// The stacked columns, one batched pass per phase.
static void draw_columns(struct application *appl, SDL_Rect area) {
    const Profiler *p = &appl->profiler;

    // Pixels per second, fixed rather than fitted to the tallest column: a
    // scale that followed the contents would draw two frames of the same length
    // at different heights, and would leave the budget line meaning nothing.
    const float px_per_second =
        (float)area.h / (PROFILER_BUDGET_SECONDS * PROFILER_GRAPH_BUDGETS);

    int columns = profiler_frame_count(p);
    if (columns > PROFILER_HISTORY_FRAMES)
        columns = PROFILER_HISTORY_FRAMES;

    for (int i = 0; i < columns; i++)
        stacked[i] = 0.0f;

    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++) {
        int count = 0;

        for (int age = 0; age < columns; age++) {
            const ProfilerFrame *frame = profiler_frame(p, age);
            if (!frame)
                break;

            float seconds = frame->phase_seconds[phase];
            float bottom = stacked[age];
            stacked[age] += seconds;

            // The newest frame is at the right-hand edge, so age counts
            // leftwards from there.
            int top_px = (int)((bottom + seconds) * px_per_second);
            int bottom_px = (int)(bottom * px_per_second);
            int height = top_px - bottom_px;
            // A phase too short to reach a whole pixel is left out rather than
            // rounded up: a one-pixel band for a microsecond would read as a
            // cost the frame did not have.
            if (height <= 0)
                continue;

            int y = area.y + area.h - top_px;
            if (y < area.y) {
                // A frame past the top of the graph is clipped to it, which is
                // what the fixed scale trades for columns that are comparable.
                height -= area.y - y;
                y = area.y;
            }
            if (height <= 0)
                continue;

            rects[count++] = (SDL_Rect){
                area.x + area.w - (age + 1) * PROFILER_COLUMN_WIDTH, y,
                PROFILER_COLUMN_WIDTH, height};
        }

        if (count > 0) {
            SDL_Color color = sdl_color(phase_colors[phase]);
            SDL_SetRenderDrawColor(appl->renderer, color.r, color.g, color.b, color.a);
            SDL_RenderFillRects(appl->renderer, rects, count);
        }
    }

    // Which iterations actually drew, as a dashed rule along the bottom edge.
    // The duty cycle is the number that says whether drawing on demand is doing
    // its job, and it is not readable from the stack alone.
    int ticks = 0;
    for (int age = 0; age < columns; age++) {
        const ProfilerFrame *frame = profiler_frame(p, age);
        if (frame && frame->drawn)
            rects[ticks++] = (SDL_Rect){
                area.x + area.w - (age + 1) * PROFILER_COLUMN_WIDTH,
                area.y + area.h, PROFILER_COLUMN_WIDTH, 2};
    }

    if (ticks > 0) {
        SDL_Color color = sdl_color(profiler_drawn_tick);
        SDL_SetRenderDrawColor(appl->renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRects(appl->renderer, rects, ticks);
    }

    // What a frame is asked to fit in. Drawn over the columns so it is legible
    // across a full one.
    SDL_Color budget = sdl_color(profiler_budget_line);
    SDL_SetRenderDrawColor(appl->renderer, budget.r, budget.g, budget.b, budget.a);
    int budget_y = area.y + area.h - (int)(PROFILER_BUDGET_SECONDS * px_per_second);
    SDL_RenderFillRect(appl->renderer,
                       &(SDL_Rect){area.x, budget_y, area.w, 1});
}

void ui_profiler_draw(struct application *appl) {
    if (!settings.show_profiler)
        return;

    const int graph_width = PROFILER_HISTORY_FRAMES * PROFILER_COLUMN_WIDTH;
    const int width = PROFILER_PADDING * 3 + graph_width + PROFILER_LEGEND_WIDTH;
    const int height = PROFILER_PADDING * 2 + PROFILER_CONTENT_HEIGHT;

    // A window too small for the overlay loses it rather than getting a clipped
    // one drawn over what little room it has.
    if (appl->window_width < width + 2 * SCREEN_BORDER_PADDING ||
        appl->window_height < height + 2 * SCREEN_BORDER_PADDING)
        return;

    int x = SCREEN_BORDER_PADDING;
    int y = appl->window_height - SCREEN_BORDER_PADDING - height;

    SDL_SetRenderDrawBlendMode(appl->renderer, SDL_BLENDMODE_BLEND);

    SDL_Color backdrop = sdl_color(profiler_backdrop);
    SDL_SetRenderDrawColor(appl->renderer, backdrop.r, backdrop.g, backdrop.b, backdrop.a);
    SDL_RenderFillRect(appl->renderer, &(SDL_Rect){x, y, width, height});

    // Two pixels short of the padding at the bottom, which is where the strip
    // saying which frames drew is laid.
    SDL_Rect graph = {x + PROFILER_PADDING, y + PROFILER_PADDING, graph_width,
                      PROFILER_GRAPH_HEIGHT - 2};
    draw_columns(appl, graph);

    legend_age += appl->delta_time;
    if (!legend_texture || legend_age >= PROFILER_LEGEND_INTERVAL_SECONDS) {
        legend_age = 0.0f;
        rebuild_legend(appl);
    }

    int legend_x = x + PROFILER_PADDING * 2 + graph_width;
    if (legend_texture)
        SDL_RenderCopy(appl->renderer, legend_texture, NULL,
                       &(SDL_Rect){legend_x, y + PROFILER_PADDING, legend_width,
                                   legend_height});

    // The swatches, beside the legend's rows rather than baked into it, so the
    // texture stays text and the palette is read where it is drawn.
    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++) {
        SDL_Color color = sdl_color(phase_colors[phase]);
        SDL_SetRenderDrawColor(appl->renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(
            appl->renderer,
            &(SDL_Rect){legend_x,
                        y + PROFILER_PADDING +
                            PROFILER_LEGEND_LINE_HEIGHT *
                                (phase + PROFILER_LEGEND_HEADER_ROWS) +
                            2,
                        PROFILER_SWATCH_SIZE, PROFILER_SWATCH_SIZE});
    }
}
