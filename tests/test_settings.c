#include "../src/settings.c"

#include "harness.h"

// The settings file is meant to be hand-edited, so most of what is worth
// testing here is what happens to a file somebody got wrong: a curve that runs
// backwards, a value out of range, a key from a version that does not exist
// yet. None of those may stop the application starting.

// Written and read back within one suite, so the name only has to be unlikely
// rather than unique across machines.
#define TEST_PATH "settings-test.tmp"

static void write_file(const char *contents) {
    FILE *file = fopen(TEST_PATH, "w");
    if (!file) {
        FAIL("could not open %s for writing", TEST_PATH);
        return;
    }
    fputs(contents, file);
    fclose(file);
}

static void remove_file(void) {
    remove(TEST_PATH);
}

// The ramp position of a point carrying `percent` of the maximum heat. Every
// expectation below is written in those terms, so the arithmetic turning a
// percentage back into a heat count lives in one place.
static float ramp_at(const Settings *s, int percent) {
    return heat_normalized(s, percent, 100);
}

static void test_defaults(void) {
    SUITE("settings: the defaults are the compiled-in constants");

    Settings s;
    settings_defaults(&s);

    CHECK_INT(s.provider, MAP_PROVIDER_OSM);
    CHECK_STR(s.stadia_api_key, "");
    CHECK_STR(s.gpx_dir, GPX_INPUT_DIR);
    CHECK_NEAR(s.heat_radius_pixels, HEAT_RADIUS_PIXELS, 0.001);
    CHECK_INT(s.track_point_size, TRACK_POINT_SIZE);
    CHECK(!s.show_profiler);
    // On rather than off: a heat map whose colours cannot be read as numbers is
    // what the readout exists to fix, so it is there before it is asked for.
    CHECK(s.show_heat_tooltip);
    CHECK_INT(s.start_zoom, START_ZOOM);
    CHECK_INT(s.start_world_x, START_WORLD_X);
    CHECK_INT(s.start_world_y, START_WORLD_Y);

    // The straight line, which is the ramp as it was before it was adjustable.
    CHECK_INT(s.heat_setpoint[0], 0);
    CHECK_INT(s.heat_setpoint[HEAT_SETPOINT_COUNT - 1], 100);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);
}

static void test_round_trip(void) {
    SUITE("settings: what is written is what is read back");

    Settings written;
    settings_defaults(&written);
    written.provider = MAP_PROVIDER_STADIA_SMOOTH_DARK;
    snprintf(written.stadia_api_key, sizeof(written.stadia_api_key), "abc-123-key");
    snprintf(written.gpx_dir, sizeof(written.gpx_dir), "/home/someone/tracks");
    written.heat_radius_pixels = 275.0f;
    written.track_point_size = 7;
    written.show_profiler = true;
    // The opposite of its default, which is what makes the trip prove anything.
    written.show_heat_tooltip = false;
    written.start_zoom = 14;
    written.start_world_x = 12345678;
    written.start_world_y = 87654321;
    written.heat_setpoint[1] = 10;
    written.heat_setpoint[2] = 25;
    written.heat_setpoint[3] = 40;
    written.heat_setpoint[4] = 55;
    written.heat_setpoint[5] = 70;

    CHECK(settings_save(&written, TEST_PATH));

    Settings read;
    CHECK(settings_load(&read, TEST_PATH));

    CHECK_INT(read.provider, written.provider);
    CHECK_STR(read.stadia_api_key, written.stadia_api_key);
    CHECK_STR(read.gpx_dir, written.gpx_dir);
    CHECK_NEAR(read.heat_radius_pixels, written.heat_radius_pixels, 0.001);
    CHECK_INT(read.track_point_size, written.track_point_size);
    CHECK_INT(read.show_profiler, written.show_profiler);
    CHECK_INT(read.show_heat_tooltip, written.show_heat_tooltip);
    CHECK_INT(read.start_zoom, written.start_zoom);
    CHECK_INT(read.start_world_x, written.start_world_x);
    CHECK_INT(read.start_world_y, written.start_world_y);
    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        CHECK_INT(read.heat_setpoint[i], written.heat_setpoint[i]);

    remove_file();
}

