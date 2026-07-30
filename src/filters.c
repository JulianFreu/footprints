#include "filters.h"

#include "time_util.h"

#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void reverse_chars(const char *str, char *rv_str, int size) {
    for (int i = 0; i < size; i++) {
        rv_str[i] = str[size - 1 - i];
    }
    for (int i = size; i < FILTER_TEXT_SIZE; i++) {
        rv_str[i] = '\0';
    }
}

void apply_filter_values(GpxCollection *c) {
    // Parse the two filter bounds once. These were re-parsed inside the loop,
    // once per track per call, and each failed parse logged to stderr.
    const time_t range_start = european_date_to_utc(c->filters.start_date_str_filter);
    const time_t range_end = european_date_to_utc(c->filters.end_date_str_filter);

    for (int i = 0; i < c->total_tracks; i++) {
        // default: visible
        c->tracks[i].visible_in_list = true;

        // check type
        if (c->tracks[i].act_type == Run && c->filters.showRuns == false)
            c->tracks[i].visible_in_list = false;
        if (c->tracks[i].act_type == Cycling && c->filters.showCycling == false)
            c->tracks[i].visible_in_list = false;
        if (c->tracks[i].act_type == Hike && c->filters.showHikes == false)
            c->tracks[i].visible_in_list = false;
        if (c->tracks[i].act_type == Other && c->filters.showOther == false)
            c->tracks[i].visible_in_list = false;

        // check limits
        if (c->tracks[i].distance < c->filters.distance_low || c->tracks[i].distance > c->filters.distance_high)
            c->tracks[i].visible_in_list = false;
        if (c->tracks[i].duration_secs < c->filters.duration_secs_low || c->tracks[i].duration_secs > c->filters.duration_secs_high)
            c->tracks[i].visible_in_list = false;
        if (c->tracks[i].secs_per_km < c->filters.secs_per_km_low || c->tracks[i].secs_per_km > c->filters.secs_per_km_high)
            c->tracks[i].visible_in_list = false;
        if ((c->tracks[i].elev_up - c->filters.elev_up_low) < -0.01 || c->tracks[i].elev_up - c->filters.elev_up_high > 0.01)
            c->tracks[i].visible_in_list = false;
        if (c->tracks[i].elev_down - c->filters.elev_down_low < -0.01 || c->tracks[i].elev_down - c->filters.elev_down_high > 0.01)
            c->tracks[i].visible_in_list = false;
        if (c->tracks[i].high_point - c->filters.high_point_low < -0.01 || c->tracks[i].high_point - c->filters.high_point_high > 0.01)
            c->tracks[i].visible_in_list = false;
        if (iso8601_to_utc(c->tracks[i].start_time_raw) < range_start ||
            iso8601_to_utc(c->tracks[i].end_time_raw) > range_end)
            c->tracks[i].visible_in_list = false;
    }
    int counter = 0;
    for (int i = 0; i < c->total_tracks; i++) {
        if (c->tracks[i].visible_in_list)
            counter++;
    }
    snprintf(c->total_visible_tracks_str, sizeof(c->total_visible_tracks_str), "Shown: %d of %d Tracks", counter, c->total_tracks);
}

static int digit(char c) {
    return (c == '\0') ? 0 : (int)c - (int)'0';
}

static float duration_str_to_duration_float(const char *str) {
    char rv_str[FILTER_TEXT_SIZE];
    reverse_chars(str, rv_str, (int)strlen(str));
    return digit(rv_str[0]) +
           10 * digit(rv_str[1]) +
           60 * digit(rv_str[3]) +
           600 * digit(rv_str[4]) +
           60 * 60 * digit(rv_str[6]) +
           60 * 600 * digit(rv_str[7]) +
           60 * 6000 * digit(rv_str[8]);
}

static float pace_str_to_pace_float(const char *str) {
    char rv_str[FILTER_TEXT_SIZE];
    reverse_chars(str, rv_str, (int)strlen(str));
    return digit(rv_str[0]) +
           10 * digit(rv_str[1]) +
           60 * digit(rv_str[3]) +
           10 * 60 * digit(rv_str[4]) +
           100 * 60 * digit(rv_str[5]) +
           1000 * 60 * digit(rv_str[6]);
}

// Every editable filter field, in no particular order -- lookup is by id.
// NO_BOUND marks the slot a given kind of filter does not use.
#define NO_BOUND ((size_t) - 1)
#define TEXT(field) offsetof(FilterSettings, field)

