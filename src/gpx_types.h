#ifndef GPX_TYPES_H
#define GPX_TYPES_H

#include <stdbool.h>
#include <time.h>

#include "config.h"
#include "filter_types.h"
#include "map_types.h"

typedef struct GpxPoint {
    double lat;
    double lon;
    int world_x;
    int world_y;
    int heat;
    int track_id;
    float elevation;        // metres above sea level
    float partial_distance; // distance covered up to this point
} GpxPoint;

typedef enum {
    Run,
    Hike,
    Cycling,
    Other,
} ActivityType;

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

    char start_time_raw[64]; // Original ISO8601 string from first <trkpt>
    char end_time_raw[64];   // Original ISO8601 string from last <trkpt>

    // start_time_raw/end_time_raw parsed once at load. The date filter compares
    // against these rather than re-running sscanf over both strings of every
    // track on every call.
    time_t start_utc;
    time_t end_utc;

    char start_time_str[16]; // Display version: "15:02:15"
    char start_date_str[16]; // Display version: "2025-08-24"

    float duration_secs;
    char duration_str[16];

    float distance;
    char distance_str[16];

    float secs_per_km;
    char pace_str[16];

    float elev_up;
    char elev_up_str[16];

    float elev_down;
    char elev_down_str[16];

    float high_point;
    char high_point_str[16];

    float low_point;
    char low_point_str[16];
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
    // Spatial index behind the rendered tiles. Both are dropped together, by
    // tracks_invalidate_cache, whenever the visible set changes.
    TrackPointIndex point_index;
} GpxCollection;

#endif
