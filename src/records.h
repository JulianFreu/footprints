#ifndef RECORDS_H
#define RECORDS_H

#include <stddef.h>
#include <time.h>

#include "gpx_types.h"

// The personal bests: for each category, the handful of activities that hold
// it. One table over the whole collection, the same way stats.c is one series
// over it -- and like stats.c, nothing here knows about SDL or Clay. The panel
// that draws it is src/ui_records.c.

// Rows kept per category. Enough to see a record and what it beat, few enough
// that the whole table is a fixed-size struct and the build allocates nothing.
#define RECORDS_TOP_N 3

typedef enum RecordCategory {
    RECORD_LONGEST_RUN = 0,
    RECORD_LONGEST_ACTIVITY,
    RECORD_HIGHEST_PEAK,
    RECORD_MOST_ASCENT,
    RECORD_FASTEST_5K,
    RECORD_FASTEST_10K,
    RECORD_FASTEST_HALF,
    RECORD_FASTEST_MARATHON,
    RECORD_CATEGORY_COUNT
} RecordCategory;

// One entry. A snapshot of the track rather than a pointer into the collection:
// the tracks array is reallocated wholesale by every rescan, and a table built
// before one would then be holding rows into freed memory. track_id is kept so
// a click can select the track, and is checked against the collection again
// before it is used.
typedef struct RecordEntry {
    int track_id;
    time_t start_utc;
    float value;         // in the category's own unit: km, metres or seconds
    float distance;      // the whole track's, for the line of context
    float duration_secs; // the whole track's
    float secs_per_km;   // the whole track's
    ActivityType act_type;
} RecordEntry;

typedef struct RecordList {
    RecordEntry entry[RECORDS_TOP_N]; // best first
    int count;
} RecordList;

typedef struct RecordTable {
    RecordList list[RECORD_CATEGORY_COUNT];
} RecordTable;

// Room for the strings the two formatters write.
#define RECORD_VALUE_MAX 16
#define RECORD_DETAIL_MAX 48

// "Longest run", "Fastest 10k" and so on, and the unit the value is in -- empty
// for the categories whose value is a time, which carries its own colons.
const char *records_category_label(RecordCategory category);
const char *records_category_unit(RecordCategory category);

// The value the way its row shows it: two decimals of a kilometre, whole
// metres, or a clock that grows an hours field only when there is one.
void records_format_value(RecordCategory category, float value, char *out, size_t size);

// The second line of a row: what the record was set inside. A distance record
// shows the time and pace it was run at, a climb shows the activity and how far
// it went, and a split shows the pace it was held at and the run it came from.
void records_format_detail(RecordCategory category, const RecordEntry *entry,
                           char *out, size_t size);

// Fills the table from every track the filters leave visible, in one pass.
// Categories nothing qualifies for come back with a count of zero rather than
// with whatever was in them last time.
void records_build(RecordTable *table, const GpxCollection *collection);

#endif
