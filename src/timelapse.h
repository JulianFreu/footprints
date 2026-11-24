#ifndef TIMELAPSE_H
#define TIMELAPSE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "anim.h"
#include "gpx_types.h"

// The visible tracks building up over a shared timeline: either as if every one
// of them had started at the same moment, each point appearing when its own
// recording reached it, or one after another in the order they were recorded.
//
// Nothing here draws. The clock is the playback, and the raster is what has
// been revealed so far, counted the way the heat is: per cell, how many tracks
// have passed through it. What a cell's count looks like is the renderer's
// business, so this file is arithmetic all the way down and is tested as such.

// --- The clock ---

typedef struct TimelapseClock {
    // Seconds along the timeline the schedule lays the tracks on. Moving toward
    // end_secs while playing and settled otherwise, so it asks for its own
    // frames.
    Anim time;
    // When the last track has finished, where the bar under the clock ends,
    // and the timeline seconds that pass in one real second at normal speed.
    float end_secs;
    float typical_secs;
    float speed;
    bool fast;
} TimelapseClock;

// --- Where each track sits on the timeline ---

typedef enum TimelapseMode {
    // Every track starts at zero and plays at the speed it was recorded at.
    TIMELAPSE_REAL_TIME,
    // Each track starts on the day it was recorded, the whole history played
    // over the target length, and is drawn over a couple of seconds of it.
    TIMELAPSE_CALENDAR,
} TimelapseMode;

typedef struct TimelapseSchedule {
    TimelapseMode mode;
    // Per track: the timeline second it starts at, NAN for one that is not in
    // this replay, and the timeline seconds each second of its recording takes.
    float *offset;
    float *scale;
    int count;
    int capacity;
    // What the clock is set from: where the bar ends and where the last track
    // does. In real time the bar ends when TIMELAPSE_TYPICAL_SHARE of the
    // tracks have finished, so one long hike does not stretch it; in calendar
    // mode it ends when the last track starts.
    float typical_secs;
    float end_secs;
    // The moment timeline zero stands for, in calendar mode.
    time_t origin_utc;
    // Raised by every build, so whatever was drawn from the last one can tell.
    int revision;
} TimelapseSchedule;

// Lays the visible tracks with a path out on the timeline. False if it could
// not be allocated, which leaves nothing scheduled.
bool timelapse_schedule_build(TimelapseSchedule *schedule, const GpxCollection *collection,
                              TimelapseMode mode);
// Where track `index` is in its own recording when the clock reads `secs`.
// False for a track the schedule leaves out.
bool timelapse_schedule_local(const TimelapseSchedule *schedule, int index, float secs,
                              float *local_secs);
void timelapse_schedule_free(TimelapseSchedule *schedule);

// Adopts a new span, keeping the position and whether it is playing.
void timelapse_clock_set_span(TimelapseClock *clock, float typical_secs, float end_secs);
float timelapse_clock_time(const TimelapseClock *clock);
bool timelapse_clock_playing(const TimelapseClock *clock);
// Plays from where it is, or from the start once it has reached the end.
void timelapse_clock_play(TimelapseClock *clock);
void timelapse_clock_pause(TimelapseClock *clock);
// Moves to `secs`, clamped to the timeline, without changing whether it plays.
void timelapse_clock_seek(TimelapseClock *clock, float secs);
void timelapse_clock_set_fast(TimelapseClock *clock, bool fast);
// Returns whether the time moved.
bool timelapse_clock_tick(TimelapseClock *clock, float dt);

// --- What has been revealed ---

// The window the raster is drawn for: world pixels per raster pixel, and the
// world point raster pixel (0, 0) covers. Overlaps are counted per square of
// `cell` pixels.
typedef struct TimelapseView {
    int origin_x;
    int origin_y;
    int per_pixel;
    int width;
    int height;
    int cell;
} TimelapseView;

typedef struct TimelapseTrack {
    int cursor; // points revealed so far
    int recent[TIMELAPSE_RECENT_CELLS];
    int recent_next;
} TimelapseTrack;

// Called for every pixel whose colour may have changed, with how many tracks
// have now passed through its cell. May be NULL, to count without drawing.
typedef void (*TimelapseEmit)(int x, int y, int tracks, void *context);

typedef struct TimelapseRaster {
    TimelapseView view;
    int cells_wide;
    int cells_high;
    uint16_t *cells; // tracks counted in each cell
    uint8_t *lit;    // nonzero where a track has passed through the pixel
    TimelapseTrack *tracks;
    int track_count;
    int capacity_pixels;
    int capacity_cells;
    int capacity_tracks;
    // Where a pass that ran out of points picks up, so none is left behind.
    int next_track;
} TimelapseRaster;

// Empties the raster and points it at `view`, for a collection of
// `track_count` tracks. Returns false if it could not be allocated.
bool timelapse_raster_reset(TimelapseRaster *raster, TimelapseView view, int track_count);

// Brings the scheduled tracks up to `secs`, walking at most `budget` points.
// Returns whether every track got there; when not, the next call carries on
// from where this one stopped. Time only moves forward here: going back is a
// reset and a fresh advance.
bool timelapse_raster_advance(TimelapseRaster *raster, const GpxCollection *collection,
                              const TimelapseSchedule *schedule, float secs, int budget,
                              TimelapseEmit emit, void *context);

// Every pixel revealed so far, once, with its cell's count. What follows an
// advance that was made without drawing.
void timelapse_raster_paint(const TimelapseRaster *raster, TimelapseEmit emit, void *context);

// Where a track that has started and not finished is `local_secs` into its
// recording, between the last point it revealed and the next. False for one
// that is not under way.
bool timelapse_head(const GpxTrack *track, int cursor, float local_secs, int *world_x, int *world_y);

void timelapse_raster_free(TimelapseRaster *raster);

#endif
