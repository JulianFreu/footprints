#include "../src/filters.c"

#include "harness.h"

#include <float.h>

// Fills a filter field's text as if it had been typed, then reparses the whole
// table. save_filter_values is the only way text becomes a numeric bound.
static void set_field(FilterSettings *filters, uint16_t id, const char *text) {
    const FilterField *field = filter_field_lookup(id);
    if (!field) {
        FAIL("no filter field for id 0x%04x", id);
        return;
    }
    snprintf(filter_field_text(filters, field), FILTER_TEXT_SIZE, "%s", text);
}

void run_filters_tests(void) {
    SUITE("filters: duration text to seconds");
    CHECK_NEAR(duration_str_to_duration_float("00:00:01"), 1.0, 1e-6);
    CHECK_NEAR(duration_str_to_duration_float("00:01:00"), 60.0, 1e-6);
    CHECK_NEAR(duration_str_to_duration_float("01:00:00"), 3600.0, 1e-6);
    CHECK_NEAR(duration_str_to_duration_float("01:30:45"), 5445.0, 1e-6);
    CHECK_NEAR(duration_str_to_duration_float("10:00:00"), 36000.0, 1e-6);

    SUITE("filters: pace text to seconds per km");
    CHECK_NEAR(pace_str_to_pace_float("5:00"), 300.0, 1e-6);
    CHECK_NEAR(pace_str_to_pace_float("4:30"), 270.0, 1e-6);
    CHECK_NEAR(pace_str_to_pace_float("10:15"), 615.0, 1e-6);

    SUITE("filters: every table field is reachable and unique");
    // filter_field_lookup is a linear search over the table; a duplicated or
    // mistyped id would silently shadow a field and the UI would edit the
    // wrong bound.
    for (size_t i = 0; i < FILTER_FIELD_COUNT; i++) {
        const FilterField *field = &filter_fields[i];
        CHECK(filter_field_lookup(field->id) == field);
        // Each row addresses exactly one end of one range.
        CHECK(((field->id & HIGH_LIMIT) != 0) != ((field->id & LOW_LIMIT) != 0));
    }
    CHECK(filter_field_lookup(0) == NULL);
    CHECK(filter_field_lookup(0xFFFF) == NULL);

    SUITE("filters: reset clears text and opens both bounds");
    FilterSettings filters;
    reset_filters(&filters);
    CHECK(filters.show_runs && filters.show_hikes && filters.show_cycling &&
          filters.show_other);
    for (size_t i = 0; i < FILTER_FIELD_COUNT; i++) {
        const FilterField *field = &filter_fields[i];
        CHECK_STR(filter_field_text(&filters, field), "");
    }
    // A blank field means "no bound": -FLT_MAX at the low end, +FLT_MAX at the
    // high end, so nothing is filtered out.
    CHECK_NEAR(filters.distance_low, -FLT_MAX, 1.0);
    CHECK_NEAR(filters.distance_high, FLT_MAX, 1.0);
    CHECK_STR(filters.start_date_str_filter, "01.01.1980");
    CHECK_STR(filters.end_date_str_filter, "01.01.9000");

    SUITE("filters: typed bounds are parsed into the numeric fields");
    reset_filters(&filters);
    set_field(&filters, FILTER_DISTANCE | LOW_LIMIT, "5.00");
    set_field(&filters, FILTER_DISTANCE | HIGH_LIMIT, "10.00");
    set_field(&filters, FILTER_DURATION | LOW_LIMIT, "00:30:00");
    set_field(&filters, FILTER_PACE | HIGH_LIMIT, "6:00");
    save_filter_values(&filters);
    CHECK_NEAR(filters.distance_low, 5.0, 1e-4);
    CHECK_NEAR(filters.distance_high, 10.0, 1e-4);
    CHECK_NEAR(filters.duration_secs_low, 1800.0, 1e-4);
    CHECK_NEAR(filters.secs_per_km_high, 360.0, 1e-4);
    // Fields left blank keep their open bound.
    CHECK_NEAR(filters.elev_up_low, -FLT_MAX, 1.0);

    SUITE("filters: apply_filter_values on a synthetic collection");
    GpxTrack tracks[4] = {0};
    // id 0: 8 km run,  1 h,   pace 450 s/km, 100 m up, on 24.08.2025
    tracks[0].act_type = Run;
    tracks[0].distance = 8.0f;
    tracks[0].duration_secs = 3600.0f;
    tracks[0].secs_per_km = 450.0f;
    tracks[0].elev_up = 100.0f;
    tracks[0].start_utc = iso8601_to_utc("2025-08-24T10:00:00Z");
    tracks[0].end_utc = iso8601_to_utc("2025-08-24T11:00:00Z");
    // id 1: 3 km run -- below a 5 km lower bound
    tracks[1].act_type = Run;
    tracks[1].distance = 3.0f;
    tracks[1].duration_secs = 1200.0f;
    tracks[1].secs_per_km = 400.0f;
    tracks[1].start_utc = tracks[0].start_utc;
    tracks[1].end_utc = tracks[0].end_utc;
    // id 2: 8 km ride -- right distance, wrong activity type
    tracks[2].act_type = Cycling;
    tracks[2].distance = 8.0f;
    tracks[2].duration_secs = 3600.0f;
    tracks[2].secs_per_km = 200.0f;
    tracks[2].start_utc = tracks[0].start_utc;
    tracks[2].end_utc = tracks[0].end_utc;
    // id 3: 8 km run in a different year -- outside the date bounds
    tracks[3].act_type = Run;
    tracks[3].distance = 8.0f;
    tracks[3].duration_secs = 3600.0f;
    tracks[3].secs_per_km = 450.0f;
    tracks[3].start_utc = iso8601_to_utc("2019-01-05T10:00:00Z");
    tracks[3].end_utc = iso8601_to_utc("2019-01-05T11:00:00Z");

    GpxCollection collection = {0};
    collection.tracks = tracks;
    collection.total_tracks = 4;

    reset_filters(&collection.filters);
    apply_filter_values(&collection);
    // Nothing set: everything visible.
    for (int i = 0; i < 4; i++)
        CHECK(collection.tracks[i].visible_in_list);
    CHECK_STR(collection.total_visible_tracks_str, "Shown: 4 of 4 Tracks");

    // A distance floor drops the short one.
    reset_filters(&collection.filters);
    set_field(&collection.filters, FILTER_DISTANCE | LOW_LIMIT, "5.00");
    save_filter_values(&collection.filters);
    apply_filter_values(&collection);
    CHECK(collection.tracks[0].visible_in_list);
    CHECK(!collection.tracks[1].visible_in_list);
    CHECK(collection.tracks[2].visible_in_list);

    // An activity toggle drops the ride.
    reset_filters(&collection.filters);
    collection.filters.show_cycling = false;
    apply_filter_values(&collection);
    CHECK(collection.tracks[0].visible_in_list);
    CHECK(!collection.tracks[2].visible_in_list);

    // A date window drops the 2019 track and keeps the 2025 ones.
    reset_filters(&collection.filters);
    set_field(&collection.filters, FILTER_DATE | LOW_LIMIT, "01.01.2025");
    set_field(&collection.filters, FILTER_DATE | HIGH_LIMIT, "31.12.2025");
    save_filter_values(&collection.filters);
    apply_filter_values(&collection);
    CHECK(collection.tracks[0].visible_in_list);
    CHECK(!collection.tracks[3].visible_in_list);
    CHECK_STR(collection.total_visible_tracks_str, "Shown: 3 of 4 Tracks");

    SUITE("filters: a bound equal to the value keeps the track");
    // Bounds are inclusive; an 8 km track must survive a filter of exactly 8.
    reset_filters(&collection.filters);
    set_field(&collection.filters, FILTER_DISTANCE | LOW_LIMIT, "8.00");
    set_field(&collection.filters, FILTER_DISTANCE | HIGH_LIMIT, "8.00");
    save_filter_values(&collection.filters);
    apply_filter_values(&collection);
    CHECK(collection.tracks[0].visible_in_list);
}
