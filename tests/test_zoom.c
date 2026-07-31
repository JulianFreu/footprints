#include "harness.h"

// Only zoom.c: anim.c arrives with test_anim.c, and every test file is
// compiled into the one binary, so including it here as well would define
// every anim_* symbol twice.
#include "../src/zoom.c"

// The property the whole design rests on is that the model can jump a whole
// zoom level without the picture moving: the transition falls back by exactly
// what the model gained, and only then eases to nothing. So most of what is
// checked here is where a world point lands on screen either side of a call,
// rather than the numbers inside the transition.

#define WINDOW_W 1200
#define WINDOW_H 800

// Where a world point lands, mirroring map_world_to_screen followed by
// map_transform_rect -- the two steps zoom.c has to keep agreeing with. The
// window centre is integer-divided for the same reason map.c does it.
static float placed_x(const ZoomTransition *z, int world_q, int world_x, int zoom) {
    const double per_pixel = map_world_per_pixel_at(zoom);
    const double unscaled = (double)(world_q - world_x) / per_pixel + WINDOW_W / 2;
    return (float)(z->anchor_x + (unscaled - z->anchor_x) * (double)zoom_scale(z));
}

static float placed_y(const ZoomTransition *z, int world_q, int world_y, int zoom) {
    const double per_pixel = map_world_per_pixel_at(zoom);
    const double unscaled = (double)(world_q - world_y) / per_pixel + WINDOW_H / 2;
    return (float)(z->anchor_y + (unscaled - z->anchor_y) * (double)zoom_scale(z));
}

// A world point off to one side of the camera, so a mistake in the re-centring
// shows up as movement rather than cancelling at the origin.
#define CAMERA_X 140750875
#define CAMERA_Y 89004498
#define POINT_X (CAMERA_X + 37000)
#define POINT_Y (CAMERA_Y - 21000)

// The re-centring truncates to whole world pixels, which at these zooms is a
// small fraction of a screen pixel.
#define STILL 0.05f

static void check_still(const char *what, int steps, int start_zoom) {
    ZoomTransition z = {0};
    int zoom = start_zoom, world_x = CAMERA_X, world_y = CAMERA_Y;

    const float before_x = placed_x(&z, POINT_X, world_x, zoom);
    const float before_y = placed_y(&z, POINT_Y, world_y, zoom);

    const int applied = zoom_step(&z, &zoom, &world_x, &world_y, steps,
                                  300.0f, 200.0f, WINDOW_W, WINDOW_H);
    CHECK_INT(applied, steps);

    current_suite = what;
    CHECK_NEAR(placed_x(&z, POINT_X, world_x, zoom), before_x, STILL);
    CHECK_NEAR(placed_y(&z, POINT_Y, world_y, zoom), before_y, STILL);
}

static void test_picture_does_not_move(void) {
    SUITE("zoom: the model jumping a level does not move the picture");
    check_still("zoom: one level in", 1, 12);
    check_still("zoom: one level out", -1, 12);
    // The low zooms are where the projection loses precision, so they are
    // where the re-centring is worth checking twice.
    check_still("zoom: one level in, low zoom", 1, 5);
    check_still("zoom: one level out, high zoom", -1, 19);
}

static void test_settles_at_identity(void) {
    SUITE("zoom: a settled transition is exactly the identity");
    ZoomTransition z = {0};

    CHECK(!zoom_active(&z));
    CHECK(zoom_scale(&z) == 1.0f);
    CHECK(!zoom_tick(&z, 0.016f)); // nothing moving, nothing to draw

    int zoom = 12, world_x = CAMERA_X, world_y = CAMERA_Y;
    zoom_step(&z, &zoom, &world_x, &world_y, 1, 300.0f, 200.0f, WINDOW_W, WINDOW_H);
    CHECK(zoom_active(&z));
    CHECK_NEAR(zoom_scale(&z), 0.5f, 1e-6f);

    // The step that settles it still reports movement, so the frame showing
    // the final value is drawn; the one after reports none.
    while (zoom_active(&z))
        CHECK(zoom_tick(&z, 0.016f));

    CHECK(zoom_scale(&z) == 1.0f);
    CHECK(!zoom_tick(&z, 0.016f));
}

