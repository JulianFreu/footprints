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

// Longest frame an animation is stepped by. A frame that takes far longer than
// the rest -- adopting a background job's results, dragging the window, a
// breakpoint -- would otherwise advance every animation most of the way to its
// end in one step.
#define MAX_FRAME_DELTA_SECONDS 0.1f

// Floor under the frame rate. Vsync normally paces the drawing frames, but a
// renderer can report it and not wait, so this stays as the backstop.
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

// Drawn where a tile is missing or has not finished fading in. Close to the
// land colour of the tiles themselves, so ground that has not arrived yet
// reads as blank map rather than as a hole.
#define TILE_FALLBACK_COLOR_R 0
#define TILE_FALLBACK_COLOR_G 0
#define TILE_FALLBACK_COLOR_B 0

// Tiles decoded and uploaded per frame. The PNG decode is synchronous, so a
// pan that reveals forty cached tiles at once would otherwise spend the whole
// frame on them. The rest arrive over the next few frames, covered by the
// fallback colour in the meantime.
#define TILE_DECODES_PER_FRAME 8

// How long a tile takes to come up to full strength once its texture exists.
#define TILE_FADE_SECONDS 0.50

// Heat tiles rasterised per frame, for the same reason. A frame drawn at half
// scale needs four times as many tiles, and the first frame of a zoom is
// exactly where that cost would land.
#define HEAT_RENDERS_PER_FRAME 8

// How long a tile that failed to download is left alone before being asked for
// again, and how many tiles can be waiting or cooling off at once.
#define TILE_RETRY_SECONDS 30
#define TILE_PENDING_MAX 128

// Sent to the tile provider; OSM's usage policy asks for an identifying agent.
#define TILE_USER_AGENT "footprints/1.0 (+https://github.com/JulianFreu/footprints)"

// Upper bound on cached tile textures, per cache. Each is a TILE_SIZE-square
// RGBA texture, so 256 of them is roughly 64 MB of video memory; past that the
// least recently used is dropped.
#define TILE_CACHE_MAX_ENTRIES 1024

// Upper bound on tiles drawn in one frame; past this the outermost tiles are
// simply not drawn. A 4K window needs about 170 at scale 1. The rest is for a
// zoom still easing: each octave of lag quadruples the count, so the same
// window needs 627 one level down and 2205 two levels down -- and
// ZOOM_MAX_OCTAVES caps the lag at two. This is a stack array in the frame
// loop, so at 24 bytes an entry it is about 96 KB.
#define MAX_VISIBLE_TILES 4096

// --- Startup view ---
// World pixel coordinates at MAX_ZOOM, and the zoom level to open at.
#define START_ZOOM 12
#define START_WORLD_X 140750875
#define START_WORLD_Y 89004498

// --- Tracks ---
// Half-width, in samples, of the centred moving average applied to elevation
// before gain and loss are accumulated. GPS altitude is noisy enough that the
// raw series roughly doubles the real climb.
#define ELEVATION_SMOOTHING_WINDOW 5

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

// How many points a heat worker finishes before publishing progress and
// checking whether it has been asked to stop.
#define HEAT_PROGRESS_BATCH 256

// --- UI ---
#define INPUT_BUFFER_SIZE 16

// How long a side panel takes to slide all the way open or shut.
#define PANEL_ANIMATION_SECONDS 0.33f

// Scroll distance contributed by one mouse-wheel detent.
#define SCROLL_PIXELS_PER_WHEEL_STEP 5

// --- Zooming ---
// How long the picture takes to catch up with a zoom level the model has
// already snapped to. The one number the feel is retuned by.
#define ZOOM_ANIMATION_SECONDS 0.15f

// How far the picture is ever allowed to lag the model, in powers of two. Two
// detents in quick succession is an ordinary gesture and has to stay smooth,
// so the limit is above it; a longer burst snaps through the levels in between
// and eases only the last. What sets the ceiling is MAX_VISIBLE_TILES, since
// each octave of lag quadruples the tiles a frame needs.
#define ZOOM_MAX_OCTAVES 2.0f

// Scratch for the strings drawn in one frame. The run list is virtualised to
// about 25 rows of 8 columns, plus the sidebar, so 64 KB is far more than a
// frame can use.
#define FRAME_TEXT_ARENA_BYTES 65536

// The panel shown while the library is being read or the heat recalculated.
// Longest single string ui_frame_printf will produce.
#define UI_FRAME_STRING_MAX 128

#define PROGRESS_PANEL_WIDTH 320
#define PROGRESS_PANEL_HEIGHT 80
#define PROGRESS_BAR_HEIGHT 16

#endif
