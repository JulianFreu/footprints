#include "harness.h"

#include "../src/profiler.c"

// The properties the overlay leans on: that the marks attribute a frame's time
// to something rather than sampling it, that the ring hands back the frames it
// was given in the order it was given them, and that the reductions divide by
// what is there. Driven through the _at variants throughout, so every
// expectation is exact rather than a tolerance around a real clock.

// One whole frame of the shape the loop produces, so the ring tests are not
// each spelling out nine marks. `drawn` decides whether the render phases run.
static void file_frame(Profiler *p, double start, double length, bool drawn) {
    profiler_mark_at(p, PROF_EVENTS, start);
    profiler_mark_at(p, PROF_UPDATE, start + length * 0.25);
    if (drawn) {
        profiler_mark_at(p, PROF_MAP, start + length * 0.5);
        profiler_mark_at(p, PROF_PRESENT, start + length * 0.75);
    }
    profiler_end_frame_at(p, start + length);
}

static void test_zeroed_is_empty(void) {
    SUITE("profiler: a zeroed Profiler is a valid empty one");

    Profiler p = {0};
    CHECK_INT(profiler_frame_count(&p), 0);
    CHECK(profiler_frame(&p, 0) == NULL);
    CHECK(profiler_peak_total(&p, PROFILER_AVERAGE_FRAMES) == 0.0f);
    CHECK_INT(profiler_drawn_count(&p, PROFILER_AVERAGE_FRAMES), 0);

    // Writes zeros rather than dividing by a count of nothing.
    float average[PROF_PHASE_COUNT];
    profiler_average(&p, PROFILER_AVERAGE_FRAMES, average);
    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++)
        CHECK(average[phase] == 0.0f);
}

static void test_marks_close_the_phase_before_them(void) {
    SUITE("profiler: a mark closes the phase that was open, not the one it opens");

    Profiler p = {0};
    profiler_mark_at(&p, PROF_EVENTS, 0.0);
    profiler_mark_at(&p, PROF_UPDATE, 0.002);
    profiler_end_frame_at(&p, 0.005);

    const ProfilerFrame *frame = profiler_frame(&p, 0);
    CHECK(frame != NULL);
    CHECK_NEAR(frame->phase_seconds[PROF_EVENTS], 0.002, 1e-9);
    CHECK_NEAR(frame->phase_seconds[PROF_UPDATE], 0.003, 1e-9);
    CHECK_NEAR(frame->total_seconds, 0.005, 1e-9);
}

static void test_phases_account_for_the_frame(void) {
    SUITE("profiler: the phases sum to the frame they were measured in");

    // The invariant the stacked columns rest on. If these did not add up, a
    // column would be a sample of the frame rather than a breakdown of it.
    Profiler p = {0};
    profiler_mark_at(&p, PROF_EVENTS, 10.0);
    profiler_mark_at(&p, PROF_ADOPT, 10.001);
    profiler_mark_at(&p, PROF_UPDATE, 10.003);
    profiler_mark_at(&p, PROF_MAP, 10.004);
    profiler_mark_at(&p, PROF_UI, 10.007);
    profiler_mark_at(&p, PROF_PRESENT, 10.009);
    profiler_mark_at(&p, PROF_CAP, 10.015);
    profiler_end_frame_at(&p, 10.016);

    const ProfilerFrame *frame = profiler_frame(&p, 0);
    float sum = 0.0f;
    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++)
        sum += frame->phase_seconds[phase];

    CHECK_NEAR(sum, frame->total_seconds, 1e-6);
    CHECK_NEAR(frame->total_seconds, 0.016, 1e-9);
}

static void test_unmarked_phases_are_zero(void) {
    SUITE("profiler: a frame the loop declined to draw has no render phases");

    Profiler p = {0};
    file_frame(&p, 0.0, 0.016, false);

    const ProfilerFrame *frame = profiler_frame(&p, 0);
    CHECK(frame->phase_seconds[PROF_MAP] == 0.0f);
    CHECK(frame->phase_seconds[PROF_TRACKS] == 0.0f);
    CHECK(frame->phase_seconds[PROF_HEAT] == 0.0f);
    CHECK(frame->phase_seconds[PROF_UI] == 0.0f);
    CHECK(frame->phase_seconds[PROF_PRESENT] == 0.0f);
    // What it did do is still measured.
    CHECK(frame->phase_seconds[PROF_EVENTS] > 0.0f);
}

static void test_drawn_follows_present(void) {
    SUITE("profiler: whether a frame drew is read off PROF_PRESENT");

    // Nothing passes it in -- marking present is what having drawn means.
    Profiler drew = {0};
    file_frame(&drew, 0.0, 0.016, true);
    CHECK(profiler_frame(&drew, 0)->drawn);

    Profiler skipped = {0};
    file_frame(&skipped, 0.0, 0.016, false);
    CHECK(!profiler_frame(&skipped, 0)->drawn);
}

