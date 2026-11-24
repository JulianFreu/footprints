#include "../src/timelapse.c"

#include "harness.h"

#include <stdlib.h>
#include <string.h>

// The playback clock, the span it is measured from, and the raster that counts
// how many tracks have passed through each cell -- checked against what the
// renderer was told, since what it was told is what ends up on the screen.

#define TEST_MAX_POINTS 64
#define RASTER_W 32
#define RASTER_H 8

static GpxPoint point_store[8][TEST_MAX_POINTS];
static GpxTrack track_store[8];

// A track along row `y`, one point per pixel from x0 for `count` points, the
// i-th of them `secs_per_point * i` seconds in.
static GpxTrack *make_track(int id, int x0, int y, int count, float secs_per_point) {
    GpxTrack *track = &track_store[id];
    memset(track, 0, sizeof(*track));
    track->points = point_store[id];
    track->total_points = count;
    track->track_id = id;
    track->has_path = true;
    track->visible_in_list = true;
    for (int i = 0; i < count; i++)
        point_store[id][i] = (GpxPoint){.world_x = x0 + i, .world_y = y, .track_id = id, .elapsed_secs = secs_per_point * (float)i};
    return track;
}

static GpxCollection collection_of(int count) {
    return (GpxCollection){.tracks = track_store, .total_tracks = count};
}

// The raster tests are in real time, laid out afresh for whatever the
// collection holds at the moment of the call.
static TimelapseSchedule real_time;

static bool advance(TimelapseRaster *raster, const GpxCollection *collection, float secs,
                    int budget, TimelapseEmit emit, void *context) {
    timelapse_schedule_build(&real_time, collection, TIMELAPSE_REAL_TIME);
    return timelapse_raster_advance(raster, collection, &real_time, secs, budget, emit, context);
}

// What the renderer would have on screen: the last count each pixel was drawn
// with, and how many draws it took.
typedef struct Screen {
    int drawn[RASTER_W * RASTER_H];
    int draws;
} Screen;

static void record_emit(int x, int y, int tracks, void *context) {
    Screen *screen = context;
    screen->drawn[y * RASTER_W + x] = tracks;
    screen->draws++;
}

static TimelapseView test_view(int cell) {
    return (TimelapseView){.origin_x = 0, .origin_y = 0, .per_pixel = 1, .width = RASTER_W, .height = RASTER_H, .cell = cell};
}

// Every lit pixel was last drawn with its cell's current count, and nothing
// unlit was drawn at all. This is the whole contract with the renderer.
static bool screen_matches(const TimelapseRaster *raster, const Screen *screen) {
    for (int y = 0; y < RASTER_H; y++) {
        for (int x = 0; x < RASTER_W; x++) {
            const int cell = (y / raster->view.cell) * raster->cells_wide + x / raster->view.cell;
            const int want = raster->lit[y * RASTER_W + x] ? raster->cells[cell] : 0;
            if (screen->drawn[y * RASTER_W + x] != want)
                return false;
        }
    }
    return true;
}

static int lit_count(const TimelapseRaster *raster) {
    int lit = 0;
    for (int i = 0; i < RASTER_W * RASTER_H; i++)
        lit += raster->lit[i] != 0;
    return lit;
}

