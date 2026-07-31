#include "zoom.h"

#include <math.h>

#include "config.h"

// Depends on nothing but libm and the tunables, which is what lets the
// arithmetic that keeps the picture still be tested without a window.

int map_world_per_pixel_at(int zoom) {
    return 1 << (MAX_ZOOM - zoom);
}

bool zoom_active(const ZoomTransition *z) {
    return anim_active(&z->octaves);
}

float zoom_scale(const ZoomTransition *z) {
    // exp2f(0) is exactly 1, so a settled transition is exactly the identity
    // and the tile grid lands back on whole pixels.
    return exp2f(anim_value(&z->octaves));
}

static int clamp_zoom(int zoom) {
    if (zoom < MIN_ZOOM)
        return MIN_ZOOM;
    if (zoom > MAX_ZOOM)
        return MAX_ZOOM;
    return zoom;
}

int zoom_step(ZoomTransition *z, int *zoom, int *world_x, int *world_y, int steps,
              float cursor_x, float cursor_y, int window_width, int window_height) {
    const int new_zoom = clamp_zoom(*zoom + steps);
    const int applied = new_zoom - *zoom;
    if (applied == 0)
        return 0;

    if (!zoom_active(z)) {
        z->anchor_x = cursor_x;
        z->anchor_y = cursor_y;
    }

    // Move the centre so the world point under the anchor stays under it. The
    // window centre is taken as an integer because that is what the projection
    // in map_world_to_screen does, and at zoom 5 half a screen pixel is
    // sixteen thousand world pixels.
    const double per_pixel_before = map_world_per_pixel_at(*zoom);
    const double per_pixel_after = map_world_per_pixel_at(new_zoom);
    const double travel = per_pixel_before - per_pixel_after;
    *world_x += (int)(((double)z->anchor_x - window_width / 2) * travel);
    *world_y += (int)(((double)z->anchor_y - window_height / 2) * travel);
    *zoom = new_zoom;

    // The picture falls back by exactly what the model just gained, so the
    // frame after this call is the frame before it, then eases to nothing.
    float octaves = anim_value(&z->octaves) - (float)applied;

    // Without this a burst of detents would reach a scale of 1/32, and
    // map_visible_tiles would ask for a thousand times the tiles. Held to one
    // level, the burst snaps through the levels in between and eases only the
    // last -- which is how a fast scroll should read anyway.
    if (octaves < -ZOOM_MAX_OCTAVES)
        octaves = -ZOOM_MAX_OCTAVES;
    if (octaves > ZOOM_MAX_OCTAVES)
        octaves = ZOOM_MAX_OCTAVES;

    // anim_set leaves the target equal to the value, so the anim_to below
    // always sees a target it does not already have and never no-ops.
    anim_set(&z->octaves, octaves);
    anim_to(&z->octaves, 0.0f, ZOOM_ANIMATION_SECONDS, ANIM_EASE_OUT);

    return applied;
}

bool zoom_tick(ZoomTransition *z, float dt) {
    return anim_tick(&z->octaves, dt);
}
