#ifndef TRACK_FORMAT_H
#define TRACK_FORMAT_H

#include <stddef.h>

#include "gpx_types.h"

// Turning a track's numbers into the strings shown next to them.
//
// These used to be nine char buffers on GpxTrack, filled by the parser at load
// time -- presentation baked into the domain type, and another buffer to add
// for every new statistic. The numbers live on the track; the strings are made
// where they are drawn.

typedef enum {
    TRACK_TEXT_DATE,       // 24.08.2025
    TRACK_TEXT_TIME,       // 15:02
    TRACK_TEXT_DURATION,   // 01:23:45
    TRACK_TEXT_DISTANCE,   // 12.34
    TRACK_TEXT_PACE,       // 5:30
    TRACK_TEXT_ELEV_UP,    // whole metres
    TRACK_TEXT_ELEV_DOWN,  // whole metres
    TRACK_TEXT_HIGH_POINT, // whole metres
    TRACK_TEXT_LOW_POINT,  // whole metres
} TrackText;

// Enough for any of the above, with room to spare.
#define TRACK_TEXT_MAX 24

// Writes the display form of one attribute into `out` and returns it, so the
// call reads inline at the point of use.
const char *track_format(const GpxTrack *track, TrackText field, char *out, size_t size);

#endif