static void test_span(void) {
    SUITE("timelapse: the span is the longest track and the 95th percentile");

    for (int i = 0; i < 8; i++)
        make_track(i, 0, 0, 2, 100.0f * (float)(i + 1)); // ends at 100..800
    // Neither of these may stretch the timeline.
    track_store[6].visible_in_list = false;
    track_store[7].has_path = false;

    GpxCollection collection = collection_of(8);
    TimelapseSchedule schedule = {0};
    CHECK(timelapse_schedule_build(&schedule, &collection, TIMELAPSE_REAL_TIME));
    CHECK_NEAR(schedule.end_secs, 600.0, 1e-3);
    // Six tracks: 95% of them is 5.7, so the sixth is the first rank that
    // covers it.
    CHECK_NEAR(schedule.typical_secs, 600.0, 1e-3);
    // In real time every track starts at once and plays as recorded; the two
    // left out are not scheduled at all.
    CHECK(schedule.offset[0] == 0.0f && schedule.scale[0] == 1.0f);
    CHECK(schedule.offset[5] == 0.0f && schedule.scale[5] == 1.0f);
    CHECK(isnan(schedule.offset[6]) && isnan(schedule.offset[7]));
    float local;
    CHECK(timelapse_schedule_local(&schedule, 2, 42.0f, &local));
    CHECK(local == 42.0f);
    CHECK(!timelapse_schedule_local(&schedule, 6, 42.0f, &local));
    CHECK(!timelapse_schedule_local(&schedule, 8, 42.0f, &local));

    // Twenty tracks put 95% on a whole track exactly: the nineteenth.
    GpxPoint many_points[20][2];
    GpxTrack many[20];
    for (int i = 0; i < 20; i++) {
        many_points[i][0] = (GpxPoint){.elapsed_secs = 0.0f};
        many_points[i][1] = (GpxPoint){.elapsed_secs = 10.0f * (float)(i + 1)};
        many[i] = (GpxTrack){.points = many_points[i], .total_points = 2, .has_path = true, .visible_in_list = true};
    }
    GpxCollection twenty = {.tracks = many, .total_tracks = 20};
    CHECK(timelapse_schedule_build(&schedule, &twenty, TIMELAPSE_REAL_TIME));
    CHECK_NEAR(schedule.typical_secs, 190.0, 1e-3);
    CHECK_NEAR(schedule.end_secs, 200.0, 1e-3);

    SUITE("timelapse: the calendar plays the history over the target length");
    // A day apart each, and none of them dated by the real-time build above.
    const time_t day = 86400, origin = 1500000000;
    for (int i = 0; i < 20; i++)
        many[i].start_utc = origin + (time_t)(19 - i) * day; // the last is first
    CHECK(timelapse_schedule_build(&schedule, &twenty, TIMELAPSE_CALENDAR));
    CHECK(schedule.origin_utc == origin);
    CHECK_NEAR(schedule.typical_secs, 19.0 * day, 1.0);
    CHECK_NEAR(schedule.offset[19], 0.0, 1e-3);
    CHECK_NEAR(schedule.offset[0], 19.0 * day, 1.0);
    const double speed = 19.0 * day / TIMELAPSE_TARGET_SECONDS;

    SUITE("timelapse: each track is drawn over a couple of seconds of it");
    // Real seconds spent drawing a track: its timeline length over the speed.
    // The typical one, the 19th of 20, takes the drawing time; a shorter one
    // keeps the same pace, and the one longer than it is squeezed into it.
    CHECK_NEAR(190.0 * schedule.scale[18] / speed, TIMELAPSE_CALENDAR_DRAW_SECONDS, 1e-3);
    CHECK_NEAR(10.0 * schedule.scale[0] / speed, TIMELAPSE_CALENDAR_DRAW_SECONDS / 19.0, 1e-3);
    CHECK_NEAR(200.0 * schedule.scale[19] / speed, TIMELAPSE_CALENDAR_DRAW_SECONDS, 1e-3);
    // The last to start is the first track, and it is the last to finish.
    CHECK_NEAR(schedule.end_secs, 19.0 * day + TIMELAPSE_CALENDAR_DRAW_SECONDS / 19.0 * speed, 2.0);
    CHECK(timelapse_schedule_local(&schedule, 0, 19.0f * day, &local));
    CHECK_NEAR(local, 0.0, 0.5);

    SUITE("timelapse: an undated track has no place on the calendar");
    many[3].start_utc = (time_t)-1;
    CHECK(timelapse_schedule_build(&schedule, &twenty, TIMELAPSE_CALENDAR));
    CHECK(isnan(schedule.offset[3]));
    CHECK(timelapse_schedule_build(&schedule, &twenty, TIMELAPSE_REAL_TIME));
    CHECK(schedule.offset[3] == 0.0f); // in real time it has no need of one

    SUITE("timelapse: a lone track still gets a timeline to be drawn along");
    GpxCollection lone = {.tracks = many, .total_tracks = 1};
    CHECK(timelapse_schedule_build(&schedule, &lone, TIMELAPSE_CALENDAR));
    CHECK(schedule.typical_secs == TIMELAPSE_TARGET_SECONDS);
    CHECK(schedule.end_secs >= schedule.typical_secs);

    SUITE("timelapse: no timed track leaves nothing to play");
    for (int i = 0; i < 20; i++)
        many_points[i][1].elapsed_secs = NAN;
    CHECK(timelapse_schedule_build(&schedule, &twenty, TIMELAPSE_REAL_TIME));
    CHECK(schedule.typical_secs == 0.0f && schedule.end_secs == 0.0f);

    SUITE("timelapse: every build is a new revision");
    const int revision = schedule.revision;
    CHECK(timelapse_schedule_build(&schedule, &twenty, TIMELAPSE_REAL_TIME));
    CHECK_INT(schedule.revision, revision + 1);

    timelapse_schedule_free(&schedule);
}