static void test_missing_file(void) {
    SUITE("settings: a missing file is the first run, not a failure");

    remove_file();

    Settings s;
    // False says the file was not there; the settings are still usable.
    CHECK(!settings_load(&s, TEST_PATH));

    Settings expected;
    settings_defaults(&expected);
    CHECK_INT(s.track_point_size, expected.track_point_size);
    CHECK_STR(s.gpx_dir, expected.gpx_dir);
    CHECK_NEAR(s.heat_radius_pixels, expected.heat_radius_pixels, 0.001);
}

static void test_partial_and_unknown_keys(void) {
    SUITE("settings: unknown keys are skipped and the rest still applies");

    write_file("# a comment\n"
               "\n"
               "point_size = 9\n"
               "future_option = whatever it means\n"
               "this line has no separator\n"
               "= value with no key\n"
               "  heat_radius   =   150  \n");

    Settings s;
    CHECK(settings_load(&s, TEST_PATH));

    CHECK_INT(s.track_point_size, 9);
    CHECK_NEAR(s.heat_radius_pixels, 150.0f, 0.001);
    // Everything the file did not mention is left at its default.
    CHECK_STR(s.gpx_dir, GPX_INPUT_DIR);
    CHECK_INT(s.start_zoom, START_ZOOM);

    remove_file();
}

static void test_unparseable_values(void) {
    SUITE("settings: a value that is not a number keeps the default");

    write_file("point_size = 4kg\n"
               "heat_radius = wide\n"
               "start_zoom =\n");

    Settings s;
    CHECK(settings_load(&s, TEST_PATH));

    CHECK_INT(s.track_point_size, TRACK_POINT_SIZE);
    CHECK_NEAR(s.heat_radius_pixels, HEAT_RADIUS_PIXELS, 0.001);
    CHECK_INT(s.start_zoom, START_ZOOM);

    remove_file();
}

static void test_bool_values(void) {
    SUITE("settings: a flag is spelled several ways and guessed at in none");

    const char *const yes[] = {"true", "1", "on"};
    const char *const no[] = {"false", "0", "off"};

    // Both flags in each file, spelled opposite ways round, so one pass covers
    // turning each of them on and off -- and covers them being told apart,
    // which two keys sharing a parser and a prefix are worth checking.
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        char line[128];
        Settings s;

        snprintf(line, sizeof(line), "show_profiler = %s\nshow_heat_tooltip = %s\n",
                 yes[i], no[i]);
        write_file(line);
        CHECK(settings_load(&s, TEST_PATH));
        CHECK(s.show_profiler);
        CHECK(!s.show_heat_tooltip);

        snprintf(line, sizeof(line), "show_profiler = %s\nshow_heat_tooltip = %s\n",
                 no[i], yes[i]);
        write_file(line);
        CHECK(settings_load(&s, TEST_PATH));
        CHECK(!s.show_profiler);
        CHECK(s.show_heat_tooltip);
    }

    // Not an answer: the comparison is case-sensitive like the provider names,
    // "yes" was never one of the spellings, and neither an empty value nor a
    // number outside 0 and 1 says anything. Each leaves the default standing.
    // Surrounding space is not in this list -- the loader trims a value before
    // it is parsed, so "true " is the same word.
    // A flag defaulting to on is the case worth having here: it says the
    // default is what survives, rather than false being what a failed parse
    // happens to leave behind.
    const char *const nonsense[] = {"TRUE", "yes", "", "2"};
    for (size_t i = 0; i < sizeof(nonsense) / sizeof(nonsense[0]); i++) {
        char line[128];
        snprintf(line, sizeof(line), "show_profiler = %s\nshow_heat_tooltip = %s\n",
                 nonsense[i], nonsense[i]);
        write_file(line);

        Settings s;
        CHECK(settings_load(&s, TEST_PATH));
        CHECK(!s.show_profiler);
        CHECK(s.show_heat_tooltip);
    }

    // And because parse_bool writes only when it understood the value, an
    // unreadable one cannot turn a flag off that the rest of the file turned on.
    write_file("show_profiler = on\n"
               "show_profiler = banana\n"
               "show_heat_tooltip = off\n"
               "show_heat_tooltip = banana\n");

    Settings s;
    CHECK(settings_load(&s, TEST_PATH));
    CHECK(s.show_profiler);
    CHECK(!s.show_heat_tooltip);

    remove_file();
}