static void test_a_phase_returned_to_accumulates(void) {
    SUITE("profiler: marking a phase twice in one frame adds to it");

    // The loop draws the track overlays either side of the heat tiles, and both
    // halves belong to PROF_TRACKS. Assigning would throw the first away.
    Profiler p = {0};
    profiler_mark_at(&p, PROF_TRACKS, 0.0);
    profiler_mark_at(&p, PROF_HEAT, 0.002);
    profiler_mark_at(&p, PROF_TRACKS, 0.005);
    profiler_end_frame_at(&p, 0.006);

    const ProfilerFrame *frame = profiler_frame(&p, 0);
    CHECK_NEAR(frame->phase_seconds[PROF_TRACKS], 0.003, 1e-9);
    CHECK_NEAR(frame->phase_seconds[PROF_HEAT], 0.003, 1e-9);
}

static void test_ring_wraps_and_ages(void) {
    SUITE("profiler: the ring keeps the newest frames, newest first");

    Profiler p = {0};
    const int filed = PROFILER_HISTORY_FRAMES + 7;
    for (int i = 0; i < filed; i++)
        file_frame(&p, (double)i, 0.001 * (double)(i + 1), true);

    CHECK_INT(profiler_frame_count(&p), PROFILER_HISTORY_FRAMES);

    // Durations are stored as float, and these are the longest any test here
    // uses -- a tenth of a second carries about 1e-8 of storage error, so the
    // tolerance is loosened from the 1e-9 the millisecond-scale checks use.
    // Age 0 is the last one filed, and each step back is one frame earlier.
    CHECK_NEAR(profiler_frame(&p, 0)->total_seconds, 0.001 * (double)filed, 1e-6);
    CHECK_NEAR(profiler_frame(&p, 1)->total_seconds, 0.001 * (double)(filed - 1), 1e-6);

    // The oldest still retained, and one past it.
    CHECK_NEAR(profiler_frame(&p, PROFILER_HISTORY_FRAMES - 1)->total_seconds,
               0.001 * (double)(filed - PROFILER_HISTORY_FRAMES + 1), 1e-6);
    CHECK(profiler_frame(&p, PROFILER_HISTORY_FRAMES) == NULL);
}

static void test_out_of_range_age_is_null(void) {
    SUITE("profiler: an age outside the ring reads as nothing, not off the end");

    Profiler p = {0};
    file_frame(&p, 0.0, 0.016, true);

    CHECK(profiler_frame(&p, -1) == NULL);
    CHECK(profiler_frame(&p, 1) == NULL);
    CHECK(profiler_frame(&p, PROFILER_HISTORY_FRAMES * 4) == NULL);
    CHECK(profiler_frame(&p, 0) != NULL);
}

static void test_out_of_range_phase_is_ignored(void) {
    SUITE("profiler: a phase outside the enum is dropped, not written past");

    Profiler p = {0};
    profiler_mark_at(&p, PROF_EVENTS, 0.0);
    profiler_mark_at(&p, (ProfilerPhase)PROF_PHASE_COUNT, 0.002);
    profiler_mark_at(&p, (ProfilerPhase)-3, 0.003);
    profiler_end_frame_at(&p, 0.004);

    // The bad marks changed nothing, so the whole frame is still the events
    // phase that was open when they arrived.
    const ProfilerFrame *frame = profiler_frame(&p, 0);
    CHECK_NEAR(frame->phase_seconds[PROF_EVENTS], 0.004, 1e-9);
    CHECK_NEAR(frame->total_seconds, 0.004, 1e-9);
}

static void test_average_windows_and_divides_by_what_is_there(void) {
    SUITE("profiler: averages are over the frames actually recorded");

    Profiler p = {0};
    // Ten frames, each spending exactly 2 ms in events.
    for (int i = 0; i < 10; i++) {
        profiler_mark_at(&p, PROF_EVENTS, (double)i);
        profiler_mark_at(&p, PROF_CAP, (double)i + 0.002);
        profiler_end_frame_at(&p, (double)i + 0.016);
    }

    float average[PROF_PHASE_COUNT];

    // Asked for fewer than there are: only the newest count.
    profiler_average(&p, 4, average);
    CHECK_NEAR(average[PROF_EVENTS], 0.002, 1e-9);

    // Asked for more than there are: divided by ten, not by the sixty asked
    // for, or the answer would read as a sixth of the real cost.
    profiler_average(&p, PROFILER_AVERAGE_FRAMES, average);
    CHECK_NEAR(average[PROF_EVENTS], 0.002, 1e-9);
    CHECK_NEAR(average[PROF_CAP], 0.014, 1e-9);
}

