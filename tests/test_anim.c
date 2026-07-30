#include "harness.h"

#include "../src/anim.c"

// The properties the rest of the program leans on, rather than the shape of the
// curves: that a zeroed Anim is usable, that the ends are exact, that the same
// elapsed time gives the same result however it was cut into frames, and that
// asking for the target it already has does not restart the move.

static void test_zeroed_is_idle(void) {
    SUITE("anim: a zeroed Anim reads as zero and is not running");

    Anim anim = {0};
    CHECK(anim_value(&anim) == 0.0f);
    CHECK(!anim_active(&anim));
    // Nothing to advance, so nothing to redraw for.
    CHECK(!anim_tick(&anim, 0.016f));
}

static void test_ends_are_exact(void) {
    SUITE("anim: every easing starts at zero and lands on one");

    const AnimEase eases[] = {ANIM_LINEAR, ANIM_EASE_OUT, ANIM_EASE_IN_OUT};

    for (size_t i = 0; i < sizeof(eases) / sizeof(eases[0]); i++) {
        Anim anim = {0};
        anim_to(&anim, 1.0f, 0.5f, eases[i]);
        CHECK(anim_value(&anim) == 0.0f);

        // Deliberately overshoots the duration: the value must clamp rather
        // than run past its target.
        anim_tick(&anim, 0.6f);
        CHECK(anim_value(&anim) == 1.0f);
        CHECK(!anim_active(&anim));
    }
}

static void test_tick_reports_the_settling_frame(void) {
    SUITE("anim: the step that settles a move still counts as movement");

    Anim anim = {0};
    anim_to(&anim, 1.0f, 0.1f, ANIM_LINEAR);

    CHECK(anim_tick(&anim, 0.05f)); // mid-move
    CHECK(anim_tick(&anim, 0.05f)); // lands exactly on the end
    CHECK(anim_value(&anim) == 1.0f);
    // The frame showing the final value has now been drawn, so there is
    // nothing left to ask for.
    CHECK(!anim_tick(&anim, 0.05f));
}

static void test_frame_rate_independence(void) {
    SUITE("anim: the same elapsed time gives the same value at any frame rate");

    for (int e = 0; e < 3; e++) {
        Anim fine = {0};
        Anim coarse = {0};
        anim_to(&fine, 1.0f, 1.0f, (AnimEase)e);
        anim_to(&coarse, 1.0f, 1.0f, (AnimEase)e);

        for (int i = 0; i < 50; i++)
            anim_tick(&fine, 0.01f);
        for (int i = 0; i < 5; i++)
            anim_tick(&coarse, 0.1f);

        CHECK_NEAR(anim_value(&fine), anim_value(&coarse), 1e-5);
    }
}

static void test_retarget_starts_from_the_current_value(void) {
    SUITE("anim: reversing mid-move carries on from where it is");

    Anim anim = {0};
    anim_to(&anim, 1.0f, 1.0f, ANIM_LINEAR);
    anim_tick(&anim, 0.4f);

    float caught = anim_value(&anim);
    CHECK_NEAR(caught, 0.4f, 1e-5);

    // This is the case the old opening/closing pair got wrong: a panel asked
    // to reverse while it was already moving.
    anim_to(&anim, 0.0f, 1.0f, ANIM_LINEAR);
    CHECK_NEAR(anim_value(&anim), caught, 1e-5); // no jump
    CHECK(anim_active(&anim));

    anim_tick(&anim, 1.0f);
    CHECK(anim_value(&anim) == 0.0f);
}

static void test_unchanged_target_does_not_restart(void) {
    SUITE("anim: asking again for the target it has does nothing");

    Anim anim = {0};
    anim_to(&anim, 1.0f, 1.0f, ANIM_LINEAR);
    anim_tick(&anim, 0.5f);

    // ui_update calls anim_to every frame from a condition rather than from an
    // event. If that restarted the clock the panel would never arrive.
    for (int i = 0; i < 10; i++)
        anim_to(&anim, 1.0f, 1.0f, ANIM_LINEAR);

    CHECK_NEAR(anim_value(&anim), 0.5f, 1e-5);
    anim_tick(&anim, 0.5f);
    CHECK(anim_value(&anim) == 1.0f);

    // Settled, and asked for the same target again: still nothing to do.
    anim_to(&anim, 1.0f, 1.0f, ANIM_LINEAR);
    CHECK(!anim_active(&anim));
}

static void test_set_is_immediate(void) {
    SUITE("anim: setting a value cancels whatever was in flight");

    Anim anim = {0};
    anim_to(&anim, 1.0f, 1.0f, ANIM_LINEAR);
    anim_tick(&anim, 0.3f);

    anim_set(&anim, 0.25f);
    CHECK(anim_value(&anim) == 0.25f);
    CHECK(!anim_active(&anim));

    // And the cancelled target is no longer the one it holds, so it can be
    // asked for again.
    anim_to(&anim, 1.0f, 1.0f, ANIM_LINEAR);
    CHECK(anim_active(&anim));
}

static void test_zero_duration_lands_immediately(void) {
    SUITE("anim: a move with no duration is just a set");

    Anim anim = {0};
    anim_to(&anim, 1.0f, 0.0f, ANIM_LINEAR);
    CHECK(anim_value(&anim) == 1.0f);
    CHECK(!anim_active(&anim));
}

static void test_approach_is_monotone_and_bounded(void) {
    SUITE("anim: approach closes the gap without overshooting");

    float value = 0.0f;
    float previous = 0.0f;
    for (int i = 0; i < 200; i++) {
        value = anim_approach(value, 1.0f, 0.1f, 0.016f);
        // Never backwards, and never past the target. It plateaus rather than
        // keeps rising, so this is >= and not >.
        CHECK(value >= previous);
        CHECK(value <= 1.0f);
        previous = value;
    }
    CHECK_NEAR(value, 1.0f, 1e-4);

    // One half-life closes half the distance, which is what the name promises.
    CHECK_NEAR(anim_approach(0.0f, 1.0f, 0.25f, 0.25f), 0.5f, 1e-5);

    // Same total time, different frame sizes.
    float fine = 0.0f, coarse = 0.0f;
    for (int i = 0; i < 100; i++)
        fine = anim_approach(fine, 1.0f, 0.2f, 0.005f);
    for (int i = 0; i < 5; i++)
        coarse = anim_approach(coarse, 1.0f, 0.2f, 0.1f);
    CHECK_NEAR(fine, coarse, 1e-5);

    // A target already reached stays put.
    CHECK(anim_approach(1.0f, 1.0f, 0.1f, 0.016f) == 1.0f);
}

void run_anim_tests(void) {
    test_zeroed_is_idle();
    test_ends_are_exact();
    test_tick_reports_the_settling_frame();
    test_frame_rate_independence();
    test_retarget_starts_from_the_current_value();
    test_unchanged_target_does_not_restart();
    test_set_is_immediate();
    test_zero_duration_lands_immediately();
    test_approach_is_monotone_and_bounded();
}
