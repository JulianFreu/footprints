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

// Every filter string is shown through the same fixed-size buffer. The longest
// is a date, "DD.MM.YYYY".
#define FILTER_TEXT_SIZE 16
// Digits a field accepts before it is full, which is again the date's eight.
#define FILTER_DIGITS_MAX 8

// One end of one range.
//
// The digits are what was typed and the only thing that is written: the text a
// field shows and the number the predicate compares are both derived from them,
// together, so the two cannot drift apart. No digits means "no bound", which
// reads as -/+DBL_MAX -- and so does a field whose digits do not add up to a
// value yet, which for the date filter is anything short of all eight.
//
// The bound is a double rather than a float because the date filter stores a
// UTC timestamp here, and a float cannot hold one without losing days.
typedef struct FilterBound {
    char digits[FILTER_DIGITS_MAX + 1];
    char text[FILTER_TEXT_SIZE];
    double value;
} FilterBound;

typedef struct FilterSettings {
    FilterBound bound[FILTER_COUNT][BOUND_COUNT];
    bool show_activity[ACTIVITY_TYPE_COUNT];
} FilterSettings;

#endif
