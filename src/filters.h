#ifndef filters_h
#define filters_h

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

// One editable end of one filter. The table in filters.c holds every field, so
// adding a filter is a row there plus a FILTER_* bit in ui_types.h -- rather
// than a new case in the draw switch, another block in save_filter_values, and
// another line in reset_filters.
typedef struct FilterField {
    uint16_t id; // FILTER_* ored with LOW_LIMIT or HIGH_LIMIT
    FilterFormat format;
    size_t text_offset; // char[16] the UI edits, within FilterSettings
    // Numeric filters parse text_offset into this float bound. Date filters
    // leave it unused and write the parsed string to date_offset instead.
    size_t value_offset;
    size_t date_offset;
    const char *empty_default; // bound used when the field is left blank
} FilterField;

// Returns NULL for an id that is not an editable field.
const FilterField *filter_field_lookup(uint16_t id);
// The editable text of `field` within `filter`.
char *filter_field_text(FilterSettings *filter, const FilterField *field);
// Label shown between a filter's two input fields.
const char *filter_display_name(uint16_t filter_id);

void apply_filter_values(GpxCollection *c);
void reset_filters(FilterSettings *filter);
void save_filter_values(FilterSettings *filter);
#endif