static void test_clamping(void) {
    SUITE("settings: values out of range are clamped, not rejected");

    write_file("point_size = 9999\n"
               "heat_radius = -5\n"
               "start_zoom = 99\n");

    Settings s;
    CHECK(settings_load(&s, TEST_PATH));

    CHECK_INT(s.track_point_size, TRACK_POINT_SIZE_MAX);
    CHECK_NEAR(s.heat_radius_pixels, HEAT_RADIUS_MIN, 0.001);
    CHECK_INT(s.start_zoom, MAX_ZOOM);

    write_file("point_size = 0\n"
               "heat_radius = 999999\n"
               "start_zoom = -3\n"
               "gpx_dir =\n");

    CHECK(settings_load(&s, TEST_PATH));
    CHECK_INT(s.track_point_size, TRACK_POINT_SIZE_MIN);
    CHECK_NEAR(s.heat_radius_pixels, HEAT_RADIUS_MAX, 0.001);
    CHECK_INT(s.start_zoom, MIN_ZOOM);
    // An empty folder would silently scan the working directory instead.
    CHECK_STR(s.gpx_dir, GPX_INPUT_DIR);

    remove_file();
}

static void test_setpoint_list_parsing(void) {
    SUITE("settings: a setpoint list is taken whole or not at all");

    Settings defaults;
    settings_defaults(&defaults);

    // Too few, too many, and one bad entry: each leaves the default curve,
    // because a half-read curve would look deliberate.
    const char *bad[] = {
        "heat_setpoints = 0,20,40\n",
        "heat_setpoints = 0,10,20,30,40,50,60\n",
        "heat_setpoints = 0,20,forty,60,80,100\n",
        "heat_setpoints =\n",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_file(bad[i]);

        Settings s;
        CHECK(settings_load(&s, TEST_PATH));
        for (int k = 0; k < HEAT_SETPOINT_COUNT; k++)
            CHECK_INT(s.heat_setpoint[k], defaults.heat_setpoint[k]);
    }

    // Spaces around the entries are the writer's formatting.
    write_file("heat_setpoints =  0 , 15 , 30 , 45 , 55 , 60 \n");

    Settings s;
    CHECK(settings_load(&s, TEST_PATH));
    CHECK_INT(s.heat_setpoint[0], 0);
    CHECK_INT(s.heat_setpoint[1], 15);
    CHECK_INT(s.heat_setpoint[4], 55);
    CHECK_INT(s.heat_setpoint[5], 60);

    remove_file();
}

static void test_setpoint_repair(void) {
    SUITE("settings: the curve is always strictly increasing");

    Settings s;

    // Backwards.
    settings_defaults(&s);
    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        s.heat_setpoint[i] = 100 - i * 20;
    settings_repair_setpoints(&s);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);

    // All the same, at the top of the range: pushing forward would run off the
    // end, so it has to come back down instead.
    settings_defaults(&s);
    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        s.heat_setpoint[i] = 100;
    settings_repair_setpoints(&s);
    CHECK_INT(s.heat_setpoint[HEAT_SETPOINT_COUNT - 1], 100);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);
    CHECK(s.heat_setpoint[0] >= 0);

    // All the same, at the bottom.
    settings_defaults(&s);
    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        s.heat_setpoint[i] = 0;
    settings_repair_setpoints(&s);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);
    CHECK(s.heat_setpoint[HEAT_SETPOINT_COUNT - 1] <= 100);

    // Outside the range entirely.
    settings_defaults(&s);
    s.heat_setpoint[0] = -40;
    s.heat_setpoint[HEAT_SETPOINT_COUNT - 1] = 900;
    settings_repair_setpoints(&s);
    CHECK(s.heat_setpoint[0] >= 0);
    CHECK(s.heat_setpoint[HEAT_SETPOINT_COUNT - 1] <= 100);

    // A file that says the curve runs backwards is repaired on load, not
    // trusted -- heat_normalized divides by the gaps.
    write_file("heat_setpoints = 90,80,70,60,50,40\n");
    CHECK(settings_load(&s, TEST_PATH));
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);

    remove_file();
}

