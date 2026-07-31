#ifndef ZOOM_H
#define ZOOM_H

#include <stdbool.h>

#include "anim.h"

// The gap between the zoom level the model is at and the one the picture is
// still showing.
//
// A wheel detent moves the model a whole level at once -- tiles, the on-disk
// cache, the point index and the heat rasterisation only ever see whole zooms.
// What eases is this gap, which the blit turns into a MapTransform. The model
// having already arrived is what keeps the transition out of everything except
// the last step of drawing.
//
// The gap is carried in octaves rather than as a scale because that is the
// space zoom is linear in: a second detent arriving mid-transition is then a
// subtraction, and the composed value is still a straight line to zero.

typedef struct ZoomTransition {
    // Powers of two the picture still owes the model. Negative while zooming
    // in -- the model is a level ahead, so the picture is drawn smaller until
    // it catches up. Zero at rest, and a zeroed struct is a settled one.
    Anim octaves;
    // The point the scaling turns about, in unscaled screen pixels. Held here
    // rather than passed per frame because it has to outlive the event that
    // set it: the same point must be used to re-centre the world, or the
    // picture moves on the frame the model changes.
    float anchor_x;
    float anchor_y;
} ZoomTransition;

// World pixels to one screen pixel at `zoom`. World coordinates are pixels at
// MAX_ZOOM, so this is the one shift the whole projection is built on. It lives
// here rather than in map.c because it is arithmetic rather than drawing, and
// because map.c cannot be reached without SDL.
int map_world_per_pixel_at(int zoom);

// Whether the picture still has a level to travel.
bool zoom_active(const ZoomTransition *z);

// The scale the map layers are drawn at right now. Exactly 1 at rest, so a
// settled transition costs the drawing nothing.
float zoom_scale(const ZoomTransition *z);

// Applies `steps` wheel detents about (cursor_x, cursor_y), given in screen
// pixels. Moves `*zoom` and the world centre immediately and starts the picture
// catching up.
//
// Returns how many levels were actually applied, which is zero at the MIN_ZOOM
// and MAX_ZOOM ends -- and in that case nothing is touched, including a
// transition already in flight, so scrolling into the end of the range does not
// keep restarting it.
//
// A transition already running keeps the anchor it started with: the anchor is
// also what the world centre is re-derived about, so adopting a new one
// mid-flight would shift the picture by (new - old) * (1 - scale). A new anchor
// is taken only from rest.
int zoom_step(ZoomTransition *z, int *zoom, int *world_x, int *world_y, int steps,
              float cursor_x, float cursor_y, int window_width, int window_height);

// Advances the transition by `dt` seconds. Returns whether the picture moved,
// which is what asks for another frame.
bool zoom_tick(ZoomTransition *z, float dt);

#endif
