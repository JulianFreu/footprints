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

// Where downloaded tiles are kept, and enough room to format a path or a tile
// URL into.
#define TILE_CACHE_DIR "tilecache"

// Scanned at startup for .gpx files. Both this and the tile cache are relative
// to the working directory, so the binary is run from the project root.
#define GPX_INPUT_DIR "./gpx_files"
#define TILE_PATH_MAX 256

// Sent to the tile provider; OSM's usage policy asks for an identifying agent.
#define TILE_USER_AGENT "footprints/1.0 (+https://github.com/JulianFreu/footprints)"

// Upper bound on cached tile textures, per cache. Each is a TILE_SIZE-square
// RGBA texture, so 256 of them is roughly 64 MB of video memory; past that the
// least recently used is dropped.
#define TILE_CACHE_MAX_ENTRIES 256

// Upper bound on tiles drawn in one frame. A 4K window at TILE_SIZE needs about
// 170; past this the outermost tiles are simply not drawn.
#define MAX_VISIBLE_TILES 512

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

// Line thickness of the selected track's polyline overlay.
#define SELECTED_TRACK_THICKNESS 10.0f

// Pixel size of the elevation profile rendered for the selected track.
#define ELEVATION_PROFILE_WIDTH 200
#define ELEVATION_PROFILE_HEIGHT 100

// --- Heatmap ---
// Radius in projected pixels within which two tracks count as overlapping.
#define HEAT_RADIUS_PIXELS 200.0f

// --- UI ---
#define INPUT_BUFFER_SIZE 16

// Slide speed of the side panels, in degrees of the easing curve per second.
// The curve spans 90 degrees, so this opens a panel in about a third of a
// second regardless of frame rate.
#define PANEL_ANIMATION_DEGREES_PER_SECOND 270.0f

// Scroll distance contributed by one mouse-wheel detent.
#define SCROLL_PIXELS_PER_WHEEL_STEP 5

#endif
