#ifndef FILTER_TYPES_H
#define FILTER_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpx_activity.h"

// The attributes a track can be filtered on. Adding one means a value here and
// a row in the table in filters.c; nothing else has to change.
typedef enum {
    FILTER_DISTANCE,
    FILTER_DURATION,
    FILTER_PACE,
    FILTER_DATE,
    FILTER_UPHILL,
    FILTER_DOWNHILL,
    FILTER_PEAK,
    FILTER_COUNT
} FilterAttribute;

// Each filter is a range, so each has two editable ends.
typedef enum {
    BOUND_LOW,
    BOUND_HIGH,
    BOUND_COUNT
} FilterBoundEnd;

// Every filter string is edited through the same fixed-size buffer.
#define FILTER_TEXT_SIZE 16

// One end of one range: the text the UI edits in place, and the number it was
// parsed into. An empty string means "no bound", which parses to -/+DBL_MAX.
//
// The bound is a double rather than a float because the date filter stores a
// UTC timestamp here, and a float cannot hold one without losing days.
typedef struct FilterBound {
    char text[FILTER_TEXT_SIZE];
    double value;
} FilterBound;

typedef struct FilterSettings {
    FilterBound bound[FILTER_COUNT][BOUND_COUNT];
    bool show_activity[ACTIVITY_TYPE_COUNT];
} FilterSettings;

#endif