static void test_clock(void) {
    SUITE("timelapse: the typical track takes the target time to play");

    TimelapseClock clock = {0};
    timelapse_clock_set_span(&clock, 600.0f, 1200.0f);
    CHECK(!timelapse_clock_playing(&clock));
    CHECK(clock.typical_secs == 600.0f);

    timelapse_clock_play(&clock);
    CHECK(timelapse_clock_playing(&clock));
    timelapse_clock_tick(&clock, TIMELAPSE_TARGET_SECONDS / 2.0f);
    CHECK_NEAR(timelapse_clock_time(&clock), 300.0, 1e-2);
    timelapse_clock_tick(&clock, TIMELAPSE_TARGET_SECONDS / 2.0f);
    CHECK_NEAR(timelapse_clock_time(&clock), 600.0, 1e-2);

    SUITE("timelapse: fast multiplies the rate without a jump");
    timelapse_clock_set_fast(&clock, true);
    CHECK_NEAR(timelapse_clock_time(&clock), 600.0, 1e-2);
    CHECK(timelapse_clock_playing(&clock));
    timelapse_clock_tick(&clock, TIMELAPSE_TARGET_SECONDS / 6.0f);
    CHECK_NEAR(timelapse_clock_time(&clock), 900.0, 1e-1);

    SUITE("timelapse: a paused clock stays put");
    timelapse_clock_pause(&clock);
    CHECK(!timelapse_clock_tick(&clock, 1.0f));
    CHECK_NEAR(timelapse_clock_time(&clock), 900.0, 1e-1);

    SUITE("timelapse: seeking clamps to the timeline and keeps the state");
    timelapse_clock_seek(&clock, -5.0f);
    CHECK(timelapse_clock_time(&clock) == 0.0f);
    CHECK(!timelapse_clock_playing(&clock));
    timelapse_clock_seek(&clock, 5000.0f);
    CHECK(timelapse_clock_time(&clock) == 1200.0f);

    SUITE("timelapse: play at the end starts again from the beginning");
    timelapse_clock_play(&clock);
    CHECK(timelapse_clock_time(&clock) == 0.0f);
    CHECK(timelapse_clock_playing(&clock));

    SUITE("timelapse: the clock lands exactly on the end, and stops there");
    for (int i = 0; i < 1000 && timelapse_clock_playing(&clock); i++)
        timelapse_clock_tick(&clock, 0.1f);
    CHECK(timelapse_clock_time(&clock) == 1200.0f);
    CHECK(!timelapse_clock_playing(&clock));

    SUITE("timelapse: a shorter span pulls the clock in and keeps it playing");
    timelapse_clock_seek(&clock, 100.0f);
    timelapse_clock_play(&clock);
    timelapse_clock_set_span(&clock, 50.0f, 80.0f);
    CHECK(timelapse_clock_time(&clock) == 80.0f);
    timelapse_clock_seek(&clock, 10.0f);
    timelapse_clock_play(&clock);
    timelapse_clock_set_span(&clock, 50.0f, 80.0f);
    CHECK(timelapse_clock_playing(&clock));
}

