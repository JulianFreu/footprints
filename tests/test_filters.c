#include "../src/filters.c"

#include "harness.h"

#include <float.h>

// Fills a filter field's text as if it had been typed. save_filter_values is
// the only thing that turns that text into a numeric bound.
static void set_field(FilterSettings *filters, FilterAttribute attribute,
                      FilterBoundEnd end, const char *text) {
    snprintf(filter_bound_text(filters, attribute, end), FILTER_TEXT_SIZE, "%s", text);
}

void run_filters_tests(void) {
    SUITE("filters: duration text to seconds");
    CHECK_NEAR(duration_str_to_seconds("00:00:01"), 1.0, 1e-6);
    CHECK_NEAR(duration_str_to_seconds("00:01:00"), 60.0, 1e-6);
    CHECK_NEAR(duration_str_to_seconds("01:00:00"), 3600.0, 1e-6);
    CHECK_NEAR(duration_str_to_seconds("01:30:45"), 5445.0, 1e-6);
    CHECK_NEAR(duration_str_to_seconds("10:00:00"), 36000.0, 1e-6);

    SUITE("filters: pace text to seconds per km");
    CHECK_NEAR(pace_str_to_seconds("5:00"), 300.0, 1e-6);
    CHECK_NEAR(pace_str_to_seconds("4:30"), 270.0, 1e-6);
    CHECK_NEAR(pace_str_to_seconds("10:15"), 615.0, 1e-6);

    SUITE("filters: field ids round-trip and every attribute is described");
    for (int a = 0; a < FILTER_COUNT; a++) {
        for (int e = 0; e < BOUND_COUNT; e++) {
            uint16_t id = filter_field_id((FilterAttribute)a, (FilterBoundEnd)e);
            FilterAttribute got_attribute;
            FilterBoundEnd got_end;
            CHECK(filter_field_unpack(id, &got_attribute, &got_end));
            CHECK_INT(got_attribute, a);
            CHECK_INT(got_end, e);
        }
        // Every row is filled in: a missing one would leave a designated
        // initialiser hole, i.e. a filter with no name and a zero offset.
        CHECK(filter_display_name((FilterAttribute)a)[0] != '\0');
        CHECK(filter_fields[a].low_member != 0 || a == 0);
    }
    FilterAttribute overflow_attribute;
    FilterBoundEnd overflow_end;
    CHECK(!filter_field_unpack(0xFFFF, &overflow_attribute, &overflow_end));

    SUITE("filters: reset clears text and opens both bounds");
    FilterSettings filters;
    reset_filters(&filters);
    for (int type = 0; type < ACTIVITY_TYPE_COUNT; type++)
        CHECK(filters.show_activity[type]);
    for (int a = 0; a < FILTER_COUNT; a++) {
        for (int e = 0; e < BOUND_COUNT; e++) {
            CHECK_STR(filter_bound_text(&filters, (FilterAttribute)a, (FilterBoundEnd)e), "");
            double expected = (e == BOUND_HIGH) ? DBL_MAX : -DBL_MAX;
            CHECK(filters.bound[a][e].value == expected);
        }
    }

    SUITE("filters: typed bounds are parsed into the numeric fields");
    reset_filters(&filters);
    set_field(&filters, FILTER_DISTANCE, BOUND_LOW, "5.00");
    set_field(&filters, FILTER_DISTANCE, BOUND_HIGH, "10.00");
    set_field(&filters, FILTER_DURATION, BOUND_LOW, "00:30:00");
    set_field(&filters, FILTER_PACE, BOUND_HIGH, "6:00");
    save_filter_values(&filters);
    CHECK_NEAR(filters.bound[FILTER_DISTANCE][BOUND_LOW].value, 5.0, 1e-4);
    CHECK_NEAR(filters.bound[FILTER_DISTANCE][BOUND_HIGH].value, 10.0, 1e-4);
    CHECK_NEAR(filters.bound[FILTER_DURATION][BOUND_LOW].value, 1800.0, 1e-4);
    CHECK_NEAR(filters.bound[FILTER_PACE][BOUND_HIGH].value, 360.0, 1e-4);
    // Fields left blank keep their open bound.
    CHECK(filters.bound[FILTER_UPHILL][BOUND_LOW].value == -DBL_MAX);

    SUITE("filters: a date bound becomes a UTC timestamp");
    reset_filters(&filters);
    set_field(&filters, FILTER_DATE, BOUND_LOW, "24.08.2025");
    save_filter_values(&filters);
    CHECK_NEAR(filters.bound[FILTER_DATE][BOUND_LOW].value,
               (double)european_date_to_utc("24.08.2025"), 0.5);
    // A half-typed date leaves the bound open rather than jumping to some
    // arbitrary instant while the digits are still arriving.
    set_field(&filters, FILTER_DATE, BOUND_LOW, "24.0");
    save_filter_values(&filters);
    CHECK(filters.bound[FILTER_DATE][BOUND_LOW].value == -DBL_MAX);

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
    set_field(&collection.filters, FILTER_DISTANCE, BOUND_LOW, "5.00");
    save_filter_values(&collection.filters);
    apply_filter_values(&collection);
    CHECK(collection.tracks[0].visible_in_list);
    CHECK(!collection.tracks[1].visible_in_list);
    CHECK(collection.tracks[2].visible_in_list);

    // An activity toggle drops the ride.
    reset_filters(&collection.filters);
    collection.filters.show_activity[Cycling] = false;
    apply_filter_values(&collection);
    CHECK(collection.tracks[0].visible_in_list);
    CHECK(!collection.tracks[2].visible_in_list);

    // A date window drops the 2019 track and keeps the 2025 ones.
    reset_filters(&collection.filters);
    set_field(&collection.filters, FILTER_DATE, BOUND_LOW, "01.01.2025");
    set_field(&collection.filters, FILTER_DATE, BOUND_HIGH, "31.12.2025");
    save_filter_values(&collection.filters);
    apply_filter_values(&collection);
    CHECK(collection.tracks[0].visible_in_list);
    CHECK(!collection.tracks[3].visible_in_list);
    CHECK_STR(collection.total_visible_tracks_str, "Shown: 3 of 4 Tracks");

    SUITE("filters: a bound equal to the value keeps the track");
    // Bounds are inclusive; an 8 km track must survive a filter of exactly 8.
    reset_filters(&collection.filters);
    set_field(&collection.filters, FILTER_DISTANCE, BOUND_LOW, "8.00");
    set_field(&collection.filters, FILTER_DISTANCE, BOUND_HIGH, "8.00");
    save_filter_values(&collection.filters);
    apply_filter_values(&collection);
    CHECK(collection.tracks[0].visible_in_list);
}
