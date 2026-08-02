#ifndef TRACK_SPLITS_H
#define TRACK_SPLITS_H

#include <time.h>

// The distances a personal best is kept over, and the search that finds the
// fastest stretch of a track covering one. Split out from gpx_types.h so
// GpxTrack can carry an array of them without the type header knowing how they
// are found -- the same way gpx_activity.h carries the activity kinds.
//
// GpxTrack is named rather than defined here: gpx_types.h includes this file,
// so including it back would be a cycle. map_types.h forward-declares
// struct GpxPoint for the same reason.
struct GpxTrack;

typedef enum SplitDistance {
    SPLIT_5K = 0,
    SPLIT_10K,
    SPLIT_HALF_MARATHON, // 21.0975 km, the official distance rather than 21
    SPLIT_MARATHON,      // 42.195 km
    SPLIT_COUNT
} SplitDistance;

// The distance in metres, which is the unit partial_distance is in. Zero for a
// value that is not one of the four.
float track_splits_metres(SplitDistance split);

// Fills track->splits with the fastest time, in seconds, over each distance
// found anywhere in the track -- not only from its start, so the ten
// kilometres between the 5 and 15 km marks of a half marathon set a 10 k
// record. Zero means the track never covered that distance, or carried no
// usable times.
//
// `point_times` is one timestamp per point of track->points, (time_t)-1 where
// a point had none. It is the parser's scratch and is not retained.
// track_calculate_distance must have run first: this reads partial_distance.
void track_splits_compute(struct GpxTrack *track, const time_t *point_times);

#endif