static void test_reveal(void) {
    SUITE("timelapse: a track appears up to the point the clock has reached");

    make_track(0, 2, 1, 10, 1.0f); // x 2..11, one second a pixel
    GpxCollection collection = collection_of(1);
    TimelapseRaster raster = {0};
    Screen screen = {0};

    CHECK(timelapse_raster_reset(&raster, test_view(1), 1));
    CHECK(advance(&raster, &collection, 4.5f, 1000, record_emit, &screen));
    CHECK_INT(raster.tracks[0].cursor, 5);
    CHECK_INT(lit_count(&raster), 5);
    CHECK(raster.lit[1 * RASTER_W + 6]);
    CHECK(!raster.lit[1 * RASTER_W + 7]);
    CHECK(screen_matches(&raster, &screen));

    int x, y;
    CHECK(timelapse_head(&track_store[0], raster.tracks[0].cursor, 4.5f, &x, &y));
    CHECK_INT(x, 7); // halfway between 6 and 7, rounded away from zero
    CHECK_INT(y, 1);

    CHECK(advance(&raster, &collection, 100.0f, 1000, record_emit, &screen));
    CHECK_INT(lit_count(&raster), 10);
    CHECK(screen_matches(&raster, &screen));
    // Finished, so no longer under way.
    CHECK(!timelapse_head(&track_store[0], raster.tracks[0].cursor, 100.0f, &x, &y));

    SUITE("timelapse: points between distant fixes are filled in");
    point_store[0][9].world_x = 30; // a 21-pixel jump from the point before
    CHECK(timelapse_raster_reset(&raster, test_view(1), 1));
    CHECK(advance(&raster, &collection, 100.0f, 1000, record_emit, &screen));
    CHECK_INT(lit_count(&raster), 29); // x 2..30 along row 1
    for (int px = 2; px <= 30; px++)
        CHECK(raster.lit[1 * RASTER_W + px]);

    timelapse_raster_free(&raster);
}

static void test_counting(void) {
    SUITE("timelapse: two tracks on one road count two, however they interleave");

    // Slow tracks: ten seconds a pixel, so each sits in a cell across many of
    // the small steps below while the other one visits it too.
    make_track(0, 0, 2, 20, 10.0f);
    make_track(1, 0, 3, 20, 10.0f); // the next row: the same 2x2 cells
    GpxCollection collection = collection_of(2);

    TimelapseRaster stepped = {0}, whole = {0};
    Screen stepped_screen = {0}, whole_screen = {0};
    CHECK(timelapse_raster_reset(&stepped, test_view(2), 2));
    CHECK(timelapse_raster_reset(&whole, test_view(2), 2));

    for (float t = 0.0f; t <= 200.0f; t += 0.7f)
        advance(&stepped, &collection, t, 1000, record_emit, &stepped_screen);
    advance(&stepped, &collection, 200.0f, 1000, record_emit, &stepped_screen);
    advance(&whole, &collection, 200.0f, 1000, record_emit, &whole_screen);

    for (int cell = 0; cell < 10; cell++)
        CHECK_INT(stepped.cells[1 * stepped.cells_wide + cell], 2);
    CHECK(memcmp(stepped.cells, whole.cells,
                 (size_t)stepped.cells_wide * stepped.cells_high * sizeof(uint16_t)) == 0);
    CHECK(memcmp(stepped.lit, whole.lit, RASTER_W * RASTER_H) == 0);
    CHECK(screen_matches(&stepped, &stepped_screen));
    CHECK(screen_matches(&whole, &whole_screen));

    SUITE("timelapse: a track lingering in one cell counts once");
    make_track(0, 4, 4, 30, 1.0f);
    for (int i = 0; i < 30; i++)
        point_store[0][i].world_x = 4 + (i % 2); // jittering between two pixels
    GpxCollection lone = collection_of(1);
    CHECK(timelapse_raster_reset(&stepped, test_view(2), 1));
    for (float t = 0.0f; t <= 30.0f; t += 0.5f)
        advance(&stepped, &lone, t, 1000, record_emit, &stepped_screen);
    CHECK_INT(stepped.cells[2 * stepped.cells_wide + 2], 1);

    timelapse_raster_free(&stepped);
    timelapse_raster_free(&whole);
}

