#include "records.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Which track holds each record, and how each one is read off a track. The
// table below is the whole of it: everything else here either walks the
// collection once against that table or turns one of its numbers into a string.

typedef enum RecordSource {
    RECORD_SOURCE_MEMBER, // a float member of GpxTrack
    RECORD_SOURCE_SPLIT,  // one of track->splits
} RecordSource;

typedef enum RecordOrder {
    RECORD_ORDER_MAX, // a distance or a climb: the largest wins
    RECORD_ORDER_MIN, // a time: the smallest wins
} RecordOrder;

typedef enum RecordValueFormat {
    RECORD_VALUE_KM,
    RECORD_VALUE_METRES,
    RECORD_VALUE_CLOCK,
} RecordValueFormat;

typedef enum RecordDetail {
    RECORD_DETAIL_TIME_PACE,     // "3:41:02 at 5:14 /km"
    RECORD_DETAIL_TYPE_DISTANCE, // "Hike, 14.2 km"
    RECORD_DETAIL_SPLIT_PACE,    // "4:12 /km in a 21.1 km run"
} RecordDetail;

typedef struct RecordDef {
    const char *label;
    const char *unit;
    RecordSource source;
    union {
        size_t member;       // offsetof(GpxTrack, field), for _MEMBER
        SplitDistance split; // which of track->splits, for _SPLIT
    } from;
    RecordOrder order;
    // Whether the category is about running. A ride holds a five-kilometre
    // "record" no runner will ever beat, so the four splits and the longest run
    // ask for a Run and the rest take whatever the library has.
    bool runs_only;
    RecordValueFormat format;
    RecordDetail detail;
} RecordDef;

#define MEMBER(field) offsetof(GpxTrack, field)

static const RecordDef record_defs[RECORD_CATEGORY_COUNT] = {
    [RECORD_LONGEST_RUN] = {"Longest run", "km", RECORD_SOURCE_MEMBER, {.member = MEMBER(distance)}, RECORD_ORDER_MAX, true, RECORD_VALUE_KM, RECORD_DETAIL_TIME_PACE},
    [RECORD_LONGEST_ACTIVITY] = {"Longest activity", "km", RECORD_SOURCE_MEMBER, {.member = MEMBER(distance)}, RECORD_ORDER_MAX, false, RECORD_VALUE_KM, RECORD_DETAIL_TYPE_DISTANCE},
    [RECORD_HIGHEST_PEAK] = {"Highest peak", "m", RECORD_SOURCE_MEMBER, {.member = MEMBER(high_point)}, RECORD_ORDER_MAX, false, RECORD_VALUE_METRES, RECORD_DETAIL_TYPE_DISTANCE},
    [RECORD_MOST_ASCENT] = {"Most elevation gain", "m", RECORD_SOURCE_MEMBER, {.member = MEMBER(elev_up)}, RECORD_ORDER_MAX, false, RECORD_VALUE_METRES, RECORD_DETAIL_TYPE_DISTANCE},
    [RECORD_FASTEST_5K] = {"Fastest 5k", "", RECORD_SOURCE_SPLIT, {.split = SPLIT_5K}, RECORD_ORDER_MIN, true, RECORD_VALUE_CLOCK, RECORD_DETAIL_SPLIT_PACE},
    [RECORD_FASTEST_10K] = {"Fastest 10k", "", RECORD_SOURCE_SPLIT, {.split = SPLIT_10K}, RECORD_ORDER_MIN, true, RECORD_VALUE_CLOCK, RECORD_DETAIL_SPLIT_PACE},
    [RECORD_FASTEST_HALF] = {"Fastest half marathon", "", RECORD_SOURCE_SPLIT, {.split = SPLIT_HALF_MARATHON}, RECORD_ORDER_MIN, true, RECORD_VALUE_CLOCK, RECORD_DETAIL_SPLIT_PACE},
    [RECORD_FASTEST_MARATHON] = {"Fastest marathon", "", RECORD_SOURCE_SPLIT, {.split = SPLIT_MARATHON}, RECORD_ORDER_MIN, true, RECORD_VALUE_CLOCK, RECORD_DETAIL_SPLIT_PACE},
};

#undef MEMBER

static bool category_is_valid(RecordCategory category) {
    return category >= 0 && category < RECORD_CATEGORY_COUNT;
}

const char *records_category_label(RecordCategory category) {
    return category_is_valid(category) ? record_defs[category].label : "";
}

const char *records_category_unit(RecordCategory category) {
    return category_is_valid(category) ? record_defs[category].unit : "";
}

// --- Formatting ---

// h:mm:ss, and mm:ss when there are no hours: a five-kilometre record reads
// "20:23" rather than "0:20:23", and a marathon still reads "3:41:02".
static void format_clock(float seconds, char *out, size_t size) {
    if (seconds < 0.0f)
        seconds = 0.0f;
    int total = (int)(seconds + 0.5f);
    int hours = total / 3600;
    int minutes = (total / 60) % 60;

    if (hours > 0)
        snprintf(out, size, "%d:%02d:%02d", hours, minutes, total % 60);
    else
        snprintf(out, size, "%d:%02d", minutes, total % 60);
}

// m:ss per kilometre, the form the run list's pace column already uses.
static void format_pace(float secs_per_km, char *out, size_t size) {
    if (!(secs_per_km > 0.0f)) {
        snprintf(out, size, "-");
        return;
    }
    int total = (int)(secs_per_km + 0.5f);
    snprintf(out, size, "%d:%02d", total / 60, total % 60);
}

