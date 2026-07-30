#ifndef FILTER_TYPES_H
#define FILTER_TYPES_H

#include <stdbool.h>

// Each filter keeps both the parsed numeric bound and the display string the
// UI edits in place. An empty string means "no bound set".
typedef struct FilterSettings {
    char start_date_str[16];        // Display version: "24.08.2025"
    char end_date_str[16];          // Display version: "24.08.2025"
    char start_date_str_filter[16]; // Parsed lower bound, or the 01.01.1980 default
    char end_date_str_filter[16];   // Parsed upper bound, or the 01.01.9000 default

    char start_time_str[16]; // Display version: "15:02:15"
    char end_time_str[16];   // Display version: "15:02:15"

    float duration_secs_high;
    char duration_high_str[16];
    float duration_secs_low;
    char duration_low_str[16];

    float distance_high;
    char distance_high_str[16];
    float distance_low;
    char distance_low_str[16];

    float secs_per_km_high;
    char pace_high_str[16];
    float secs_per_km_low;
    char pace_low_str[16];

    float elev_up_high;
    char elev_up_high_str[16];
    float elev_up_low;
    char elev_up_low_str[16];

    float elev_down_high;
    char elev_down_high_str[16];
    float elev_down_low;
    char elev_down_low_str[16];

    float high_point_high;
    char high_point_high_str[16];
    float high_point_low;
    char high_point_low_str[16];

    float low_point_high;
    char low_point_high_str[16];
    float low_point_low;
    char low_point_low_str[16];

    bool showRuns;
    bool showCycling;
    bool showHikes;
    bool showOther;
} FilterSettings;

#endif
