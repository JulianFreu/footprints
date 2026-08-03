#ifndef FILTERS_H
#define FILTERS_H

#include <stdbool.h>
#include <stdint.h>

#include "filter_types.h"
#include "gpx_types.h"

// How a filter field's digits are laid out as they are typed, and how the
// finished string is turned back into a bound. One value per shape of input,
// not one per filter, so filters that read the same way share it.
typedef enum {
    FILTER_FORMAT_DATE,      // DD.MM.YYYY
    FILTER_FORMAT_DISTANCE,  // KM.HH, two implied decimals
    FILTER_FORMAT_DURATION,  // HH:MM:SS
    FILTER_FORMAT_PACE,      // MM:SS per km
    FILTER_FORMAT_ELEVATION, // plain metres
} FilterFormat;

// Clay identifies elements, and carries hover user data, as a single integer.
// These pack an (attribute, end) pair into one and take it apart again.
uint16_t filter_field_id(FilterAttribute attribute, FilterBoundEnd end);
bool filter_field_unpack(uint16_t id, FilterAttribute *attribute, FilterBoundEnd *end);

// Label shown between a filter's two input fields.
const char *filter_display_name(FilterAttribute attribute);
// How this attribute's fields are laid out as they are typed.
FilterFormat filter_format(FilterAttribute attribute);
// What one end of one range shows, and the digits behind it. Both are reads:
// a field is changed through the four calls below, which is what keeps the
// text, the digits and the numeric bound in step.
const char *filter_bound_text(const FilterSettings *filters,
                              FilterAttribute attribute, FilterBoundEnd end);
const char *filter_bound_digits(const FilterSettings *filters,
                                FilterAttribute attribute, FilterBoundEnd end);

// Editing one end of one range. Each rewrites the field's text and its numeric
// bound from the digits, so neither can be left stale. A digit past what the
// format holds is dropped, and a backspace on an empty field does nothing.
void filter_bound_push_digit(FilterSettings *filters, FilterAttribute attribute,
                             FilterBoundEnd end, char digit);
void filter_bound_backspace(FilterSettings *filters, FilterAttribute attribute,
                            FilterBoundEnd end);
// Replaces the field with `digits`, ignoring anything in it that is not one.
// How a field is restored after an edit is abandoned.
void filter_bound_set_digits(FilterSettings *filters, FilterAttribute attribute,
                             FilterBoundEnd end, const char *digits);
void filter_bound_clear(FilterSettings *filters, FilterAttribute attribute,
                        FilterBoundEnd end);

// Clears every field and shows every activity type.
void reset_filters(FilterSettings *filters);
// Recomputes visible_in_list for every track, and the "Shown: n of m" label.
void apply_filter_values(GpxCollection *collection);

#endif
