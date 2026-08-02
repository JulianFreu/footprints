#include "../src/records.c"

// The label is its own translation unit precisely so a pure module can link it;
// records.c calls it, and nothing else in the test binary pulls it in.
#include "../src/gpx_activity.c"

#include "harness.h"

#include <stdlib.h>

// What holds each record, and what happens when nothing does. The interesting
// cases are all about exclusion: a zero split is not a fast time, a hidden
// track is not a record, and a bike ride is not a run.

static time_t at(const char *iso) {
    struct tm tm = {0};
    int year, month, day, hour, minute, second;
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day,
               &hour, &minute, &second) != 6)
        return (time_t)-1;
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return timegm(&tm);
}

// One track, dated and visible, with everything a category can rank it by.
static GpxTrack track_at(const char *iso, ActivityType type, float distance,
                         float high_point, float elev_up, float split_5k) {
    GpxTrack track = {0};
    track.start_utc = at(iso);
    track.end_utc = track.start_utc + 3600;
    track.act_type = type;
    track.distance = distance;
    track.duration_secs = 3600.0f;
    track.secs_per_km = (distance > 0.0f) ? 3600.0f / distance : 0.0f;
    track.high_point = high_point;
    track.elev_up = elev_up;
    track.splits[SPLIT_5K] = split_5k;
    track.visible_in_list = true;
    return track;
}

static GpxCollection collection_of(GpxTrack *tracks, int count) {
    GpxCollection collection = {0};
    collection.tracks = tracks;
    collection.total_tracks = count;
    for (int i = 0; i < count; i++)
        tracks[i].track_id = i;
    return collection;
}