void records_format_value(RecordCategory category, float value, char *out, size_t size) {
    if (size == 0)
        return;
    if (!category_is_valid(category)) {
        out[0] = '\0';
        return;
    }

    switch (record_defs[category].format) {
    case RECORD_VALUE_KM:
        snprintf(out, size, "%.2f", (double)value);
        break;
    case RECORD_VALUE_METRES:
        snprintf(out, size, "%.0f", (double)value);
        break;
    case RECORD_VALUE_CLOCK:
        format_clock(value, out, size);
        break;
    default:
        out[0] = '\0';
        break;
    }
}

void records_format_detail(RecordCategory category, const RecordEntry *entry,
                           char *out, size_t size) {
    if (size == 0)
        return;
    if (!category_is_valid(category) || !entry) {
        out[0] = '\0';
        return;
    }

    char clock[RECORD_VALUE_MAX];
    char pace[RECORD_VALUE_MAX];

    switch (record_defs[category].detail) {
    case RECORD_DETAIL_TIME_PACE:
        format_clock(entry->duration_secs, clock, sizeof(clock));
        format_pace(entry->secs_per_km, pace, sizeof(pace));
        snprintf(out, size, "%s at %s /km", clock, pace);
        break;

    case RECORD_DETAIL_TYPE_DISTANCE:
        snprintf(out, size, "%s, %.1f km", activity_type_label(entry->act_type),
                 (double)entry->distance);
        break;

    case RECORD_DETAIL_SPLIT_PACE: {
        // The pace the split itself was held at, which is not the track's:
        // the whole point of the category is that the record sits inside a
        // longer and slower run.
        float metres = track_splits_metres(record_defs[category].from.split);
        float secs_per_km = (metres > 0.0f) ? entry->value / (metres / 1000.0f) : 0.0f;
        format_pace(secs_per_km, pace, sizeof(pace));
        snprintf(out, size, "%s /km in a %.1f km run", pace, (double)entry->distance);
        break;
    }

    default:
        out[0] = '\0';
        break;
    }
}

// --- Building the table ---

// Reads the GpxTrack member a row points at. Every one of them is a float, but
// taken through memcpy rather than a cast so the offset does not have to be an
// aligned one. This is the same four lines as stats.c's reader; a header that
// existed to hold one static would be the worse of the two.
static float track_metric_value(const GpxTrack *track, size_t member_offset) {
    float value;
    memcpy(&value, (const char *)track + member_offset, sizeof(value));
    return value;
}

// Whether a track is one of the ones being ranked. The same two conditions
// stats.c counts by: a track the filter panel has hidden is not a record, so
// the panel and the run list always agree about what the library is, and a
// track the parser found no timestamp on has no date to show beside its number.
static bool track_counts(const GpxTrack *track) {
    return track->visible_in_list && track->start_utc != (time_t)-1;
}

static bool value_beats(float candidate, float held, RecordOrder order) {
    return (order == RECORD_ORDER_MAX) ? candidate > held : candidate < held;
}

// Insertion into a list of at most three, kept sorted. Sorting the collection
// per category would order a thousand tracks eight times over to show
// twenty-four rows.
static void record_insert(RecordList *list, const RecordEntry *entry, RecordOrder order) {
    int position = list->count;
    while (position > 0 && value_beats(entry->value, list->entry[position - 1].value, order))
        position--;

    if (position >= RECORDS_TOP_N)
        return; // beaten by everything already held

    int last = (list->count < RECORDS_TOP_N) ? list->count : RECORDS_TOP_N - 1;
    for (int i = last; i > position; i--)
        list->entry[i] = list->entry[i - 1];

    list->entry[position] = *entry;
    if (list->count < RECORDS_TOP_N)
        list->count++;
}

void records_build(RecordTable *table, const GpxCollection *collection) {
    if (!table)
        return;

    // Cleared rather than added to: a filter that hides the holder of a record
    // has to drop it, and a category nothing qualifies for has to read as empty
    // rather than as whatever was there last time.
    memset(table, 0, sizeof(*table));
    if (!collection || !collection->tracks)
        return;

    for (int i = 0; i < collection->total_tracks; i++) {
        const GpxTrack *track = &collection->tracks[i];
        if (!track_counts(track))
            continue;

        for (int c = 0; c < RECORD_CATEGORY_COUNT; c++) {
            const RecordDef *def = &record_defs[c];
            if (def->runs_only && track->act_type != Run)
                continue;

            float value = (def->source == RECORD_SOURCE_SPLIT)
                              ? track->splits[def->from.split]
                              : track_metric_value(track, def->from.member);

            // Zero is what an unset split and an unmeasured climb both read as,
            // and neither is a record. Without this a minimum over zeroes would
            // put every track that never reached five kilometres at the top of
            // the five-kilometre list.
            if (!(value > 0.0f))
                continue;

            RecordEntry entry = {
                .track_id = track->track_id,
                .start_utc = track->start_utc,
                .value = value,
                .distance = track->distance,
                .duration_secs = track->duration_secs,
                .secs_per_km = track->secs_per_km,
                .act_type = track->act_type};
            record_insert(&table->list[c], &entry, def->order);
        }
    }
}