static void test_setpoint_repair_around_an_edit(void) {
    SUITE("settings: the setpoint just typed is the one that is kept");

    Settings s;

    // The case this exists for. "60 % of the maximum and above is the hottest
    // colour" is typed into the last setpoint; repairing forward would answer
    // by moving the 60 up past the 80 next to it, which is not what was asked.
    settings_defaults(&s);
    s.heat_setpoint[HEAT_SETPOINT_COUNT - 1] = 60;
    settings_repair_setpoints_around(&s, HEAT_SETPOINT_COUNT - 1);
    CHECK_INT(s.heat_setpoint[HEAT_SETPOINT_COUNT - 1], 60);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);
    // And it means what it says: 60 % of the maximum is the hottest colour.
    CHECK_NEAR(ramp_at(&s, 60), 1.0f, 0.001);

    // The other end: a first setpoint typed above the ones after it pushes
    // them up rather than being dragged back down itself.
    settings_defaults(&s);
    s.heat_setpoint[0] = 70;
    settings_repair_setpoints_around(&s, 0);
    CHECK_INT(s.heat_setpoint[0], 70);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);

    // A middle setpoint pushes in both directions at once.
    settings_defaults(&s);
    s.heat_setpoint[2] = 95;
    settings_repair_setpoints_around(&s, 2);
    CHECK_INT(s.heat_setpoint[2], 95);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);

    // An anchor with no room for its neighbours is pulled in far enough to
    // make it, rather than pushing them off either end of the scale.
    settings_defaults(&s);
    s.heat_setpoint[HEAT_SETPOINT_COUNT - 1] = 0;
    settings_repair_setpoints_around(&s, HEAT_SETPOINT_COUNT - 1);
    CHECK(s.heat_setpoint[0] >= 0);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);

    settings_defaults(&s);
    s.heat_setpoint[0] = 100;
    settings_repair_setpoints_around(&s, 0);
    CHECK(s.heat_setpoint[HEAT_SETPOINT_COUNT - 1] <= 100);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);

    // An anchor outside the array is the unanchored repair rather than a
    // write past the end.
    settings_defaults(&s);
    s.heat_setpoint[3] = 5;
    settings_repair_setpoints_around(&s, -1);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);

    settings_defaults(&s);
    settings_repair_setpoints_around(&s, HEAT_SETPOINT_COUNT);
    for (int i = 1; i < HEAT_SETPOINT_COUNT; i++)
        CHECK(s.heat_setpoint[i] > s.heat_setpoint[i - 1]);
}

static void test_providers(void) {
    SUITE("settings: a provider survives being written out and read back");

    for (int i = 0; i < MAP_PROVIDER_COUNT; i++) {
        const char *id = map_provider_id((MapProvider)i);
        CHECK(id != NULL && id[0] != '\0');
        CHECK_INT(map_provider_from_id(id), i);
    }

    // A name from a version that does not exist yet, and no name at all. Both
    // give the provider that needs no key.
    CHECK_INT(map_provider_from_id("some_future_provider"), MAP_PROVIDER_OSM);
    CHECK_INT(map_provider_from_id(""), MAP_PROVIDER_OSM);
    CHECK_INT(map_provider_from_id(NULL), MAP_PROVIDER_OSM);

    // Every id has to be distinct, or one of them is unreachable.
    for (int i = 0; i < MAP_PROVIDER_COUNT; i++)
        for (int k = i + 1; k < MAP_PROVIDER_COUNT; k++)
            CHECK(strcmp(map_provider_id((MapProvider)i), map_provider_id((MapProvider)k)) != 0);
}

static void test_ramp_defaults(void) {
    SUITE("heat ramp: the default curve is the straight line");

    Settings s;
    settings_defaults(&s);

    CHECK_NEAR(ramp_at(&s, 0), 0.0f, 0.001);
    CHECK_NEAR(ramp_at(&s, 25), 0.25f, 0.001);
    CHECK_NEAR(ramp_at(&s, 50), 0.50f, 0.001);
    CHECK_NEAR(ramp_at(&s, 75), 0.75f, 0.001);
    CHECK_NEAR(ramp_at(&s, 100), 1.0f, 0.001);
}