static void test_skipped_frames_pull_the_average_down(void) {
    SUITE("profiler: an average is per frame, not per drawn frame");

    // Four frames, two of which drew. The map phase cost 4 ms on each of those
    // and nothing on the others, so the answer to "what does a frame cost" is
    // 2 ms -- deliberately not the 4 ms a drawn frame costs.
    Profiler p = {0};
    for (int i = 0; i < 4; i++) {
        profiler_mark_at(&p, PROF_EVENTS, (double)i);
        if (i % 2 == 0) {
            profiler_mark_at(&p, PROF_MAP, (double)i + 0.001);
            profiler_mark_at(&p, PROF_PRESENT, (double)i + 0.005);
        }
        profiler_end_frame_at(&p, (double)i + 0.016);
    }

    float average[PROF_PHASE_COUNT];
    profiler_average(&p, 4, average);
    CHECK_NEAR(average[PROF_MAP], 0.002, 1e-9);
    CHECK_INT(profiler_drawn_count(&p, 4), 2);
}

static void test_peak_and_drawn_count(void) {
    SUITE("profiler: the peak is the longest frame in the window");

    Profiler p = {0};
    file_frame(&p, 0.0, 0.016, true);
    file_frame(&p, 1.0, 0.040, false); // the hitch
    file_frame(&p, 2.0, 0.016, true);
    file_frame(&p, 3.0, 0.016, true);

    CHECK_NEAR(profiler_peak_total(&p, 4), 0.040, 1e-9);
    // A window that does not reach back to it does not report it.
    CHECK_NEAR(profiler_peak_total(&p, 2), 0.016, 1e-9);
    CHECK_INT(profiler_drawn_count(&p, 4), 3);
    CHECK_INT(profiler_drawn_count(&p, 2), 2);
}

static void test_backwards_clock_contributes_nothing(void) {
    SUITE("profiler: a clock that goes backwards cannot make a phase negative");

    // A segment below the baseline would be drawn upside down out of the
    // bottom of the graph, so it is clamped where it is measured.
    Profiler p = {0};
    profiler_mark_at(&p, PROF_EVENTS, 5.0);
    profiler_mark_at(&p, PROF_UPDATE, 4.0);
    profiler_end_frame_at(&p, 3.0);

    const ProfilerFrame *frame = profiler_frame(&p, 0);
    CHECK(frame->phase_seconds[PROF_EVENTS] == 0.0f);
    CHECK(frame->phase_seconds[PROF_UPDATE] == 0.0f);
    CHECK(frame->total_seconds == 0.0f);
}

static void test_empty_frame_is_not_filed(void) {
    SUITE("profiler: ending a frame that was never marked files nothing");

    // Otherwise a stray call would push a real frame out of the ring to record
    // that nothing happened.
    Profiler p = {0};
    profiler_end_frame_at(&p, 1.0);
    profiler_end_frame_at(&p, 2.0);
    CHECK_INT(profiler_frame_count(&p), 0);

    file_frame(&p, 3.0, 0.016, true);
    CHECK_INT(profiler_frame_count(&p), 1);
}

static void test_phase_names_and_idleness(void) {
    SUITE("profiler: every phase has its own name, and two of them are waiting");

    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++) {
        const char *name = profiler_phase_name((ProfilerPhase)phase);
        CHECK(name[0] != '\0');
        // A duplicate would make one legend row name the wrong measurement.
        for (int other = 0; other < phase; other++)
            CHECK(strcmp(name, profiler_phase_name((ProfilerPhase)other)) != 0);
    }

    // These two are the loop waiting rather than working, which is what the
    // overlay greys out and stacks on top.
    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++) {
        bool idle = profiler_phase_is_idle((ProfilerPhase)phase);
        CHECK(idle == (phase == PROF_PRESENT || phase == PROF_CAP));
    }

    CHECK_STR(profiler_phase_name((ProfilerPhase)PROF_PHASE_COUNT), "?");
}

void run_profiler_tests(void) {
    test_zeroed_is_empty();
    test_marks_close_the_phase_before_them();
    test_phases_account_for_the_frame();
    test_unmarked_phases_are_zero();
    test_drawn_follows_present();
    test_a_phase_returned_to_accumulates();
    test_ring_wraps_and_ages();
    test_out_of_range_age_is_null();
    test_out_of_range_phase_is_ignored();
    test_average_windows_and_divides_by_what_is_there();
    test_skipped_frames_pull_the_average_down();
    test_peak_and_drawn_count();
    test_backwards_clock_contributes_nothing();
    test_empty_frame_is_not_filed();
    test_phase_names_and_idleness();
}