static void test_ends_of_the_range(void) {
    SUITE("zoom: scrolling past an end changes nothing");
    ZoomTransition z = {0};
    int zoom = MAX_ZOOM, world_x = CAMERA_X, world_y = CAMERA_Y;

    CHECK_INT(zoom_step(&z, &zoom, &world_x, &world_y, 1, 300.0f, 200.0f, WINDOW_W, WINDOW_H), 0);
    CHECK_INT(zoom, MAX_ZOOM);
    CHECK_INT(world_x, CAMERA_X);
    CHECK_INT(world_y, CAMERA_Y);
    CHECK(!zoom_active(&z)); // and no transition was started to ease nothing

    zoom = MIN_ZOOM;
    CHECK_INT(zoom_step(&z, &zoom, &world_x, &world_y, -1, 300.0f, 200.0f, WINDOW_W, WINDOW_H), 0);
    CHECK_INT(zoom, MIN_ZOOM);
    CHECK_INT(world_x, CAMERA_X);
    CHECK_INT(world_y, CAMERA_Y);

    // A step that only partly fits reports what it took, not what it was asked
    // for, so the caller's transition matches the model.
    zoom = MAX_ZOOM - 1;
    CHECK_INT(zoom_step(&z, &zoom, &world_x, &world_y, 3, 300.0f, 200.0f, WINDOW_W, WINDOW_H), 1);
    CHECK_INT(zoom, MAX_ZOOM);
}

static void test_a_step_mid_flight(void) {
    SUITE("zoom: a second detent mid-transition keeps the picture continuous");
    ZoomTransition z = {0};
    int zoom = 12, world_x = CAMERA_X, world_y = CAMERA_Y;

    zoom_step(&z, &zoom, &world_x, &world_y, 1, 300.0f, 200.0f, WINDOW_W, WINDOW_H);
    zoom_tick(&z, 0.05f);
    CHECK(zoom_active(&z));

    const float before_x = placed_x(&z, POINT_X, world_x, zoom);
    const float before_y = placed_y(&z, POINT_Y, world_y, zoom);
    const float mid_scale = zoom_scale(&z);

    // The cursor has drifted since the first detent. The anchor must not
    // follow it, because the anchor is also what the world centre is derived
    // about -- adopting a new one here would shift the picture.
    zoom_step(&z, &zoom, &world_x, &world_y, 1, 480.0f, 610.0f, WINDOW_W, WINDOW_H);

    CHECK_NEAR(z.anchor_x, 300.0f, 1e-6f);
    CHECK_NEAR(z.anchor_y, 200.0f, 1e-6f);
    CHECK_NEAR(placed_x(&z, POINT_X, world_x, zoom), before_x, STILL);
    CHECK_NEAR(placed_y(&z, POINT_Y, world_y, zoom), before_y, STILL);
    CHECK_NEAR(zoom_scale(&z), mid_scale * 0.5f, 1e-5f);

    // And once it has settled the anchor is free again.
    while (zoom_active(&z))
        zoom_tick(&z, 0.016f);
    zoom_step(&z, &zoom, &world_x, &world_y, -1, 480.0f, 610.0f, WINDOW_W, WINDOW_H);
    CHECK_NEAR(z.anchor_x, 480.0f, 1e-6f);
    CHECK_NEAR(z.anchor_y, 610.0f, 1e-6f);
}

static void test_burst_is_bounded(void) {
    SUITE("zoom: a burst of detents cannot run the scale away");
    ZoomTransition z = {0};
    int zoom = 8, world_x = CAMERA_X, world_y = CAMERA_Y;

    // Eight detents with no frame in between. Held to one level the picture
    // snaps through the ones before the last, which is the trade the clamp
    // exists to make: past a scale of 1/2 the tile count is more than
    // MAX_VISIBLE_TILES was sized for.
    for (int i = 0; i < 8; i++) {
        zoom_step(&z, &zoom, &world_x, &world_y, 1, 300.0f, 200.0f, WINDOW_W, WINDOW_H);
        CHECK(anim_value(&z.octaves) >= -ZOOM_MAX_OCTAVES);
        CHECK(anim_value(&z.octaves) <= ZOOM_MAX_OCTAVES);
    }
    CHECK_INT(zoom, 16);

    // Reversing mid-flight composes rather than restarting: half a level in
    // and then a level out leaves the picture on the other side of the model.
    ZoomTransition r = {0};
    zoom = 12;
    zoom_step(&r, &zoom, &world_x, &world_y, 1, 300.0f, 200.0f, WINDOW_W, WINDOW_H);
    while (anim_value(&r.octaves) < -0.5f) // eases upward, from -1 toward 0
        zoom_tick(&r, 0.01f);
    zoom_step(&r, &zoom, &world_x, &world_y, -1, 300.0f, 200.0f, WINDOW_W, WINDOW_H);
    CHECK_INT(zoom, 12);
    CHECK(anim_value(&r.octaves) > 0.0f);
    CHECK(zoom_scale(&r) > 1.0f);
}

void run_zoom_tests(void) {
    test_picture_does_not_move();
    test_settles_at_identity();
    test_ends_of_the_range();
    test_a_step_mid_flight();
    test_burst_is_bounded();
}
