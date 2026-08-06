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
// URL into. Each provider gets a subdirectory of its own under here: the same
// z/x/y names a different picture depending on who drew it, so one shared tree
// would hand OpenStreetMap tiles back for a Stadia map.
#define TILE_CACHE_DIR "tilecache"

// The default scanned at startup for .gpx files, overridable in the settings.
// Both this and the tile cache are relative to the working directory, so the
// binary is run from the project root.
#define GPX_INPUT_DIR "./gpx_files"
#define TILE_PATH_MAX 256

// Enough to build a path to a file anywhere under the library folder, which is
// itself up to SETTINGS_PATH_MAX. A name that will not fit is skipped rather
// than truncated into a path to something else.
#define GPX_PATH_MAX 1024
// How far the scan descends below the library folder. The imports write into
// subfolders of it, so one level is the point; the rest is room to file a
// library by year or by activity, and a limit at all is what keeps a symlink
// loop finite.
#define GPX_SCAN_MAX_DEPTH 8

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

// Fastest a runner is taken to be moving, in metres per second, when the record
// distances are searched for. 7 m/s is 2:23 per kilometre -- quicker than any
// amateur holds for five kilometres, and slower than anything on wheels.
//
// This is not noise filtering. A library exported from a phone app contains
// files labelled "Running" whose second half is a drive home, and a search for
// a *minimum* finds exactly those: one such file offered a five-kilometre
// record of 6:28. A stretch above this speed ends the run being scanned, the
// same way a clock that goes backwards does, so the honest part of a track can
// still hold a record while the drive cannot.
#define SPLIT_MAX_SPEED_MPS 7.0f

// Default side length, in pixels, of the square stamped down for each track
// point on the heat tiles, and what the settings will accept for it.
#define TRACK_POINT_SIZE 4
#define TRACK_POINT_SIZE_MIN 1
#define TRACK_POINT_SIZE_MAX 32

// Line thickness of the selected track's polyline overlay.
#define SELECTED_TRACK_THICKNESS 10.0f

// Pixel width of the graphs rendered for the selected track, and so of the
// sidebar holding them: the panel is this plus its padding. It is the one
// number to change to make that sidebar wider or narrower, and the surfaces
// still land on the screen at their own scale rather than being stretched to
// fit a width chosen separately from them.
#define TRACK_GRAPH_WIDTH 320
// Their height is not a number set here: the graphs divide whatever the
// attribute rows above them leave, three ways, so that they reach the bottom of
// the panel however tall the window is. This is only the floor under that
// division, so that a window with nothing left to divide cannot ask SDL for a
// surface of no height. Keep it small: a window this short is already squashing
// the graphs to fit, and anything larger here would have them rasterised bigger
// than the box they are drawn into.
#define TRACK_GRAPH_MIN_HEIGHT 8

// The two bounds on how dense the horizontal rules across a graph may be.
//
// Each kind has a step it is drawn at by choice -- fifty metres of climb,
// thirty seconds per kilometre, twenty beats. It is coarsened, to 100m then
// 200m and up, whenever those rules would land closer together than
// TRACK_GRAPH_GRID_MIN_SPACING pixels, which is what keeps a mountain from
// coming out hatched and what makes the same climb read the same way in a
// short window and a tall one. The same idea as STATS_XLABEL_MIN_WIDTH, which
// thins the statistics plot's labels for the same reason.
//
// It is refined the other way only when the chosen step would draw fewer than
// TRACK_GRAPH_GRID_MIN_RULES rules -- a run that held one pace, where the range
// is narrower than a step, would otherwise have nothing drawn across it at all.
// Two is the point: one rule measures nothing, having nothing to be spaced
// from.
#define TRACK_GRAPH_GRID_MIN_SPACING 24
#define TRACK_GRAPH_GRID_MIN_RULES 2

// The value printed against each of those rules, at the graph's left edge. A
// point under the 12 the rest of the small text is set at -- that size lives in
// ui_internal.h, which is the UI's own vocabulary and not for the rasteriser to
// read -- because these numbers sit over a drawn curve rather than on a panel,
// and want to be read without being what the eye lands on first. The padding is
// both the gap from the left edge and the gap above the rule.
#define TRACK_GRAPH_LABEL_FONT_SIZE 11
#define TRACK_GRAPH_LABEL_PADDING 3

