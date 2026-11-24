#include "timelapse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- The clock ---

// When a track ends, measured off its points rather than its start and end
// stamps: a point is revealed once the clock has passed it, so the last one to
// appear is the latest of them, whatever order the file has them in. Zero for
// a track with no timed point.
static float track_end_secs(const GpxTrack *track) {
    float end = 0.0f;
    for (int i = 0; i < track->total_points; i++)
        if (track->points[i].elapsed_secs > end)
            end = track->points[i].elapsed_secs;
    return end;
}

static int compare_floats(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

void timelapse_span(const GpxCollection *collection, float *typical_secs, float *end_secs) {
    *typical_secs = 0.0f;
    *end_secs = 0.0f;

    // One spare, so an empty collection is still an allocation rather than a
    // malloc(0) that may come back NULL.
    float *ends = malloc(((size_t)collection->total_tracks + 1) * sizeof(float));
    if (!ends)
        return;

    int count = 0;
    for (int i = 0; i < collection->total_tracks; i++) {
        const GpxTrack *track = &collection->tracks[i];
        if (!track->visible_in_list || !track->has_path)
            continue;
        const float end = track_end_secs(track);
        if (end > 0.0f)
            ends[count++] = end;
    }

    if (count > 0) {
        qsort(ends, (size_t)count, sizeof(float), compare_floats);
        // The nearest rank: the shortest duration that at least the share of
        // tracks asked for is no longer than. The nudge keeps a share that
        // lands exactly on a whole track from rounding up past it.
        int rank = (int)ceil((double)TIMELAPSE_TYPICAL_SHARE * count - 1e-4);
        if (rank < 1)
            rank = 1;
        *typical_secs = ends[rank - 1];
        *end_secs = ends[count - 1];
    }
    free(ends);
}

static float clock_rate(const TimelapseClock *clock) {
    return clock->speed * (clock->fast ? TIMELAPSE_FAST_FACTOR : 1.0f);
}

// Sets the time running toward the end at the current rate. A linear move,
// so a retarget mid-play only changes how long the rest of it takes.
static void clock_resume(TimelapseClock *clock) {
    const float now = timelapse_clock_time(clock);
    const float rate = clock_rate(clock);
    if (rate > 0.0f && now < clock->end_secs)
        anim_to(&clock->time, clock->end_secs, (clock->end_secs - now) / rate, ANIM_LINEAR);
}

float timelapse_clock_time(const TimelapseClock *clock) {
    return anim_value(&clock->time);
}

bool timelapse_clock_playing(const TimelapseClock *clock) {
    return anim_active(&clock->time);
}

void timelapse_clock_pause(TimelapseClock *clock) {
    anim_set(&clock->time, timelapse_clock_time(clock));
}

void timelapse_clock_set_span(TimelapseClock *clock, float typical_secs, float end_secs) {
    const bool playing = timelapse_clock_playing(clock);
    const float now = timelapse_clock_time(clock);

    anim_set(&clock->time, now < end_secs ? now : end_secs);
    clock->end_secs = end_secs;
    clock->typical_secs = typical_secs;
    clock->speed = typical_secs / TIMELAPSE_TARGET_SECONDS;
    if (playing)
        clock_resume(clock);
}

void timelapse_clock_play(TimelapseClock *clock) {
    if (timelapse_clock_time(clock) >= clock->end_secs)
        anim_set(&clock->time, 0.0f);
    clock_resume(clock);
}

void timelapse_clock_seek(TimelapseClock *clock, float secs) {
    const bool playing = timelapse_clock_playing(clock);

    if (secs < 0.0f)
        secs = 0.0f;
    if (secs > clock->end_secs)
        secs = clock->end_secs;
    anim_set(&clock->time, secs);
    if (playing)
        clock_resume(clock);
}

void timelapse_clock_set_fast(TimelapseClock *clock, bool fast) {
    const bool playing = timelapse_clock_playing(clock);

    timelapse_clock_pause(clock);
    clock->fast = fast;
    if (playing)
        clock_resume(clock);
}

bool timelapse_clock_tick(TimelapseClock *clock, float dt) {
    return anim_tick(&clock->time, dt);
}

// --- What has been revealed ---

bool timelapse_raster_reset(TimelapseRaster *raster, TimelapseView view, int track_count) {
    raster->track_count = 0;
    raster->next_track = 0;
    raster->view = (TimelapseView){0};
    if (view.width <= 0 || view.height <= 0 || view.per_pixel <= 0 || view.cell <= 0 ||
        track_count < 0)
        return false;

    const int cells_wide = (view.width + view.cell - 1) / view.cell;
    const int cells_high = (view.height + view.cell - 1) / view.cell;
    const int pixels = view.width * view.height;
    const int cells = cells_wide * cells_high;

    // Grown and kept rather than allocated per reset: a pan ends in one of
    // these, and the window is the same size it was the last time.
    if (pixels > raster->capacity_pixels) {
        uint8_t *grown = realloc(raster->lit, (size_t)pixels);
        if (!grown)
            return false;
        raster->lit = grown;
        raster->capacity_pixels = pixels;
    }
    if (cells > raster->capacity_cells) {
        uint16_t *grown = realloc(raster->cells, (size_t)cells * sizeof(uint16_t));
        if (!grown)
            return false;
        raster->cells = grown;
        raster->capacity_cells = cells;
    }
    if (track_count > raster->capacity_tracks) {
        TimelapseTrack *grown = realloc(raster->tracks, (size_t)track_count * sizeof(TimelapseTrack));
        if (!grown)
            return false;
        raster->tracks = grown;
        raster->capacity_tracks = track_count;
    }

    memset(raster->lit, 0, (size_t)pixels);
    memset(raster->cells, 0, (size_t)cells * sizeof(uint16_t));
    for (int i = 0; i < track_count; i++) {
        raster->tracks[i].cursor = 0;
        raster->tracks[i].recent_next = 0;
        for (int j = 0; j < TIMELAPSE_RECENT_CELLS; j++)
            raster->tracks[i].recent[j] = -1;
    }

    raster->view = view;
    raster->cells_wide = cells_wide;
    raster->cells_high = cells_high;
    raster->track_count = track_count;
    return true;
}

// Whether the track has counted itself in this cell lately, remembering that
// it now has if not. A cell is counted once per visit rather than once per
// frame the track spends in it: a run crossing a cell takes many frames to,
// and other tracks reach the same cell in between.
static bool counted_recently(TimelapseTrack *track, int cell) {
    for (int i = 0; i < TIMELAPSE_RECENT_CELLS; i++)
        if (track->recent[i] == cell)
            return true;

    track->recent[track->recent_next] = cell;
    track->recent_next = (track->recent_next + 1) % TIMELAPSE_RECENT_CELLS;
    return false;
}

static void stamp(TimelapseRaster *raster, TimelapseTrack *track, int x, int y,
                  TimelapseEmit emit, void *context) {
    const TimelapseView *view = &raster->view;
    if (x < 0 || y < 0 || x >= view->width || y >= view->height)
        return;

    const int cell_x = x / view->cell, cell_y = y / view->cell;
    const int cell = cell_y * raster->cells_wide + cell_x;
    uint8_t *pixel = &raster->lit[y * view->width + x];
    const bool newly_lit = !*pixel;
    *pixel = 1;

    if (counted_recently(track, cell)) {
        if (newly_lit && emit)
            emit(x, y, raster->cells[cell], context);
        return;
    }

    if (raster->cells[cell] < UINT16_MAX)
        raster->cells[cell]++;
    if (!emit)
        return;

    // The whole cell changes colour with its count, so every pixel already
    // drawn in it is drawn again -- this one among them.
    const int x0 = cell_x * view->cell, y0 = cell_y * view->cell;
    const int x1 = x0 + view->cell < view->width ? x0 + view->cell : view->width;
    const int y1 = y0 + view->cell < view->height ? y0 + view->cell : view->height;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            if (raster->lit[py * view->width + px])
                emit(px, py, raster->cells[cell], context);
}

// Every pixel from one point to the next, leaving out the first: it is the
// last pixel of the segment before, and was stamped then.
static void stamp_segment(TimelapseRaster *raster, TimelapseTrack *track,
                          int x0, int y0, int x1, int y1,
                          TimelapseEmit emit, void *context) {
    const int width = raster->view.width, height = raster->view.height;

    // Both ends past the same edge: nothing of it is on the raster.
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) ||
        (x0 >= width && x1 >= width) || (y0 >= height && y1 >= height))
        return;

    const int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    if (dx > TIMELAPSE_SEGMENT_MAX_PIXELS || -dy > TIMELAPSE_SEGMENT_MAX_PIXELS) {
        stamp(raster, track, x1, y1, emit, context);
        return;
    }

    const int step_x = x0 < x1 ? 1 : -1, step_y = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    int x = x0, y = y0;
    while (x != x1 || y != y1) {
        const int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x += step_x;
        }
        if (twice <= dx) {
            error += dx;
            y += step_y;
        }
        stamp(raster, track, x, y, emit, context);
    }
}

