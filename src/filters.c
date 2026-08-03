#include "filters.h"

#include "time_util.h"

#include <float.h>
#include <stddef.h>
#include <stdio.h>
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

const char *filter_bound_text(const FilterSettings *filters,
                              FilterAttribute attribute, FilterBoundEnd end) {
    return filters->bound[attribute][end].text;
}

const char *filter_bound_digits(const FilterSettings *filters,
                                FilterAttribute attribute, FilterBoundEnd end) {
    return filters->bound[attribute][end].digits;
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

// --- Typed digits, and what they add up to ---
//
// Every field but the date is a number written in mixed units: the digits are
// grouped from the least significant end and each group is weighted. 5445
// seconds is 1:30:45 because the last two digits are worth one apiece, the two
// before them sixty, and what is left thirty-six hundred. Splitting the digits
// once gives both the string the field shows and the bound the predicate
// compares, which is why the two cannot disagree.
//
// The date is not a number at all -- its digits read left to right and its
// value comes from the calendar -- so it is the one shape handled on its own.

// Fields per format. Nothing needs more than the three of HH:MM:SS.
#define FILTER_GROUP_MAX 3

typedef struct FilterFormatSpec {
    int max_digits;
    // Least significant group first; a zero ends the list.
    int group_digits[FILTER_GROUP_MAX];
    double group_weight[FILTER_GROUP_MAX];
    char separator;
    // Distance keeps its two decimals whatever has been typed, so one digit
    // reads as 0.05 rather than as 5. The clock formats grow a field at a time
    // instead, and show nothing they have not reached.
    bool show_empty_groups;
} FilterFormatSpec;

static const FilterFormatSpec format_specs[] = {
    [FILTER_FORMAT_DATE] = {8, {0}, {0}, '.', false},
    [FILTER_FORMAT_DISTANCE] = {6, {2, 4}, {0.01, 1.0}, '.', true},
    [FILTER_FORMAT_DURATION] = {6, {2, 2, 2}, {1.0, 60.0, 3600.0}, ':', false},
    [FILTER_FORMAT_PACE] = {4, {2, 2}, {1.0, 60.0}, ':', false},
    [FILTER_FORMAT_ELEVATION] = {5, {5}, {1.0}, '\0', false},
};

// Where each group's digits sit inside the typed string, least significant
// first, and how many groups have any digits at all. Filling from the right is
// what makes a field grow the way a number does: the digit typed last is
// always the smallest one.
static int split_groups(const FilterFormatSpec *spec, int length,
                        int *start, int *count) {
    int remaining = length;
    int used = 0;
    for (int g = 0; g < FILTER_GROUP_MAX && spec->group_digits[g] > 0; g++) {
        int take = (spec->group_digits[g] < remaining) ? spec->group_digits[g] : remaining;
        remaining -= take;
        start[g] = remaining;
        count[g] = take;
        if (take > 0)
            used = g + 1;
    }
    return used;
}

static void build_number_text(char *out, size_t out_size, const char *digits,
                              const FilterFormatSpec *spec) {
    int start[FILTER_GROUP_MAX] = {0};
    int count[FILTER_GROUP_MAX] = {0};
    int groups = split_groups(spec, (int)strlen(digits), start, count);

    if (groups == 0) {
        out[0] = '\0';
        return;
    }

    if (spec->show_empty_groups) {
        groups = 0;
        while (groups < FILTER_GROUP_MAX && spec->group_digits[groups] > 0)
            groups++;
    }

    size_t at = 0;
    for (int g = groups - 1; g >= 0 && at + 1 < out_size; g--) {
        bool leading = (g == groups - 1);
        if (!leading && spec->separator != '\0')
            out[at++] = spec->separator;

        // Any group under the leading one is full, either because the digits
        // reached it or because it is being shown as zeroes. The leading one
        // is as wide as what was typed, and reads "0" when that is nothing.
        int width = leading ? (count[g] > 0 ? count[g] : 1) : spec->group_digits[g];
        for (int pad = width - count[g]; pad > 0 && at + 1 < out_size; pad--)
            out[at++] = '0';
        for (int i = 0; i < count[g] && at + 1 < out_size; i++)
            out[at++] = digits[start[g] + i];
    }
    out[at] = '\0';
}

static double number_value(const char *digits, const FilterFormatSpec *spec) {
    int start[FILTER_GROUP_MAX] = {0};
    int count[FILTER_GROUP_MAX] = {0};
    split_groups(spec, (int)strlen(digits), start, count);

    double value = 0.0;
    for (int g = 0; g < FILTER_GROUP_MAX && spec->group_digits[g] > 0; g++) {
        int group = 0;
        for (int i = 0; i < count[g]; i++)
            group = group * 10 + (digits[start[g] + i] - '0');
        value += (double)group * spec->group_weight[g];
    }
    return value;
}

// DD.MM.YYYY. A date is read left to right, so the first two digits are the
// day however few have arrived after them.
static void build_date_text(char *out, size_t out_size, const char *digits) {
    size_t at = 0;
    for (size_t i = 0; digits[i] != '\0' && at + 2 < out_size; i++) {
        if (i == 2 || i == 4)
            out[at++] = '.';
        out[at++] = digits[i];
    }
    out[at] = '\0';
}

// An unset bound admits everything, so it sits at the end of the range it
// bounds rather than at some arbitrary sentinel date.
static double open_bound(FilterBoundEnd end) {
    return (end == BOUND_HIGH) ? DBL_MAX : -DBL_MAX;
}

// Rewrites what a field shows and what it compares, from the digits in it.
// Every edit ends here, which is the whole of why the three stay in step.
static void refresh_bound(FilterSettings *filters, FilterAttribute attribute,
                          FilterBoundEnd end) {
    FilterBound *bound = &filters->bound[attribute][end];
    const FilterFormatSpec *spec = &format_specs[filter_fields[attribute].format];

    if (filter_fields[attribute].format == FILTER_FORMAT_DATE) {
        build_date_text(bound->text, sizeof(bound->text), bound->digits);
        // Half-typed dates are normal: "24.08" is not a date yet, and a field
        // that does not name an instant bounds nothing.
        time_t parsed = (strlen(bound->digits) == (size_t)spec->max_digits)
                            ? european_date_to_utc(bound->text)
                            : (time_t)-1;
        bound->value = (parsed == (time_t)-1) ? open_bound(end) : (double)parsed;
        return;
    }

    build_number_text(bound->text, sizeof(bound->text), bound->digits, spec);
    bound->value = (bound->digits[0] == '\0') ? open_bound(end)
                                              : number_value(bound->digits, spec);
}

void filter_bound_push_digit(FilterSettings *filters, FilterAttribute attribute,
                             FilterBoundEnd end, char digit) {
    FilterBound *bound = &filters->bound[attribute][end];
    size_t length = strlen(bound->digits);
    // A full field keeps what it has. It is already showing its whole pattern,
    // so it reads as full rather than as broken.
    if ((int)length >= format_specs[filter_fields[attribute].format].max_digits)
        return;

    bound->digits[length] = digit;
    bound->digits[length + 1] = '\0';
    refresh_bound(filters, attribute, end);
}

void filter_bound_backspace(FilterSettings *filters, FilterAttribute attribute,
                            FilterBoundEnd end) {
    FilterBound *bound = &filters->bound[attribute][end];
    size_t length = strlen(bound->digits);
    if (length > 0)
        bound->digits[length - 1] = '\0';
    refresh_bound(filters, attribute, end);
}

void filter_bound_set_digits(FilterSettings *filters, FilterAttribute attribute,
                             FilterBoundEnd end, const char *digits) {
    FilterBound *bound = &filters->bound[attribute][end];
    int max_digits = format_specs[filter_fields[attribute].format].max_digits;

    size_t at = 0;
    for (size_t i = 0; digits[i] != '\0' && (int)at < max_digits; i++) {
        if (digits[i] >= '0' && digits[i] <= '9')
            bound->digits[at++] = digits[i];
    }
    bound->digits[at] = '\0';
    refresh_bound(filters, attribute, end);
}

void filter_bound_clear(FilterSettings *filters, FilterAttribute attribute,
                        FilterBoundEnd end) {
    filters->bound[attribute][end].digits[0] = '\0';
    refresh_bound(filters, attribute, end);
}

void reset_filters(FilterSettings *filters) {
    for (int attribute = 0; attribute < FILTER_COUNT; attribute++)
        for (int end = 0; end < BOUND_COUNT; end++)
            filter_bound_clear(filters, (FilterAttribute)attribute, (FilterBoundEnd)end);

    for (int type = 0; type < ACTIVITY_TYPE_COUNT; type++)
        filters->show_activity[type] = true;
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
