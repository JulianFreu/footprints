#ifndef GPX_ACTIVITY_H
#define GPX_ACTIVITY_H

// The activity kinds a track can have. Split out from gpx_types.h so the
// filter types can index an array by activity without pulling in the whole
// track definition.
typedef enum {
    Run,
    Hike,
    Cycling,
    Other,
    ACTIVITY_TYPE_COUNT
} ActivityType;

// The one place activity types are spelled for display.
const char *activity_type_label(ActivityType type);

#endif
