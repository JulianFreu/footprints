#include "track_sort.h"

#include <stdlib.h>

// Sorting the run list is a question about tracks, not about the UI that
// happens to trigger it, so it lives here rather than in the layout code.

typedef struct TrackSortEntry {
    double key;
    int track_id;
} TrackSortEntry;

// The value a track is ordered by. Every criteria reduces to one number, which
// is what lets a single comparator serve all of them -- including the two that
// used to be special-cased inside the comparator itself.
//
// Everything reads largest-first except pace, where a lower number is the
// better run; negating its key puts it in the same descending order as the
// rest, so one comparator serves every criteria.
static double track_sort_key(const GpxTrack *track, AttributeType criteria) {
    switch (criteria) {
    case DISTANCE:
        return track->distance;
    case DURATION:
        return track->duration_secs;
    case PACE:
        return -(double)track->secs_per_km;
    case UPHILL:
        return track->elev_up;
    case DOWNHILL:
        return track->elev_down;
    case HIGHPOINT:
        return track->high_point;
    case TYPE:
        return track->act_type;
    case DATE:
        // Latest first. The parsed timestamp is compared rather than the raw
        // ISO string, which is the same ordering without the strcmp.
        return (double)track->start_utc;
    case ID:
        break;
    }
    return track->track_id;
}

// Descending by key, with the track id breaking ties so the order is the same
// on every run -- qsort is not stable, so equal keys were free to swap places
// between sorts of the same data.
static int compare_entries(const void *a, const void *b) {
    const TrackSortEntry *ea = (const TrackSortEntry *)a;
    const TrackSortEntry *eb = (const TrackSortEntry *)b;
    if (ea->key != eb->key)
        return (ea->key < eb->key) - (ea->key > eb->key);
    return (ea->track_id > eb->track_id) - (ea->track_id < eb->track_id);
}

// Sorting a (key, id) array rather than the id array alone is what removes the
// need for the comparator to reach the collection through a global: qsort gives
// a comparator no user-data channel, so the key has to travel with the element.
static void sort_list_order(GpxCollection *collection, AttributeType criteria) {
    int n = collection->total_tracks;
    if (n <= 0)
        return;

    TrackSortEntry *entries = malloc((size_t)n * sizeof(TrackSortEntry));
    if (!entries)
        return;

    for (int i = 0; i < n; i++) {
        entries[i].track_id = collection->tracks[i].track_id;
        entries[i].key = track_sort_key(&collection->tracks[i], criteria);
    }

    qsort(entries, (size_t)n, sizeof(TrackSortEntry), compare_entries);

    for (int i = 0; i < n; i++)
        collection->list_order[i] = entries[i].track_id;

    free(entries);
}

static void reverse_list_order(GpxCollection *collection) {
    int *order = collection->list_order;
    int n = collection->total_tracks;

    for (int i = 0; i < n / 2; ++i) {
        int temp = order[i];
        order[i] = order[n - 1 - i];
        order[n - 1 - i] = temp;
    }
}

void track_sort_by(GpxCollection *collection, AttributeType criteria) {
    if (collection->current_sorting == criteria) {
        reverse_list_order(collection);
        return;
    }
    sort_list_order(collection, criteria);
    collection->current_sorting = criteria;
}
