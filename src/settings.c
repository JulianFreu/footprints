#include "settings.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The key used to be pasted into src/api_key.h and compiled in. It lives in the
// settings file now, but a header left over from before is still worth reading
// once so an existing checkout does not lose its key. This is the only place
// that still knows the header exists.
#if defined(__has_include)
#if __has_include("api_key.h")
#include "api_key.h"
#define HAVE_LEGACY_API_KEY 1
#endif
#endif

Settings settings;

// --- Providers ---

// Indexed by MapProvider. The id is what the settings file stores; see the
// comment on the enum for why it is a name rather than the ordinal.
static const char *const provider_ids[MAP_PROVIDER_COUNT] = {
    [MAP_PROVIDER_OSM] = "osm",
    [MAP_PROVIDER_STADIA_TERRAIN] = "stadia_stamen_terrain",
    [MAP_PROVIDER_STADIA_TONER_LITE] = "stadia_stamen_toner_lite",
    [MAP_PROVIDER_STADIA_SMOOTH_DARK] = "stadia_alidade_smooth_dark",
    [MAP_PROVIDER_STADIA_OUTDOORS] = "stadia_outdoors",
};

const char *map_provider_id(MapProvider provider) {
    if (provider < 0 || provider >= MAP_PROVIDER_COUNT)
        return provider_ids[MAP_PROVIDER_OSM];
    return provider_ids[provider];
}

MapProvider map_provider_from_id(const char *id) {
    if (!id)
        return MAP_PROVIDER_OSM;

    for (int i = 0; i < MAP_PROVIDER_COUNT; i++)
        if (strcmp(id, provider_ids[i]) == 0)
            return (MapProvider)i;

    // A name from a newer version, or a typo. The default is the provider that
    // needs no key and so always works.
    return MAP_PROVIDER_OSM;
}

// --- Defaults ---

void settings_defaults(Settings *s) {
    memset(s, 0, sizeof(*s));

    // The straight line: setpoint i at i/(N-1) of the way up both scales, which
    // is the identity, which is the ramp as it was before it was adjustable.
    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        s->heat_setpoint[i] = i * 100 / (HEAT_SETPOINT_COUNT - 1);

    s->provider = MAP_PROVIDER_OSM;
    s->stadia_api_key[0] = '\0';

    snprintf(s->gpx_dir, sizeof(s->gpx_dir), "%s", GPX_INPUT_DIR);
    s->garmin_email[0] = '\0';
    s->strava_client_id[0] = '\0';

    s->heat_radius_pixels = HEAT_RADIUS_PIXELS;
    s->track_point_size = TRACK_POINT_SIZE;

    s->start_zoom = START_ZOOM;
    s->start_world_x = START_WORLD_X;
    s->start_world_y = START_WORLD_Y;
}

void settings_seed_legacy_api_key(Settings *s) {
#ifdef HAVE_LEGACY_API_KEY
    if (s->stadia_api_key[0] == '\0' && api_key && api_key[0] != '\0')
        snprintf(s->stadia_api_key, sizeof(s->stadia_api_key), "%s", api_key);
#else
    (void)s;
#endif
}

// --- Repair ---