// How much ground the pace window spans, in metres. Pace between two
// consecutive fixes is mostly GPS noise -- a metre of scatter on a one-second
// sample is a minute per kilometre -- so it is measured across a fixed stretch
// of the route instead, which also makes the result independent of how often
// the watch recorded.
#define PACE_WINDOW_METERS 100.0f

// --- Heatmap ---
// Default radius in projected pixels within which two tracks count as
// overlapping, overridable in the settings.
#define HEAT_RADIUS_PIXELS 200.0f
// What the settings will accept for it. The lower bound is not zero: a radius
// of nothing gives every point the same heat and a flat map, which reads as a
// bug rather than as a setting.
#define HEAT_RADIUS_MIN 10.0f
#define HEAT_RADIUS_MAX 2000.0f

// How many points a heat worker finishes before publishing progress and
// checking whether it has been asked to stop.
#define HEAT_PROGRESS_BATCH 256

// Knots of the curve taking a point's share of the maximum heat to a position
// along the colour ramp. Six is what the settings panel draws a field for.
#define HEAT_SETPOINT_COUNT 6

// --- Settings ---
// Written beside the tile cache and the GPX folder, and read at startup. Plain
// text, so it can be hand-edited.
#define SETTINGS_FILE "settings.conf"
// The longest a settings value can be. The key is the longer of the two in
// practice; the path has to hold an absolute one.
#define SETTINGS_KEY_MAX 128
#define SETTINGS_PATH_MAX 512
// One line of the file, and the buffer a field is edited in.
#define SETTINGS_LINE_MAX (SETTINGS_PATH_MAX + 64)
#define SETTINGS_EDIT_MAX SETTINGS_PATH_MAX

// --- Activity import ---
// One helper per provider, the folder each writes into under the library
// folder, and where the token it mints is kept. All relative to the working
// directory, like the tile cache and the library.
#define GARMIN_SCRIPT "garmin_sync.py"
#define GARMIN_IMPORT_SUBDIR "garmin_import"
#define GARMIN_SESSION_DIR "garmin_session"
// Written by garth, and what tells the panel a password is no longer needed.
#define GARMIN_TOKEN_FILE "oauth2_token.json"

#define STRAVA_SCRIPT "strava_sync.py"
#define STRAVA_IMPORT_SUBDIR "strava_import"
#define STRAVA_SESSION_DIR "strava_session"
// Written by strava_sync.py, and what tells the panel the browser step is done.
// It carries the client secret as well as the refresh token, which is why it is
// this file rather than settings.conf that holds one.
#define STRAVA_TOKEN_FILE "token.json"

// The interpreter the helpers are run with. Spelled with the major version: on
// several distributions "python" is either Python 2 or absent entirely.
#define IMPORT_PYTHON "python3"

// The longest line a helper is read a line at a time in, and the longest of
// its messages the panel keeps to show.
#define IMPORT_LINE_MAX 512
#define IMPORT_MESSAGE_MAX 160
// A credential on its way to a helper -- an email and a password, or a Strava
// application's id and secret. They live only as long as it takes to write them
// to it.
#define IMPORT_CREDENTIAL_MAX SETTINGS_KEY_MAX

// --- UI ---
// How long a filter field has to sit still before the heat tiles are
// rasterised again. The run list, the counter and the derived panels follow
// every keystroke -- they are a pass over eleven hundred tracks. The tiles are
// a pass over more than a million points, so they wait for the digit after
// this one not to arrive.
#define FILTER_SETTLE_SECONDS 0.15f

// The caret's full cycle, on for the first half and off for the second.
#define FILTER_CARET_BLINK_SECONDS 1.06f

// How long a side panel takes to slide all the way open or shut. The same
// stretch of time is what it has to fade in over, so a panel is invisible when
// it starts moving and fully opaque the moment it lands.
#define PANEL_ANIMATION_SECONDS 0.20f

// Rows the run list moves for one mouse-wheel detent. Scrolling in whole rows
// from a row-aligned start is what keeps the rows on their grid.
#define RUN_LIST_SCROLL_ROWS_PER_STEP 3

// How long the list takes to catch up with where the wheel has put it, as a
// half-life. The one number the scroll feel is retuned by.
#define RUN_LIST_SCROLL_TAU 0.06f

// Periods the statistics plot pans for one mouse-wheel detent, and how long it
// takes to get there, as a half-life. The two numbers the pan feel is tuned by.
// Panning is measured in periods rather than pixels, so a detent moves the same
// amount of calendar whatever the bars happen to be wide.
#define STATS_PAN_PERIODS_PER_STEP 3
#define STATS_PAN_TAU 0.06f

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