void run_records_tests(void) {
    RecordTable table;
    char text[RECORD_DETAIL_MAX];

    SUITE("records: every category is described and reads a real source");
    // A missing row would be a designated-initialiser hole: a category with no
    // label whose member offset is zero, which reads whatever comes first in
    // GpxTrack -- a pointer, reinterpreted as a float.
    for (int c = 0; c < RECORD_CATEGORY_COUNT; c++) {
        CHECK(records_category_label((RecordCategory)c)[0] != '\0');
        const RecordDef *def = &record_defs[c];
        if (def->source == RECORD_SOURCE_MEMBER)
            CHECK(def->from.member != 0);
        else
            CHECK(track_splits_metres(def->from.split) > 0.0f);
    }
    // Out of range asks for nothing rather than reading off the table.
    CHECK_STR(records_category_label(RECORD_CATEGORY_COUNT), "");
    CHECK_STR(records_category_unit((RecordCategory)-1), "");

    SUITE("records: the largest wins a distance, the smallest wins a time");
    {
        GpxTrack tracks[] = {
            track_at("2025-01-01T08:00:00Z", Run, 10.0f, 300.0f, 100.0f, 1500.0f),
            track_at("2025-02-01T08:00:00Z", Run, 30.0f, 900.0f, 700.0f, 1300.0f),
            track_at("2025-03-01T08:00:00Z", Run, 20.0f, 600.0f, 400.0f, 1400.0f),
        };
        GpxCollection collection = collection_of(tracks, 3);
        records_build(&table, &collection);

        CHECK_INT(table.list[RECORD_LONGEST_RUN].count, 3);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[0].track_id, 1); // 30 km
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[1].track_id, 2); // 20 km
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[2].track_id, 0); // 10 km

        // A time ranks the other way round.
        CHECK_INT(table.list[RECORD_FASTEST_5K].entry[0].track_id, 1); // 1300 s
        CHECK_INT(table.list[RECORD_FASTEST_5K].entry[2].track_id, 0); // 1500 s

        CHECK_INT(table.list[RECORD_HIGHEST_PEAK].entry[0].track_id, 1);
        CHECK_INT(table.list[RECORD_MOST_ASCENT].entry[0].track_id, 1);
    }

    SUITE("records: the list keeps the best three, not the first three");
    {
        // Eight tracks in ascending order, so a list that simply took the first
        // three it saw would hold the three shortest.
        GpxTrack tracks[8];
        for (int i = 0; i < 8; i++)
            tracks[i] = track_at("2025-01-01T08:00:00Z", Run, (float)(i + 1),
                                 0.0f, 0.0f, 0.0f);
        GpxCollection collection = collection_of(tracks, 8);
        records_build(&table, &collection);

        CHECK_INT(table.list[RECORD_LONGEST_RUN].count, RECORDS_TOP_N);
        CHECK_NEAR(table.list[RECORD_LONGEST_RUN].entry[0].value, 8.0, 1e-6);
        CHECK_NEAR(table.list[RECORD_LONGEST_RUN].entry[1].value, 7.0, 1e-6);
        CHECK_NEAR(table.list[RECORD_LONGEST_RUN].entry[2].value, 6.0, 1e-6);

        // And in descending order, where a list that never evicted would hold
        // the three longest anyway -- so this pins the insertion, not the input.
        for (int i = 0; i < 8; i++)
            tracks[i].distance = (float)(8 - i);
        records_build(&table, &collection);
        CHECK_NEAR(table.list[RECORD_LONGEST_RUN].entry[0].value, 8.0, 1e-6);
        CHECK_NEAR(table.list[RECORD_LONGEST_RUN].entry[2].value, 6.0, 1e-6);
    }

    SUITE("records: ties are both kept, oldest first");
    {
        GpxTrack tracks[] = {
            track_at("2025-01-01T08:00:00Z", Run, 12.0f, 0.0f, 0.0f, 0.0f),
            track_at("2025-02-01T08:00:00Z", Run, 12.0f, 0.0f, 0.0f, 0.0f),
        };
        GpxCollection collection = collection_of(tracks, 2);
        records_build(&table, &collection);

        CHECK_INT(table.list[RECORD_LONGEST_RUN].count, 2);
        // A tie does not beat what is already held, so the earlier track keeps
        // the higher place -- which is what makes the order stable rather than
        // dependent on the order the directory was read in.
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[0].track_id, 0);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[1].track_id, 1);
    }

    SUITE("records: a filtered-out track is not a record");
    {
        GpxTrack tracks[] = {
            track_at("2025-01-01T08:00:00Z", Run, 42.0f, 0.0f, 0.0f, 0.0f),
            track_at("2025-02-01T08:00:00Z", Run, 12.0f, 0.0f, 0.0f, 0.0f),
        };
        GpxCollection collection = collection_of(tracks, 2);

        records_build(&table, &collection);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[0].track_id, 0);

        // The filter panel hides the holder; the runner-up takes it.
        tracks[0].visible_in_list = false;
        records_build(&table, &collection);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].count, 1);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[0].track_id, 1);
    }

    SUITE("records: an undated track is not a record");
    {
        GpxTrack tracks[] = {
            track_at("2025-01-01T08:00:00Z", Run, 42.0f, 0.0f, 0.0f, 0.0f),
            track_at("2025-02-01T08:00:00Z", Run, 12.0f, 0.0f, 0.0f, 0.0f),
        };
        // What a planned route looks like: a shape with no recording behind it.
        tracks[0].start_utc = (time_t)-1;
        GpxCollection collection = collection_of(tracks, 2);
        records_build(&table, &collection);

        CHECK_INT(table.list[RECORD_LONGEST_RUN].count, 1);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[0].track_id, 1);
    }

    SUITE("records: only runs hold the running records");
    {
        GpxTrack tracks[] = {
            // A ride, further and faster than anything on foot.
            track_at("2025-01-01T08:00:00Z", Cycling, 80.0f, 1200.0f, 900.0f, 600.0f),
            track_at("2025-02-01T08:00:00Z", Run, 12.0f, 300.0f, 100.0f, 1500.0f),
        };
        GpxCollection collection = collection_of(tracks, 2);
        records_build(&table, &collection);

        // The ride cannot hold a run record...
        CHECK_INT(table.list[RECORD_FASTEST_5K].count, 1);
        CHECK_INT(table.list[RECORD_FASTEST_5K].entry[0].track_id, 1);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].count, 1);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].entry[0].track_id, 1);
        // ...but it is still the longest activity, and still the biggest climb.
        CHECK_INT(table.list[RECORD_LONGEST_ACTIVITY].entry[0].track_id, 0);
        CHECK_INT(table.list[RECORD_HIGHEST_PEAK].entry[0].track_id, 0);
        CHECK_INT(table.list[RECORD_MOST_ASCENT].entry[0].track_id, 0);
    }

    SUITE("records: a distance never covered is not a fast time");
    {
        // Every one of these is too short for the split, so its time is zero.
        // Ranked as a minimum without the guard, zero beats every real record.
        GpxTrack tracks[] = {
            track_at("2025-01-01T08:00:00Z", Run, 3.0f, 0.0f, 0.0f, 0.0f),
            track_at("2025-02-01T08:00:00Z", Run, 3.0f, 0.0f, 0.0f, 0.0f),
            track_at("2025-03-01T08:00:00Z", Run, 12.0f, 0.0f, 0.0f, 1500.0f),
        };
        GpxCollection collection = collection_of(tracks, 3);
        records_build(&table, &collection);

        CHECK_INT(table.list[RECORD_FASTEST_5K].count, 1);
        CHECK_INT(table.list[RECORD_FASTEST_5K].entry[0].track_id, 2);
        CHECK_NEAR(table.list[RECORD_FASTEST_5K].entry[0].value, 1500.0, 1e-6);
        // Nothing here ever ran a marathon, so that category stays empty.
        CHECK_INT(table.list[RECORD_FASTEST_MARATHON].count, 0);
    }

    SUITE("records: nothing qualifying leaves the table empty, not stale");
    {
        GpxTrack tracks[] = {
            track_at("2025-01-01T08:00:00Z", Run, 42.0f, 900.0f, 700.0f, 1200.0f),
        };
        GpxCollection collection = collection_of(tracks, 1);
        records_build(&table, &collection);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].count, 1);

        tracks[0].visible_in_list = false;
        records_build(&table, &collection);
        for (int c = 0; c < RECORD_CATEGORY_COUNT; c++)
            CHECK_INT(table.list[c].count, 0);

        // And the degenerate callers, which must not walk anything.
        records_build(&table, NULL);
        CHECK_INT(table.list[RECORD_LONGEST_RUN].count, 0);
        records_build(NULL, &collection);
    }

    SUITE("records: values are formatted the way their row reads");
    records_format_value(RECORD_FASTEST_5K, 1223.0f, text, sizeof(text));
    CHECK_STR(text, "20:23"); // no hours field where there are no hours
    records_format_value(RECORD_FASTEST_MARATHON, 13262.0f, text, sizeof(text));
    CHECK_STR(text, "3:41:02");
    records_format_value(RECORD_FASTEST_5K, 59.6f, text, sizeof(text));
    CHECK_STR(text, "1:00"); // rounded, not truncated to 0:59
    records_format_value(RECORD_LONGEST_RUN, 41.88f, text, sizeof(text));
    CHECK_STR(text, "41.88");
    records_format_value(RECORD_LONGEST_ACTIVITY, 184.4f, text, sizeof(text));
    CHECK_STR(text, "184.40"); // three digits and two decimals still fit
    records_format_value(RECORD_HIGHEST_PEAK, 1854.4f, text, sizeof(text));
    CHECK_STR(text, "1854");
    // A negative time is not something to print as one.
    records_format_value(RECORD_FASTEST_5K, -5.0f, text, sizeof(text));
    CHECK_STR(text, "0:00");
    records_format_value(RECORD_CATEGORY_COUNT, 1.0f, text, sizeof(text));
    CHECK_STR(text, "");

    SUITE("records: the detail line says what the record was set inside");
    {
        RecordEntry entry = {
            .track_id = 0,
            .start_utc = at("2025-04-08T08:00:00Z"),
            .value = 2468.0f,
            .distance = 21.07f,
            .duration_secs = 5273.0f,
            .secs_per_km = 250.0f,
            .act_type = Run};

        records_format_detail(RECORD_LONGEST_RUN, &entry, text, sizeof(text));
        CHECK_STR(text, "1:27:53 at 4:10 /km");

        records_format_detail(RECORD_HIGHEST_PEAK, &entry, text, sizeof(text));
        CHECK_STR(text, "Run, 21.1 km");

        // The split's own pace, not the track's: 10 km in 2468 s is 4:07/km,
        // inside a run that averaged 4:10/km. That difference is the feature.
        records_format_detail(RECORD_FASTEST_10K, &entry, text, sizeof(text));
        CHECK_STR(text, "4:07 /km in a 21.1 km run");

        // A track with no distance must not divide by zero on the way out.
        RecordEntry empty = {0};
        empty.act_type = Other; // zero is Run, which would not test the label
        records_format_detail(RECORD_LONGEST_ACTIVITY, &empty, text, sizeof(text));
        CHECK_STR(text, "Other, 0.0 km");
        records_format_detail(RECORD_FASTEST_5K, &empty, text, sizeof(text));
        CHECK_STR(text, "- /km in a 0.0 km run");
        records_format_detail(RECORD_LONGEST_RUN, &empty, text, sizeof(text));
        CHECK_STR(text, "0:00 at - /km");

        records_format_detail(RECORD_LONGEST_RUN, NULL, text, sizeof(text));
        CHECK_STR(text, "");
    }
}
