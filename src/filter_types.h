#ifndef FILTER_TYPES_H
#define FILTER_TYPES_H

#include <stdbool.h>
#include <stddef.h>

// Filter identifiers. The low bits select which attribute a filter edits; the
// two high bits select which end of its range, so a field is addressed as
// e.g. FILTER_DISTANCE | HIGH_LIMIT.
#define FILTER_DISTANCE 0b0000000000000001
#define FILTER_DATE 0b0000000000000010
#define FILTER_DURATION 0b0000000000000100
#define FILTER_UPHILL 0b0000000000001000
#define FILTER_DOWNHILL 0b0000000000010000
#define FILTER_PEAK 0b0000000000100000
#define FILTER_PACE 0b0000000001000000
#define HIGH_LIMIT 0b1000000000000000
#define LOW_LIMIT 0b0100000000000000

// Every filter string is edited through the same fixed-size buffer.
#define FILTER_TEXT_SIZE 16

// Each filter keeps both the parsed numeric bound and the display string the
// UI edits in place. An empty string means "no bound set".
typedef struct FilterSettings {
    char start_date_str[FILTER_TEXT_SIZE];        // Display version: "24.08.2025"
    char end_date_str[FILTER_TEXT_SIZE];          // Display version: "24.08.2025"
    char start_date_str_filter[FILTER_TEXT_SIZE]; // Parsed lower bound, or the 01.01.1980 default
    char end_date_str_filter[FILTER_TEXT_SIZE];   // Parsed upper bound, or the 01.01.9000 default

    float duration_secs_high;
    char duration_high_str[FILTER_TEXT_SIZE];
    float duration_secs_low;
    char duration_low_str[FILTER_TEXT_SIZE];

    float distance_high;
    char distance_high_str[FILTER_TEXT_SIZE];
    float distance_low;
    char distance_low_str[FILTER_TEXT_SIZE];

    float secs_per_km_high;
    char pace_high_str[FILTER_TEXT_SIZE];
    float secs_per_km_low;
    char pace_low_str[FILTER_TEXT_SIZE];

    float elev_up_high;
    char elev_up_high_str[FILTER_TEXT_SIZE];
    float elev_up_low;
    char elev_up_low_str[FILTER_TEXT_SIZE];

    float elev_down_high;
    char elev_down_high_str[FILTER_TEXT_SIZE];
    float elev_down_low;
    char elev_down_low_str[FILTER_TEXT_SIZE];

    float high_point_high;
    char high_point_high_str[FILTER_TEXT_SIZE];
    float high_point_low;
    char high_point_low_str[FILTER_TEXT_SIZE];

    bool showRuns;
    bool showCycling;
    bool showHikes;
    bool showOther;
} FilterSettings;

#endif
