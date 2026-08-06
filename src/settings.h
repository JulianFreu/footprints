#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

// What the settings panel writes and startup reads back.
//
// The tunables here used to be the compile-time constants in config.h, which
// are now the defaults rather than the values: a fresh checkout with no
// settings file behaves exactly as it did before this file existed.
//
// Nothing in here includes SDL or pthreads, which is deliberate -- it is what
// lets tests/test_settings.c include settings.c the way the other suites
// include the module they cover.

// The tile servers the map can be drawn from. Everything but OpenStreetMap is
// Stadia Maps and needs a key.
//
// The enum's order is what the panel draws; it is not what the file stores.
// map_provider_id is, so that inserting a provider here cannot silently
// repoint somebody's saved choice at a different map.
typedef enum MapProvider {
    MAP_PROVIDER_OSM = 0,
    MAP_PROVIDER_STADIA_TERRAIN,
    MAP_PROVIDER_STADIA_TONER_LITE,
    MAP_PROVIDER_STADIA_SMOOTH_DARK,
    MAP_PROVIDER_STADIA_OUTDOORS,
    MAP_PROVIDER_COUNT
} MapProvider;

typedef struct Settings {
    // Knots of a piecewise-linear curve from "percent of the maximum heat" to
    // "position along the colour ramp". Setpoint i is pinned to i/(N-1) of the
    // way along the ramp; what moves is the percentage of heat that arrives
    // there. The colours between two setpoints are still interpolated the way
    // they always were -- these bend the curve, they do not band it.
    //
    // Strictly increasing and within 0..100. The default 0,20,40,60,80,100 is
    // the straight line, and draws exactly what the map drew before the
    // setpoints existed.
    int heat_setpoint[HEAT_SETPOINT_COUNT];

    MapProvider provider;
    char stadia_api_key[SETTINGS_KEY_MAX];

    char gpx_dir[SETTINGS_PATH_MAX];

    // The account the import panel signs in to Garmin as, and the Strava API
    // application it authorises through. The password and the client secret
    // that go with them are deliberately not here: each is traded for a token
    // once and then dropped, and this file is plain text.
    char garmin_email[SETTINGS_KEY_MAX];
    char strava_client_id[SETTINGS_KEY_MAX];

    // Feed the heat calculation and the tiles rasterised from it.
    float heat_radius_pixels;
    int track_point_size;

    // The view the window opens at, in world pixels at MAX_ZOOM.
    int start_zoom;
    int start_world_x;
    int start_world_y;
} Settings;

// The one instance. A global for the same reason use_osm_tiles was one: the
// tile download worker is handed only its queue, so there is no application
// pointer down there to reach a member through.
extern Settings settings;

// Fills `s` with the compile-time defaults. Never fails.
void settings_defaults(Settings *s);

// Reads `path` over the defaults. A missing file is not a failure -- it is the
// first run -- and returns false only so a caller that cares can tell the two
// apart. Anything unparseable, unknown or out of range is repaired rather than
// rejected, because this file is meant to be hand-edited.
bool settings_load(Settings *s, const char *path);

// Writes the whole file. Called on every committed change: it is a few hundred
// bytes, so there is nothing to be gained by batching and a crash to lose by it.
bool settings_save(const Settings *s, const char *path);

// Puts the setpoints back in order: inside 0..100 and strictly increasing, so
// heat_normalized can divide by a gap it knows is positive. This is the form
// for a file, where no one value has a better claim than another.
void settings_repair_setpoints(Settings *s);

// The same, for after an edit: `anchor` is the setpoint that was just typed, so
// it keeps the value it was given and the others move out of its way -- earlier
// ones down, later ones up.
//
// Which way they move is the whole point. Typing 60 into the last setpoint
// means "60 % of the maximum and above is the hottest colour", and repairing it
// the other way round answers by moving the 60 instead.
void settings_repair_setpoints_around(Settings *s, int anchor);

// Carries a key over from the compiled-in src/api_key.h, which is how the key
// used to be supplied, if the settings file did not have one. Separate from
// settings_load so that reading a file stays a function of that file alone.
void settings_seed_legacy_api_key(Settings *s);

// Where a point's heat falls along the colour ramp, from 0 (coldest) to 1
// (hottest). The caller owns the ramp; this only says how far along it to look.
float heat_normalized(const Settings *s, int heat, int max_heat);

// The name a provider is stored under, and the provider a stored name means.
// An unrecognised name gives OpenStreetMap, which is the one that always works.
const char *map_provider_id(MapProvider provider);
MapProvider map_provider_from_id(const char *id);

#endif
