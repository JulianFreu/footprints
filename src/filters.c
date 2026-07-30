#include "filters.h"

#include "time_util.h"

#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Every filter, in the order of the FilterAttribute enum.
//
// A row says how the field is typed, which GpxTrack member each end of the
// range is compared against, and what the label between the two inputs reads.
// Everything else -- drawing the pair of inputs, parsing the text, deciding
// which tracks survive -- is driven from here, so a new filter is a value in
// the enum and a row in this table.
typedef struct FilterField {
    FilterFormat format;
    // Which member the low and high ends are compared against. Almost always
    // the same one; the date filter is the exception, where the low bound
    // tests when a track started and the high bound when it ended.
    size_t low_member;
    size_t high_member;
    // Track members are float except the timestamps, which are time_t.
    bool member_is_time;
    const char *display_name;
} FilterField;

#define MEMBER(field) offsetof(GpxTrack, field)

static const FilterField filter_fields[FILTER_COUNT] = {
    [FILTER_DISTANCE] = {FILTER_FORMAT_DISTANCE, MEMBER(distance), MEMBER(distance), false, "distance"},
    [FILTER_DURATION] = {FILTER_FORMAT_DURATION, MEMBER(duration_secs), MEMBER(duration_secs), false, "duration"},
    [FILTER_PACE] = {FILTER_FORMAT_PACE, MEMBER(secs_per_km), MEMBER(secs_per_km), false, "pace"},
    [FILTER_DATE] = {FILTER_FORMAT_DATE, MEMBER(start_utc), MEMBER(end_utc), true, "date"},
    [FILTER_UPHILL] = {FILTER_FORMAT_ELEVATION, MEMBER(elev_up), MEMBER(elev_up), false, "up"},
    [FILTER_DOWNHILL] = {FILTER_FORMAT_ELEVATION, MEMBER(elev_down), MEMBER(elev_down), false, "down"},
    [FILTER_PEAK] = {FILTER_FORMAT_ELEVATION, MEMBER(high_point), MEMBER(high_point), false, "peak"},
};

#undef MEMBER

static bool attribute_is_valid(FilterAttribute attribute) {
    return attribute >= 0 && attribute < FILTER_COUNT;
}

uint16_t filter_field_id(FilterAttribute attribute, FilterBoundEnd end) {
    return (uint16_t)(attribute * BOUND_COUNT + end);
}

bool filter_field_unpack(uint16_t id, FilterAttribute *attribute, FilterBoundEnd *end) {
    FilterAttribute a = (FilterAttribute)(id / BOUND_COUNT);
    if (!attribute_is_valid(a))
        return false;
    *attribute = a;
    *end = (FilterBoundEnd)(id % BOUND_COUNT);
    return true;
}

const char *filter_display_name(FilterAttribute attribute) {
    return attribute_is_valid(attribute) ? filter_fields[attribute].display_name : "";
}

FilterFormat filter_format(FilterAttribute attribute) {
    return filter_fields[attribute].format;
}

char *filter_bound_text(FilterSettings *filters, FilterAttribute attribute,
                        FilterBoundEnd end) {
    return filters->bound[attribute][end].text;
}

// Reads the GpxTrack member a filter row points at, whatever its type.
static double track_member_value(const GpxTrack *track, size_t member_offset,
                                 bool member_is_time) {
    const char *base = (const char *)track;
    if (member_is_time) {
        time_t value;
        memcpy(&value, base + member_offset, sizeof(value));
        return (double)value;
    }
    float value;
    memcpy(&value, base + member_offset, sizeof(value));
    return (double)value;
}

// Elevation is shown rounded to whole metres, so a bound typed as "500" has to
// admit a track whose smoothed gain came out at 499.9997. The other filters
// are compared against what the user sees to full precision.
static double comparison_epsilon(FilterFormat format) {
    return (format == FILTER_FORMAT_ELEVATION) ? 0.01 : 0.0;
}

static int digit(char c) {
    return (c == '\0') ? 0 : (int)c - (int)'0';
}

static void reverse_chars(const char *str, char *rv_str, int size) {
    for (int i = 0; i < size; i++) {
        rv_str[i] = str[size - 1 - i];
    }
    for (int i = size; i < FILTER_TEXT_SIZE; i++) {
        rv_str[i] = '\0';
    }
}