static void test_silent_then_paint(void) {
    SUITE("timelapse: counting without drawing, then painting, shows the same");

    make_track(0, 0, 2, 20, 3.0f);
    make_track(1, 5, 3, 20, 2.0f);
    make_track(2, 0, 2, 20, 5.0f);
    GpxCollection collection = collection_of(3);

    TimelapseRaster live = {0}, silent = {0};
    Screen live_screen = {0}, silent_screen = {0};
    CHECK(timelapse_raster_reset(&live, test_view(2), 3));
    CHECK(timelapse_raster_reset(&silent, test_view(2), 3));
    for (float t = 0.0f; t <= 100.0f; t += 1.3f) {
        advance(&live, &collection, t, 1000, record_emit, &live_screen);
        advance(&silent, &collection, t, 4, NULL, NULL);
    }
    while (!advance(&silent, &collection, 100.0f, 4, NULL, NULL))
        ;
    advance(&live, &collection, 100.0f, 1000, record_emit, &live_screen);
    CHECK_INT(silent_screen.draws, 0);

    timelapse_raster_paint(&silent, record_emit, &silent_screen);
    CHECK(screen_matches(&silent, &silent_screen));
    CHECK(memcmp(live_screen.drawn, silent_screen.drawn, sizeof(live_screen.drawn)) == 0);
    // Each pixel once, against once per change while drawing as it grew.
    CHECK_INT(silent_screen.draws, lit_count(&silent));
    CHECK(live_screen.draws > silent_screen.draws);

    timelapse_raster_free(&live);
    timelapse_raster_free(&silent);
}

static void test_calendar_reveal(void) {
    SUITE("timelapse: on the calendar a track waits for its day");

    const time_t day = 86400;
    make_track(0, 0, 1, 10, 60.0f);
    make_track(1, 0, 4, 10, 60.0f);
    track_store[0].start_utc = 1500000000;
    track_store[1].start_utc = 1500000000 + day;
    GpxCollection collection = collection_of(2);

    TimelapseSchedule schedule = {0};
    TimelapseRaster raster = {0};
    Screen screen = {0};
    CHECK(timelapse_schedule_build(&schedule, &collection, TIMELAPSE_CALENDAR));
    CHECK(timelapse_raster_reset(&raster, test_view(1), 2));

    // Half a day in: the first is drawn whole, the second not begun.
    CHECK(timelapse_raster_advance(&raster, &collection, &schedule, 0.5f * day, 1000,
                                   record_emit, &screen));
    CHECK_INT(raster.tracks[0].cursor, 10);
    CHECK_INT(raster.tracks[1].cursor, 0);
    float local;
    int x, y;
    CHECK(timelapse_schedule_local(&schedule, 1, 0.5f * day, &local));
    CHECK(local < 0.0f);
    CHECK(!timelapse_head(&track_store[1], raster.tracks[1].cursor, local, &x, &y));

    // Partway into the second one's drawing time, it is under way.
    const float midway = (float)day + 0.5f * 540.0f * schedule.scale[1];
    CHECK(timelapse_raster_advance(&raster, &collection, &schedule, midway, 1000,
                                   record_emit, &screen));
    CHECK(raster.tracks[1].cursor > 0 && raster.tracks[1].cursor < 10);
    CHECK(timelapse_schedule_local(&schedule, 1, midway, &local));
    CHECK_NEAR(local, 270.0, 1.0);
    CHECK(timelapse_head(&track_store[1], raster.tracks[1].cursor, local, &x, &y));
    CHECK_INT(y, 4);

    CHECK(timelapse_raster_advance(&raster, &collection, &schedule, schedule.end_secs, 1000,
                                   record_emit, &screen));
    CHECK_INT(raster.tracks[1].cursor, 10);
    CHECK_INT(lit_count(&raster), 20);
    CHECK(screen_matches(&raster, &screen));

    timelapse_raster_free(&raster);
    timelapse_schedule_free(&schedule);
}

