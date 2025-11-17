#ifndef STRUCTS_H
#define STRUCTS_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#define M_PI 3.14159265358979323846
#define TILE_SIZE 256
#define FIFO_DEPTH 16
#define WINDOW_TITLE "footprints"
#define SCREEN_WIDTH 1200
#define SCREEN_HEIGHT 800
#define TILE_ZOOM 12
#define TILE_X 2147
#define TILE_Y 1358
#define MIN_ZOOM 4
#define MAX_ZOOM 20
#define TARGET_FPS 60
#define FRAME_DELAY_MS (1000 / TARGET_FPS)
#define INPUT_BUFFER_SIZE 16
#define HEAT_BUCKETS 16
#define NUM_THREADS 24
#define DEG_TO_RAD (M_PI / 180.0)
#define METERS_PER_DEG_LAT 111320.0

typedef enum
{
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

#define FILTER_DISTANCE 0b0000000000000001
#define FILTER_DATE 0b0000000000000010
#define FILTER_DURATION 0b0000000000000100
#define FILTER_UPHILL 0b0000000000001000
#define FILTER_DOWNHILL 0b0000000000010000
#define FILTER_PEAK 0b0000000000100000
#define FILTER_PACE 0b0000000001000000
#define HIGH_LIMIT 0b1000000000000000
#define LOW_LIMIT 0b0100000000000000

typedef struct Clay_String
{
    // Set this boolean to true if the char* data underlying this string will live for the entire lifetime of the program.
    // This will automatically be set for strings created with CLAY_STRING, as the macro requires a string literal.
    bool isStaticallyAllocated;
    int32_t length;
    // The underlying character memory. Note: this will not be copied and will not extend the lifetime of the underlying memory.
    const char *chars;
} Clay_String;

typedef struct AnimationState
{
    bool opening;
    bool closing;
    float animation;
    int ticks;
} AnimationState;

typedef struct UIState
{
    AnimationState left_sidebar;
    AnimationState right_sidebar;
    AnimationState run_list;
    AnimationState filters_animation;
    bool text_input_mode;
    char text_input_buffer[INPUT_BUFFER_SIZE];
    size_t text_input_length;
    uint16_t activeFilterID;
} UIState;

typedef struct MapTile
{
    int tile_x;
    int tile_y;
    int zoom;
} MapTile;

typedef struct
{
    SDL_Point pos;
    int heat;
} HeatPoint;

typedef struct
{
    MapTile key;
    HeatPoint *points;
    int point_count;
    int capacity;
} CombinedTilePoints;

typedef struct
{
    MapTile key;
    SDL_Texture *texture;
    bool valid;
} TrackTileTexture;

typedef struct
{
    TrackTileTexture *entries;
    int size;
    int capacity;
} TrackTileTextureCache;

typedef struct
{
    uint32_t fontId;
    TTF_Font *font;
} SDL2_Font;

struct MemoryStruct
{
    char *memory;
    size_t size;
};

typedef struct TileTexture
{
    MapTile key;
    SDL_Texture *texture;
} TileTexture;

typedef struct
{
    TileTexture *entries;
    int size;
    int capacity;
} TileTextureCache;

struct fifo
{
    int read_p;
    int write_p;
    MapTile tile[FIFO_DEPTH];
    MapTile tile_in_dl;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

struct application
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *map;
    SDL_Texture *tex_tracks;
    int window_width;
    int window_height;
    int zoom;
    int mouse_x;
    int mouse_y;
    int wheel_y;
    int world_x;
    int world_y;
    double center_coord_x;
    double center_coord_y;
    int running;
    int dragging;
    bool leftMouseButtonPressed;
    int selected_track;
    struct fifo download_queue;
    SDL2_Font fonts[1];
    TileTextureCache tile_cache;
    SDL_Texture *selected_track_overlay[MAX_ZOOM + 1]; // +1 for zoom level 0 to 20
    int currentFPS;
    SDL_Event event;
    Uint32 lastFrameTime;
    bool mouseOverUI;
    bool show_heat;
    bool update_window;
};

typedef struct GpxPoint
{
    double lat;
    double lon;
    int world_x;
    int world_y;
    int heat;
    int track_id;
    float elevation;        // Höhe in Metern
    float partial_distance; // Bis hier zurückgelegte Strecke
} GpxPoint;

typedef enum
{
    Run,
    Hike,
    Cycling,
    Other,
} ActivityType;

typedef struct GpxTrack
{
    GpxPoint *points;
    int total_points;
    int track_id;
    int mid_x;
    int mid_y;
    ActivityType act_type; // Original ISO8601 string from first <trkpt>

    bool visible_in_list;

    char start_time_raw[64]; // Original ISO8601 string from first <trkpt>
    char end_time_raw[64];   // Original ISO8601 string from last <trkpt>

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

typedef struct FilterSettings
{
    char start_date_str[16];        // Display version: "15:02:15"
    char end_date_str[16];          // Display version: "15:02:15"
    char start_date_str_filter[16]; // Display version: "15:02:15"
    char end_date_str_filter[16];   // Display version: "15:02:15"

    char start_time_str[16]; // Display version: "15:02:15"
    char end_time_str[16];   // Display version: "15:02:15"

    float duration_secs_high;
    char duration_high_str[16];
    float duration_secs_low;
    char duration_low_str[16];

    float distance_high;
    char distance_high_str[16];
    float distance_low;
    char distance_low_str[16];

    float secs_per_km_high;
    char pace_high_str[16];
    float secs_per_km_low;
    char pace_low_str[16];

    float elev_up_high;
    char elev_up_high_str[16];
    float elev_up_low;
    char elev_up_low_str[16];

    float elev_down_high;
    char elev_down_high_str[16];
    float elev_down_low;
    char elev_down_low_str[16];

    float high_point_high;
    char high_point_high_str[16];
    float high_point_low;
    char high_point_low_str[16];

    float low_point_high;
    char low_point_high_str[16];
    float low_point_low;
    char low_point_low_str[16];

    bool showRuns;
    bool showCycling;
    bool showHikes;
    bool showOther;

} FilterSettings;

typedef struct GpxCollection
{
    GpxTrack *tracks;
    int total_tracks;
    char total_visible_tracks_str[32];
    int max_heat;
    AttributeType current_sorting;
    AttributeType to_be_sorted_by;
    int *list_order;
    FilterSettings filters;
    TrackTileTextureCache track_tile_cache;
} GpxCollection;

typedef struct KDNode
{
    GpxPoint *point;
    int axis;
    struct KDNode *left, *right;
} KDNode;

typedef struct
{
    GpxPoint **points;
    int start;
    int end;
    KDNode *tree;
    float radius2;
    int total_tracks;
    int *thread_max_heat;
    pthread_mutex_t *max_mutex;
    pthread_mutex_t *progress_mutex;
    int thread_progress;
    int *total_progress;
} HeatmapTask;

#endif