static double duration_str_to_seconds(const char *str) {
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

static double pace_str_to_seconds(const char *str) {
    char rv_str[FILTER_TEXT_SIZE];
    reverse_chars(str, rv_str, (int)strlen(str));
    return digit(rv_str[0]) +
           10 * digit(rv_str[1]) +
           60 * digit(rv_str[3]) +
           10 * 60 * digit(rv_str[4]) +
           100 * 60 * digit(rv_str[5]) +
           1000 * 60 * digit(rv_str[6]);
}

// Turns a field's typed text into its numeric bound. Dates become a UTC
// timestamp, so the date filter compares numbers like every other filter
// rather than keeping a second copy of the bound as a string.
static double parse_filter_text(const char *text, FilterFormat format, bool *ok) {
    *ok = true;
    switch (format) {
    case FILTER_FORMAT_DURATION:
        return duration_str_to_seconds(text);
    case FILTER_FORMAT_PACE:
        return pace_str_to_seconds(text);
    case FILTER_FORMAT_DISTANCE:
    case FILTER_FORMAT_ELEVATION:
        return atof(text);
    case FILTER_FORMAT_DATE: {
        time_t parsed = european_date_to_utc(text);
        if (parsed == (time_t)-1) {
            // Half-typed dates are normal: the field is reparsed on the way
            // out of input mode, and "24.08" is not a date yet.
            *ok = false;
            return 0.0;
        }
        return (double)parsed;
    }
    }
    *ok = false;
    return 0.0;
}

// An unset bound admits everything, so it sits at the end of the range it
// bounds rather than at some arbitrary sentinel date.
static double open_bound(FilterBoundEnd end) {
    return (end == BOUND_HIGH) ? DBL_MAX : -DBL_MAX;
}

void save_filter_values(FilterSettings *filters) {
    for (int attribute = 0; attribute < FILTER_COUNT; attribute++) {
        for (int end = 0; end < BOUND_COUNT; end++) {
            FilterBound *bound = &filters->bound[attribute][end];
            if (bound->text[0] == '\0') {
                bound->value = open_bound((FilterBoundEnd)end);
                continue;
            }

            bool ok = false;
            double parsed = parse_filter_text(bound->text,
                                              filter_fields[attribute].format, &ok);
            bound->value = ok ? parsed : open_bound((FilterBoundEnd)end);
        }
    }
}

void reset_filters(FilterSettings *filters) {
    for (int attribute = 0; attribute < FILTER_COUNT; attribute++)
        for (int end = 0; end < BOUND_COUNT; end++)
            filters->bound[attribute][end].text[0] = '\0';

    for (int type = 0; type < ACTIVITY_TYPE_COUNT; type++)
        filters->show_activity[type] = true;

    save_filter_values(filters);
}

// A track survives when its activity type is shown and every filter's range
// contains it. Both loops are driven by the table, so a new filter needs
// nothing here.
static bool track_passes_filters(const GpxTrack *track, const FilterSettings *filters) {
    if (track->act_type >= 0 && track->act_type < ACTIVITY_TYPE_COUNT &&
        !filters->show_activity[track->act_type])
        return false;

    for (int attribute = 0; attribute < FILTER_COUNT; attribute++) {
        const FilterField *field = &filter_fields[attribute];
        double epsilon = comparison_epsilon(field->format);

        double low = filters->bound[attribute][BOUND_LOW].value;
        if (low != -DBL_MAX) {
            double value = track_member_value(track, field->low_member, field->member_is_time);
            if (value - low < -epsilon)
                return false;
        }

        double high = filters->bound[attribute][BOUND_HIGH].value;
        if (high != DBL_MAX) {
            double value = track_member_value(track, field->high_member, field->member_is_time);
            if (value - high > epsilon)
                return false;
        }
    }
    return true;
}

void apply_filter_values(GpxCollection *collection) {
    int visible = 0;
    for (int i = 0; i < collection->total_tracks; i++) {
        collection->tracks[i].visible_in_list =
            track_passes_filters(&collection->tracks[i], &collection->filters);
        if (collection->tracks[i].visible_in_list)
            visible++;
    }

    snprintf(collection->total_visible_tracks_str,
             sizeof(collection->total_visible_tracks_str),
             "Shown: %d of %d Tracks", visible, collection->total_tracks);
}
