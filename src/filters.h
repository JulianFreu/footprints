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
// The editable text of one end of one range.
char *filter_bound_text(FilterSettings *filters, FilterAttribute attribute,
                        FilterBoundEnd end);

// Re-reads every field's text into its numeric bound.
void save_filter_values(FilterSettings *filters);
// Clears every field and shows every activity type.
void reset_filters(FilterSettings *filters);
// Recomputes visible_in_list for every track, and the "Shown: n of m" label.
void apply_filter_values(GpxCollection *collection);

#endif