static const FilterField filter_fields[] = {
    {FILTER_DATE | LOW_LIMIT, FILTER_FORMAT_DATE, TEXT(start_date_str), NO_BOUND, TEXT(start_date_str_filter), "01.01.1980"},
    {FILTER_DATE | HIGH_LIMIT, FILTER_FORMAT_DATE, TEXT(end_date_str), NO_BOUND, TEXT(end_date_str_filter), "01.01.9000"},

    {FILTER_DISTANCE | LOW_LIMIT, FILTER_FORMAT_DISTANCE, TEXT(distance_low_str), TEXT(distance_low), NO_BOUND, NULL},
    {FILTER_DISTANCE | HIGH_LIMIT, FILTER_FORMAT_DISTANCE, TEXT(distance_high_str), TEXT(distance_high), NO_BOUND, NULL},

    {FILTER_DURATION | LOW_LIMIT, FILTER_FORMAT_DURATION, TEXT(duration_low_str), TEXT(duration_secs_low), NO_BOUND, NULL},
    {FILTER_DURATION | HIGH_LIMIT, FILTER_FORMAT_DURATION, TEXT(duration_high_str), TEXT(duration_secs_high), NO_BOUND, NULL},

    {FILTER_PACE | LOW_LIMIT, FILTER_FORMAT_PACE, TEXT(pace_low_str), TEXT(secs_per_km_low), NO_BOUND, NULL},
    {FILTER_PACE | HIGH_LIMIT, FILTER_FORMAT_PACE, TEXT(pace_high_str), TEXT(secs_per_km_high), NO_BOUND, NULL},

    {FILTER_UPHILL | LOW_LIMIT, FILTER_FORMAT_ELEVATION, TEXT(elev_up_low_str), TEXT(elev_up_low), NO_BOUND, NULL},
    {FILTER_UPHILL | HIGH_LIMIT, FILTER_FORMAT_ELEVATION, TEXT(elev_up_high_str), TEXT(elev_up_high), NO_BOUND, NULL},

    {FILTER_DOWNHILL | LOW_LIMIT, FILTER_FORMAT_ELEVATION, TEXT(elev_down_low_str), TEXT(elev_down_low), NO_BOUND, NULL},
    {FILTER_DOWNHILL | HIGH_LIMIT, FILTER_FORMAT_ELEVATION, TEXT(elev_down_high_str), TEXT(elev_down_high), NO_BOUND, NULL},

    {FILTER_PEAK | LOW_LIMIT, FILTER_FORMAT_ELEVATION, TEXT(high_point_low_str), TEXT(high_point_low), NO_BOUND, NULL},
    {FILTER_PEAK | HIGH_LIMIT, FILTER_FORMAT_ELEVATION, TEXT(high_point_high_str), TEXT(high_point_high), NO_BOUND, NULL},
};

#undef TEXT

#define FILTER_FIELD_COUNT (sizeof(filter_fields) / sizeof(filter_fields[0]))

const FilterField *filter_field_lookup(uint16_t id) {
    for (size_t i = 0; i < FILTER_FIELD_COUNT; i++) {
        if (filter_fields[i].id == id)
            return &filter_fields[i];
    }
    return NULL;
}

const char *filter_display_name(uint16_t filter_id) {
    switch (filter_id) {
    case FILTER_DISTANCE:
        return "distance";
    case FILTER_DURATION:
        return "duration";
    case FILTER_PACE:
        return "pace";
    case FILTER_DATE:
        return "date";
    case FILTER_UPHILL:
        return "up";
    case FILTER_DOWNHILL:
        return "down";
    case FILTER_PEAK:
        return "peak";
    default:
        return "";
    }
}

char *filter_field_text(FilterSettings *filter, const FilterField *field) {
    return (char *)filter + field->text_offset;
}

static float *filter_field_value(FilterSettings *filter, const FilterField *field) {
    return (float *)(void *)((char *)filter + field->value_offset);
}

// Turns a field's typed text into its numeric bound.
static float parse_filter_text(const char *text, FilterFormat format) {
    switch (format) {
    case FILTER_FORMAT_DURATION:
        return duration_str_to_duration_float(text);
    case FILTER_FORMAT_PACE:
        return pace_str_to_pace_float(text);
    case FILTER_FORMAT_DISTANCE:
    case FILTER_FORMAT_ELEVATION:
        return (float)atof(text);
    case FILTER_FORMAT_DATE:
        break;
    }
    return 0.0f;
}

// A blank field means "no bound", which for the low end is -FLT_MAX and for the
// high end +FLT_MAX.
static bool field_is_high_limit(const FilterField *field) {
    return (field->id & HIGH_LIMIT) != 0;
}

void save_filter_values(FilterSettings *filter) {
    for (size_t i = 0; i < FILTER_FIELD_COUNT; i++) {
        const FilterField *field = &filter_fields[i];
        const char *text = filter_field_text(filter, field);

        if (field->format == FILTER_FORMAT_DATE) {
            char *bound = (char *)filter + field->date_offset;
            snprintf(bound, FILTER_TEXT_SIZE, "%s",
                     text[0] != '\0' ? text : field->empty_default);
            continue;
        }

        *filter_field_value(filter, field) =
            text[0] != '\0' ? parse_filter_text(text, field->format)
                            : (field_is_high_limit(field) ? FLT_MAX : -FLT_MAX);
    }
}

void reset_filters(FilterSettings *filter) {
    for (size_t i = 0; i < FILTER_FIELD_COUNT; i++)
        filter_field_text(filter, &filter_fields[i])[0] = '\0';

    filter->showCycling = true;
    filter->showOther = true;
    filter->showRuns = true;
    filter->showHikes = true;
    save_filter_values(filter);
}
