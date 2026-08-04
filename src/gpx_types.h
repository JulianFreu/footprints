#ifndef GPX_TYPES_H
#define GPX_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "config.h"
#include "gpx_activity.h"
#include "filter_types.h"
#include "map_types.h"
#include "track_splits.h"

typedef struct GpxPoint {
    double lat;
    double lon;
    int world_x;
    int world_y;
    int heat;
    int track_id;
    float elevation;        // metres above sea level
    float partial_distance; // distance covered up to this point
    // Seconds since the track's first timed point, and NAN where this point
    // carried no usable <time>. The graphs are the reason this is kept: pace
    // over the course of a run cannot be recovered from the two timestamps on
    // the track.
    float elapsed_secs;
    uint16_t heart_rate; // bpm, or 0 where the point carried none
} GpxPoint;

typedef enum {
    ID,
    TYPE,
    DATE,
    DISTANCE,
    DURATION,
    UPHILL,
    DOWNHILL,
    HIGHPOINT,
    PACE,
} AttributeType;

typedef struct GpxTrack {
    GpxPoint *points;
    int total_points;
    int points_capacity; // allocated slots in points[]
    int track_id;
    int mid_x;
    int mid_y;
    ActivityType act_type;

    bool visible_in_list;

    // Parsed once at load, from the first and last <trkpt> that carried a
    // time. Everything downstream -- the date filter, the sort, the displayed
    // date -- reads these rather than re-parsing a string.
    time_t start_utc;
    time_t end_utc;

    // Derived statistics. Their display form is produced where they are drawn;
    // see track_format.h.
    float duration_secs;
    float distance;
    float secs_per_km;
    float elev_up;
    float elev_down;
    float high_point;
    float low_point;

    // The fastest time, in seconds, over each of the record distances found
    // anywhere in the track; zero where the track never covered it. Filled
    // once at load by track_splits_compute, from the timestamps the parse
    // collects. What the points keep of those is the elapsed seconds above.
    float splits[SPLIT_COUNT];
} GpxTrack;

typedef struct GpxCollection {
    GpxTrack *tracks;
    int total_tracks;
    char total_visible_tracks_str[32];
    int max_heat;
    AttributeType current_sorting;
    AttributeType to_be_sorted_by;
    int *list_order;
    FilterSettings filters;
    TileTextureCache track_tile_cache;
    // Spatial index behind the rendered tiles. It spans every point, so the
    // filters change the tiles without touching it; only a reparse rebuilds it.
    TrackPointIndex point_index;
} GpxCollection;

#endif