// Floored rather than truncated, so a point just left of the raster lands at
// -1 rather than on column 0.
static int raster_coord(int world, int origin, int per_pixel) {
    return (int)floor((double)(world - origin) / per_pixel);
}

bool timelapse_raster_advance(TimelapseRaster *raster, const GpxCollection *collection,
                              float secs, int budget, TimelapseEmit emit, void *context) {
    const int count = raster->track_count < collection->total_tracks
                          ? raster->track_count
                          : collection->total_tracks;
    const TimelapseView *view = &raster->view;
    const int first = raster->next_track;

    for (int visited = 0; visited < count; visited++) {
        const int i = (first + visited) % count;
        const GpxTrack *track = &collection->tracks[i];
        TimelapseTrack *state = &raster->tracks[i];
        if (!track->visible_in_list || !track->has_path)
            continue;

        // A point with no time of its own is revealed along with the one
        // before it: the comparison is false for NAN, so it never holds the
        // cursor back. A track with no times at all is therefore there whole
        // from the first frame.
        while (state->cursor < track->total_points &&
               !(track->points[state->cursor].elapsed_secs > secs)) {
            if (budget-- <= 0) {
                raster->next_track = i;
                return false;
            }

            const GpxPoint *point = &track->points[state->cursor];
            const int x = raster_coord(point->world_x, view->origin_x, view->per_pixel);
            const int y = raster_coord(point->world_y, view->origin_y, view->per_pixel);
            if (state->cursor == 0) {
                stamp(raster, state, x, y, emit, context);
            } else {
                const GpxPoint *previous = point - 1;
                stamp_segment(raster, state,
                              raster_coord(previous->world_x, view->origin_x, view->per_pixel),
                              raster_coord(previous->world_y, view->origin_y, view->per_pixel),
                              x, y, emit, context);
            }
            state->cursor++;
        }
    }
    return true;
}