static void test_budget(void) {
    SUITE("timelapse: a pass that runs out of points carries on where it stopped");

    make_track(0, 0, 0, 20, 1.0f);
    make_track(1, 0, 5, 20, 1.0f);
    GpxCollection collection = collection_of(2);
    TimelapseRaster raster = {0};
    Screen screen = {0};
    CHECK(timelapse_raster_reset(&raster, test_view(1), 2));

    int passes = 0;
    while (!advance(&raster, &collection, 100.0f, 7, record_emit, &screen))
        passes++;
    CHECK_INT(passes, 5); // forty points, seven at a time
    CHECK_INT(raster.tracks[0].cursor, 20);
    CHECK_INT(raster.tracks[1].cursor, 20);
    CHECK_INT(lit_count(&raster), 40);
    CHECK(screen_matches(&raster, &screen));

    timelapse_raster_free(&raster);
}

static void test_missing_times_and_edges(void) {
    SUITE("timelapse: a point with no time comes with the one before it");

    make_track(0, 0, 0, 6, 1.0f);
    point_store[0][3].elapsed_secs = NAN;
    GpxCollection collection = collection_of(1);
    TimelapseRaster raster = {0};
    Screen screen = {0};
    CHECK(timelapse_raster_reset(&raster, test_view(1), 1));
    advance(&raster, &collection, 2.0f, 1000, record_emit, &screen);
    CHECK_INT(raster.tracks[0].cursor, 4);

    SUITE("timelapse: a track with no times is there whole from the start");
    for (int i = 0; i < 6; i++)
        point_store[0][i].elapsed_secs = NAN;
    CHECK(timelapse_raster_reset(&raster, test_view(1), 1));
    advance(&raster, &collection, 0.0f, 1000, record_emit, &screen);
    CHECK_INT(raster.tracks[0].cursor, 6);

    SUITE("timelapse: hidden tracks and tracks off the raster draw nothing");
    make_track(0, -50, -50, 20, 1.0f);        // entirely off to the top left
    make_track(1, 100000, 3, 2, 1.0f);        // a far jump onto row 3
    point_store[1][1].world_x = RASTER_W - 1; // longer than a segment may be
    make_track(2, 0, 6, 10, 1.0f);
    track_store[2].visible_in_list = false;
    collection = collection_of(3);
    memset(&screen, 0, sizeof(screen));
    CHECK(timelapse_raster_reset(&raster, test_view(1), 3));
    advance(&raster, &collection, 100.0f, 1000, record_emit, &screen);
    CHECK_INT(lit_count(&raster), 1); // only the end of the jump
    CHECK(raster.lit[3 * RASTER_W + RASTER_W - 1]);
    CHECK_INT(raster.tracks[2].cursor, 0);
    CHECK(screen_matches(&raster, &screen));

    SUITE("timelapse: a raster with nowhere to draw refuses the reset");
    CHECK(!timelapse_raster_reset(&raster, (TimelapseView){.per_pixel = 1, .cell = 1}, 1));
    CHECK(advance(&raster, &collection, 100.0f, 1000, record_emit, &screen));

    timelapse_raster_free(&raster);
}

void run_timelapse_tests(void) {
    test_span();
    test_clock();
    test_reveal();
    test_counting();
    test_silent_then_paint();
    test_calendar_reveal();
    test_budget();
    test_missing_times_and_edges();
    timelapse_schedule_free(&real_time);
}