static int clamp_int(int value, int low, int high) {
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

static float clamp_float(float value, float low, float high) {
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

void settings_repair_setpoints(Settings *s) {
    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        s->heat_setpoint[i] = clamp_int(s->heat_setpoint[i], 0, 100);

    // Forward first, which is what an edit usually needs: the value just typed
    // pushes the ones above it out of its way.
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        if (s->heat_setpoint[i] <= s->heat_setpoint[i - 1])
            s->heat_setpoint[i] = s->heat_setpoint[i - 1] + 1;

    // Going forward can run the top off the end -- six values all at 100 come
    // out as 100..105. Pin the top back down and push the other way, which
    // cannot run off the bottom in turn: there are only five gaps to make and a
    // hundred to make them in.
    if (s->heat_setpoint[HEAT_SETPOINT_COUNT - 1] > 100) {
        s->heat_setpoint[HEAT_SETPOINT_COUNT - 1] = 100;
        for (int i = HEAT_SETPOINT_COUNT - 1; i > 0; i--)
            if (s->heat_setpoint[i - 1] >= s->heat_setpoint[i])
                s->heat_setpoint[i - 1] = s->heat_setpoint[i] - 1;
    }
}

void settings_repair_setpoints_around(Settings *s, int anchor) {
    if (anchor < 0 || anchor >= HEAT_SETPOINT_COUNT) {
        settings_repair_setpoints(s);
        return;
    }

    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        s->heat_setpoint[i] = clamp_int(s->heat_setpoint[i], 0, 100);

    // The anchor keeps what it was given, but it still has to leave room for
    // the setpoints either side of it: one whole number each, since they have
    // to be strictly below and strictly above.
    s->heat_setpoint[anchor] = clamp_int(s->heat_setpoint[anchor], anchor,
                                         100 - (HEAT_SETPOINT_COUNT - 1 - anchor));

    // Outwards from the anchor in both directions, so everything moves away
    // from it rather than it moving away from everything.
    for (int i = anchor; i > 0; i--)
        if (s->heat_setpoint[i - 1] >= s->heat_setpoint[i])
            s->heat_setpoint[i - 1] = s->heat_setpoint[i] - 1;

    for (int i = anchor; i < HEAT_SETPOINT_COUNT - 1; i++)
        if (s->heat_setpoint[i + 1] <= s->heat_setpoint[i])
            s->heat_setpoint[i + 1] = s->heat_setpoint[i] + 1;
}

static void settings_repair(Settings *s) {
    settings_repair_setpoints(s);

    s->provider = (MapProvider)clamp_int((int)s->provider, 0, MAP_PROVIDER_COUNT - 1);
    s->heat_radius_pixels = clamp_float(s->heat_radius_pixels, HEAT_RADIUS_MIN, HEAT_RADIUS_MAX);
    s->track_point_size = clamp_int(s->track_point_size, TRACK_POINT_SIZE_MIN, TRACK_POINT_SIZE_MAX);
    s->start_zoom = clamp_int(s->start_zoom, MIN_ZOOM, MAX_ZOOM);

    // An empty folder would make the parser open the working directory and find
    // nothing, which reads as an empty library rather than as a bad setting.
    if (s->gpx_dir[0] == '\0')
        snprintf(s->gpx_dir, sizeof(s->gpx_dir), "%s", GPX_INPUT_DIR);
}

// --- The colour ramp ---

float heat_normalized(const Settings *s, int heat, int max_heat) {
    // Every point shares the same heat when nothing in the collection overlaps
    // -- a single track, or tracks that never come near each other. Measured as
    // a share of the maximum that is 100%, which would paint a lone track in
    // the hottest colour the map has. Collapse it onto the bottom of the ramp
    // instead, which is what it did before the ramp was adjustable.
    if (max_heat <= 1)
        return 0.0f;

    float percent = 100.0f * (float)heat / (float)max_heat;

    if (percent <= (float)s->heat_setpoint[0])
        return 0.0f;
    if (percent >= (float)s->heat_setpoint[HEAT_SETPOINT_COUNT - 1])
        return 1.0f;

    // Which segment it lands in, and how far into it. The setpoints are
    // strictly increasing, so the gap below is never zero.
    for (int i = 0; i < HEAT_SETPOINT_COUNT - 1; i++) {
        float low = (float)s->heat_setpoint[i];
        float high = (float)s->heat_setpoint[i + 1];
        if (percent < high) {
            float within = (percent - low) / (high - low);
            return ((float)i + within) / (float)(HEAT_SETPOINT_COUNT - 1);
        }
    }

    return 1.0f; // unreachable; the bound above already caught it
}

// --- Parsing ---

// Whitespace either side of a key or a value is the writer's formatting rather
// than part of what was written.
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;

    char *end = text + strlen(text);
    while (end > text) {
        char previous = *(end - 1);
        if (previous != ' ' && previous != '\t' && previous != '\r' && previous != '\n')
            break;
        end--;
    }
    *end = '\0';
    return text;
}

// Parses a whole value, rejecting trailing rubbish. "12abc" is a mistake worth
// ignoring rather than a 12 worth acting on. Both callers hand these an already
// trimmed string, so anything left after the number really is rubbish.
static bool parse_int(const char *text, int *out) {
    if (!*text)
        return false;

    char *end;
    long value = strtol(text, &end, 10);
    if (*end != '\0' || value < INT_MIN || value > INT_MAX)
        return false;

    *out = (int)value;
    return true;
}

static bool parse_float(const char *text, float *out) {
    if (!*text)
        return false;

    char *end;
    double value = strtod(text, &end);
    if (*end != '\0')
        return false;

    *out = (float)value;
    return true;
}

// The setpoints arrive as one comma-separated list. Applied only if the whole
// list parses and is the right length: a half-read curve is worse than the
// default one, because it would look deliberate.
static bool parse_setpoints(const char *text, int *out) {
    int parsed[HEAT_SETPOINT_COUNT];
    int count = 0;

    char buffer[SETTINGS_LINE_MAX];
    snprintf(buffer, sizeof(buffer), "%s", text);

    for (char *token = strtok(buffer, ","); token; token = strtok(NULL, ",")) {
        if (count >= HEAT_SETPOINT_COUNT)
            return false; // too many
        if (!parse_int(trim(token), &parsed[count]))
            return false;
        count++;
    }

    if (count != HEAT_SETPOINT_COUNT)
        return false; // too few

    memcpy(out, parsed, sizeof(parsed));
    return true;
}

static void apply_pair(Settings *s, const char *key, const char *value) {
    if (strcmp(key, "heat_setpoints") == 0) {
        parse_setpoints(value, s->heat_setpoint);
    } else if (strcmp(key, "map_provider") == 0) {
        s->provider = map_provider_from_id(value);
    } else if (strcmp(key, "stadia_api_key") == 0) {
        snprintf(s->stadia_api_key, sizeof(s->stadia_api_key), "%s", value);
    } else if (strcmp(key, "gpx_dir") == 0) {
        snprintf(s->gpx_dir, sizeof(s->gpx_dir), "%s", value);
    } else if (strcmp(key, "garmin_email") == 0) {
        snprintf(s->garmin_email, sizeof(s->garmin_email), "%s", value);
    } else if (strcmp(key, "strava_client_id") == 0) {
        snprintf(s->strava_client_id, sizeof(s->strava_client_id), "%s", value);
    } else if (strcmp(key, "heat_radius") == 0) {
        parse_float(value, &s->heat_radius_pixels);
    } else if (strcmp(key, "point_size") == 0) {
        parse_int(value, &s->track_point_size);
    } else if (strcmp(key, "start_zoom") == 0) {
        parse_int(value, &s->start_zoom);
    } else if (strcmp(key, "start_world_x") == 0) {
        parse_int(value, &s->start_world_x);
    } else if (strcmp(key, "start_world_y") == 0) {
        parse_int(value, &s->start_world_y);
    }
    // Anything else is from a newer version, or a typo. Either way the rest of
    // the file is still worth reading.
}

bool settings_load(Settings *s, const char *path) {
    settings_defaults(s);

    FILE *file = fopen(path, "r");
    if (!file)
        return false; // first run; the defaults stand

    char line[SETTINGS_LINE_MAX];
    while (fgets(line, sizeof(line), file)) {
        char *text = trim(line);
        if (*text == '\0' || *text == '#')
            continue;

        char *separator = strchr(text, '=');
        if (!separator)
            continue; // not a pair; skip it rather than guess

        *separator = '\0';
        apply_pair(s, trim(text), trim(separator + 1));
    }

    fclose(file);

    settings_repair(s);
    return true;
}

bool settings_save(const Settings *s, const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Could not write settings to %s\n", path);
        return false;
    }

    fprintf(file, "# footprints settings, written by the settings panel.\n");
    fprintf(file, "# Hand-editing is fine; anything unreadable falls back to the default.\n\n");

    fprintf(file, "# Percent of the maximum heat at each knot of the colour ramp,\n");
    fprintf(file, "# coldest first. Strictly increasing, 0..100.\n");
    fprintf(file, "heat_setpoints   = ");
    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        fprintf(file, "%s%d", i ? "," : "", s->heat_setpoint[i]);
    fprintf(file, "\n\n");

    fprintf(file, "map_provider     = %s\n", map_provider_id(s->provider));
    fprintf(file, "stadia_api_key   = %s\n", s->stadia_api_key);
    fprintf(file, "gpx_dir          = %s\n\n", s->gpx_dir);

    fprintf(file, "# The Garmin account the import panel signs in as, and the Strava API\n");
    fprintf(file, "# application it authorises through. Neither the password nor the client\n");
    fprintf(file, "# secret is written here -- only the tokens minted from them, under %s/\n",
            GARMIN_SESSION_DIR);
    fprintf(file, "# and %s/.\n", STRAVA_SESSION_DIR);
    fprintf(file, "garmin_email     = %s\n", s->garmin_email);
    fprintf(file, "strava_client_id = %s\n\n", s->strava_client_id);

    fprintf(file, "heat_radius      = %g\n", (double)s->heat_radius_pixels);
    fprintf(file, "point_size       = %d\n\n", s->track_point_size);

    fprintf(file, "# The view the window opens at, in world pixels at the maximum zoom.\n");
    fprintf(file, "start_zoom       = %d\n", s->start_zoom);
    fprintf(file, "start_world_x    = %d\n", s->start_world_x);
    fprintf(file, "start_world_y    = %d\n", s->start_world_y);

    bool ok = ferror(file) == 0;
    if (fclose(file) != 0)
        ok = false;

    if (!ok)
        fprintf(stderr, "Could not write settings to %s\n", path);

    return ok;
}
