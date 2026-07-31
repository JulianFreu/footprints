#ifndef CONFIG_H
#define CONFIG_H

// Compile-time configuration. Everything here is a tunable rather than a type,
// kept separate so a module can pull in constants without dragging in the SDL
// and pthread headers the type headers need.

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Window ---
#define WINDOW_TITLE "footprints"
#define SCREEN_WIDTH 1200
#define SCREEN_HEIGHT 800
#define TARGET_FPS 60
#define FRAME_DELAY_MS (1000 / TARGET_FPS)

// --- Map tiles ---
#define TILE_SIZE 256

// The latitude at which the Web Mercator projection closes into a square. Past
// it the projection is undefined, so coordinates are clamped here.
#define MERCATOR_MAX_LATITUDE 85.05112878
#define MIN_ZOOM 4
#define MAX_ZOOM 20
#define FIFO_DEPTH 16

// --- Startup view ---
// World pixel coordinates at MAX_ZOOM, and the zoom level to open at.
#define START_ZOOM 12
#define START_WORLD_X 140750875
#define START_WORLD_Y 89004498

// --- Tracks ---
// Half-width, in samples, of the centred moving average applied to elevation
// before gain and loss are accumulated. GPS altitude is noisy enough that the
// raw series roughly doubles the real climb.
#define ELEVATION_SMOOTHING_WINDOW 10

// Side length, in pixels, of the square stamped down for each track point on
// the heat tiles.
#define TRACK_POINT_SIZE 4

// --- Heatmap ---
// Radius in projected pixels within which two tracks count as overlapping.
#define HEAT_RADIUS_PIXELS 200.0f

// --- UI ---
#define INPUT_BUFFER_SIZE 16

#endif