static void test_ramp_setpoints(void) {
    SUITE("heat ramp: each setpoint lands on its own share of the ramp");

    Settings s;
    settings_defaults(&s);
    s.heat_setpoint[0] = 0;
    s.heat_setpoint[1] = 15;
    s.heat_setpoint[2] = 30;
    s.heat_setpoint[3] = 45;
    s.heat_setpoint[4] = 55;
    s.heat_setpoint[5] = 60;

    // Setpoint i sits at i/(N-1) of the way along the ramp, whatever heat was
    // put there.
    for (int i = 0; i < HEAT_SETPOINT_COUNT; i++)
        CHECK_NEAR(ramp_at(&s, s.heat_setpoint[i]),
                   (float)i / (float)(HEAT_SETPOINT_COUNT - 1), 0.001);

    // The case this was built for: 60 % of the maximum and above is the
    // hottest colour, and everything past it stays there.
    CHECK_NEAR(ramp_at(&s, 60), 1.0f, 0.001);
    CHECK_NEAR(ramp_at(&s, 80), 1.0f, 0.001);
    CHECK_NEAR(ramp_at(&s, 100), 1.0f, 0.001);
    CHECK_NEAR(ramp_at(&s, 55), 0.8f, 0.001);

    // Halfway between two setpoints is halfway between their positions -- the
    // colours between the knots are still interpolated, not banded.
    CHECK_NEAR(ramp_at(&s, 22), 0.5f * (0.2f + 0.4f), 0.02);
    CHECK_NEAR(ramp_at(&s, 37), 0.5f * (0.4f + 0.6f), 0.02);

    // The other case: only 90 % and above is the hottest colour.
    settings_defaults(&s);
    s.heat_setpoint[5] = 90;
    settings_repair_setpoints(&s);
    CHECK_NEAR(ramp_at(&s, 90), 1.0f, 0.001);
    CHECK_NEAR(ramp_at(&s, 95), 1.0f, 0.001);
    CHECK(ramp_at(&s, 85) < 1.0f);
}

static void test_ramp_edges(void) {
    SUITE("heat ramp: the ends and the degenerate collection");

    Settings s;
    settings_defaults(&s);

    // A collection with no overlap anywhere gives every point the same heat.
    // Measured as a share of the maximum that is 100 %, which would paint a
    // lone track in the hottest colour there is.
    CHECK_NEAR(heat_normalized(&s, 1, 1), 0.0f, 0.001);
    CHECK_NEAR(heat_normalized(&s, 0, 0), 0.0f, 0.001);
    CHECK_NEAR(heat_normalized(&s, 5, -1), 0.0f, 0.001);

    // Below the first setpoint and above the last, whatever they are set to.
    s.heat_setpoint[0] = 20;
    settings_repair_setpoints(&s);
    CHECK_NEAR(ramp_at(&s, 0), 0.0f, 0.001);
    CHECK_NEAR(ramp_at(&s, 10), 0.0f, 0.001);
    CHECK_NEAR(ramp_at(&s, 20), 0.0f, 0.001);
    CHECK(ramp_at(&s, 30) > 0.0f);

    // The result is a position along a ramp, so it may never leave 0..1
    // however the curve is shaped -- the caller indexes an array with it.
    const int curves[][HEAT_SETPOINT_COUNT] = {
        {0, 1, 2, 3, 4, 5},
        {95, 96, 97, 98, 99, 100},
        {0, 20, 40, 60, 80, 100},
        {0, 50, 51, 52, 53, 100},
    };

    for (size_t c = 0; c < sizeof(curves) / sizeof(curves[0]); c++) {
        settings_defaults(&s);
        memcpy(s.heat_setpoint, curves[c], sizeof(s.heat_setpoint));
        settings_repair_setpoints(&s);

        for (int percent = 0; percent <= 100; percent++) {
            float position = ramp_at(&s, percent);
            CHECK(position >= 0.0f && position <= 1.0f);
        }
    }
}

static void test_ramp_is_monotonic(void) {
    SUITE("heat ramp: more heat is never a colder colour");

    Settings s;
    settings_defaults(&s);
    s.heat_setpoint[0] = 5;
    s.heat_setpoint[1] = 12;
    s.heat_setpoint[2] = 33;
    s.heat_setpoint[3] = 34;
    s.heat_setpoint[4] = 71;
    s.heat_setpoint[5] = 88;
    settings_repair_setpoints(&s);

    float previous = -1.0f;
    for (int heat = 0; heat <= 1000; heat++) {
        float position = heat_normalized(&s, heat, 1000);
        CHECK(position >= previous);
        previous = position;
    }
}

void run_settings_tests(void) {
    test_defaults();
    test_round_trip();
    test_bool_values();
    test_missing_file();
    test_partial_and_unknown_keys();
    test_unparseable_values();
    test_clamping();
    test_setpoint_list_parsing();
    test_setpoint_repair();
    test_setpoint_repair_around_an_edit();
    test_providers();
    test_ramp_defaults();
    test_ramp_setpoints();
    test_ramp_edges();
    test_ramp_is_monotonic();
}