void timelapse_raster_paint(const TimelapseRaster *raster, TimelapseEmit emit, void *context) {
    const TimelapseView *view = &raster->view;
    for (int y = 0; y < view->height; y++) {
        const uint8_t *row = &raster->lit[y * view->width];
        const uint16_t *cells = &raster->cells[(y / view->cell) * raster->cells_wide];
        for (int x = 0; x < view->width; x++)
            if (row[x])
                emit(x, y, cells[x / view->cell], context);
    }
}

bool timelapse_head(const GpxTrack *track, int cursor, float secs, int *world_x, int *world_y) {
    if (!track->has_path || cursor <= 0 || cursor >= track->total_points)
        return false;

    const GpxPoint *from = &track->points[cursor - 1];
    const GpxPoint *to = &track->points[cursor];

    // How far between the two the clock is. False for a missing time, which
    // leaves the dot on the last point the track revealed.
    float share = 0.0f;
    if (to->elapsed_secs > from->elapsed_secs)
        share = (secs - from->elapsed_secs) / (to->elapsed_secs - from->elapsed_secs);
    if (!(share > 0.0f))
        share = 0.0f;
    if (share > 1.0f)
        share = 1.0f;

    *world_x = from->world_x + (int)lroundf(share * (float)(to->world_x - from->world_x));
    *world_y = from->world_y + (int)lroundf(share * (float)(to->world_y - from->world_y));
    return true;
}

void timelapse_raster_free(TimelapseRaster *raster) {
    free(raster->lit);
    free(raster->cells);
    free(raster->tracks);
    *raster = (TimelapseRaster){0};
}
