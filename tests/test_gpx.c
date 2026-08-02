#include "../src/gpx_parser.c"

#include "harness.h"

#include <stdlib.h>

// Runs gpx_extract_coords over a document held in memory, so the walk that
// fills the points and their timestamps is testable without a file on disk.
static void parse_string(const char *xml, GpxTrack *track, TrackTimes *times) {
    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "test.gpx", NULL, 0);
    CHECK(doc != NULL);
    if (!doc)
        return;
    times->count = 0;
    gpx_extract_coords(xmlDocGetRootElement(doc), track, times);
    xmlFreeDoc(doc);
}

// Builds a track over a fixed set of elevations. Coordinates advance in a
// straight line so partial_distance is monotonic; only elevation matters to the
// suites below that use it.
static GpxTrack track_with_elevations(const float *elevations, int count) {
    GpxTrack track = {0};
    track.points = calloc((size_t)count, sizeof(GpxPoint));
    track.total_points = count;
    track.points_capacity = count;
    for (int i = 0; i < count; i++) {
        track.points[i].lat = 48.0 + i * 0.0001;
        track.points[i].lon = 11.0;
        track.points[i].elevation = elevations[i];
    }
    return track;
}

static void free_track(GpxTrack *track) {
    free(track->points);
    track->points = NULL;
}

void run_gpx_tests(void) {
    SUITE("gpx: haversine_distance");
    CHECK_NEAR(haversine_distance(48.0, 11.0, 48.0, 11.0), 0.0, 1e-9);
    // One degree of latitude is the same length anywhere.
    CHECK_NEAR(haversine_distance(0.0, 0.0, 1.0, 0.0), 111194.93, 0.5);
    CHECK_NEAR(haversine_distance(48.0, 11.0, 48.001, 11.0), 111.19, 0.05);
    // A long diagonal, to catch a sign or radian/degree slip that a short one
    // would hide.
    CHECK_NEAR(haversine_distance(48.1372, 11.5755, 52.5200, 13.4050),
               504307.23, 5.0);
    // Symmetric in its arguments.
    CHECK_NEAR(haversine_distance(48.1372, 11.5755, 52.5200, 13.4050),
               haversine_distance(52.5200, 13.4050, 48.1372, 11.5755), 1e-6);

    SUITE("gpx: lat_lon_to_pixel");
    int x, y;
    // Null island sits at the centre of the single zoom-0 tile.
    lat_lon_to_pixel(0.0, 0.0, 0, &x, &y);
    CHECK_INT(x, 128);
    CHECK_INT(y, 128);
    // Munich, against the standard Web Mercator tile numbers.
    lat_lon_to_pixel(48.1372, 11.5755, 12, &x, &y);
    CHECK_INT(x / TILE_SIZE, 2179);
    CHECK_INT(y / TILE_SIZE, 1421);
    // At MAX_ZOOM the world no longer fits in a float's mantissa; these exact
    // values are what pins the projection to double precision.
    lat_lon_to_pixel(48.1372, 11.5755, MAX_ZOOM, &x, &y);
    CHECK_INT(x, 142849046);
    CHECK_INT(y, 93158939);
    // The poles are clamped rather than sent to infinity through the log.
    lat_lon_to_pixel(90.0, 0.0, 4, &x, &y);
    CHECK(y >= 0);
    lat_lon_to_pixel(-90.0, 0.0, 4, &x, &y);
    CHECK(y <= (TILE_SIZE << 4));

    SUITE("gpx: track_calculate_distance");
    float flat[4] = {0, 0, 0, 0};
    GpxTrack track = track_with_elevations(flat, 4);
    track_calculate_distance(&track);
    // Three hops of 0.0001 degrees of latitude, reported in kilometres.
    CHECK_NEAR(track.distance, 3 * 0.01111949, 1e-5);
    CHECK_NEAR(track.points[0].partial_distance, 0.0, 1e-9);
    // partial_distance is in metres and must be monotonic -- the elevation
    // profile divides by its last value.
    CHECK(track.points[3].partial_distance > track.points[1].partial_distance);
    free_track(&track);

    SUITE("gpx: track_calculate_distance on a degenerate track");
    float one[1] = {0};
    track = track_with_elevations(one, 1);
    track_calculate_distance(&track);
    CHECK_NEAR(track.distance, 0.0, 1e-9);
    free_track(&track);

    SUITE("gpx: elevation smoothing is symmetric");
    // The property the old in-place filter broke. Smoothing a series and
    // smoothing its reverse must mirror: what is climb one way is descent the
    // other. Writing each average back into the window of the points after it
    // made the filter one-sided, so the two disagreed.
    enum {
        N = 200
    };
    float rising[N], falling[N];
    for (int i = 0; i < N; i++) {
        // A hill with a little noise on it, so the smoothing has work to do.
        float value = 100.0f + 50.0f * (float)i / N + ((i % 3) - 1) * 2.0f;
        rising[i] = value;
        falling[N - 1 - i] = value;
    }

    GpxTrack up = track_with_elevations(rising, N);
    GpxTrack down = track_with_elevations(falling, N);
    track_calculate_elevation_gain_loss(&up);
    track_calculate_elevation_gain_loss(&down);

    CHECK_NEAR(up.elev_up, down.elev_down, 1e-3);
    CHECK_NEAR(up.elev_down, down.elev_up, 1e-3);
    CHECK_NEAR(up.high_point, down.high_point, 1e-3);
    CHECK_NEAR(up.low_point, down.low_point, 1e-3);
    free_track(&up);
    free_track(&down);

    SUITE("gpx: elevation smoothing removes noise, not the hill");
    // A clean 100 m climb with +/-2 m of jitter. The raw series accumulates the
    // jitter into hundreds of metres of phantom gain; the smoothed one should
    // land near the real climb and report no meaningful descent.
    float noisy[N];
    for (int i = 0; i < N; i++)
        noisy[i] = 100.0f + 100.0f * (float)i / (N - 1) + ((i % 2) ? 2.0f : -2.0f);

    GpxTrack hill = track_with_elevations(noisy, N);
    track_calculate_elevation_gain_loss(&hill);
    CHECK_NEAR(hill.elev_up, 100.0, 10.0);
    CHECK(hill.elev_down < 5.0f);
    CHECK_NEAR(hill.high_point, 200.0, 10.0);
    CHECK_NEAR(hill.low_point, 100.0, 10.0);
    free_track(&hill);

    SUITE("gpx: elevation smoothing handles short tracks");
    // Fewer points than the smoothing window; the window clamps at both ends
    // rather than reading off the array or bailing out and leaving the
    // elevation fields untouched.
    float few[4] = {500.0f, 520.0f, 505.0f, 530.0f};
    GpxTrack shorty = track_with_elevations(few, 4);
    track_calculate_elevation_gain_loss(&shorty);
    // Every sample averages the whole track, so the series is flat and there is
    // no gain to report -- but high_point and low_point must still be set to
    // that average rather than left at zero.
    CHECK(shorty.high_point > 500.0f);
    CHECK(shorty.low_point > 500.0f);
    CHECK_NEAR(shorty.elev_up, 0.0, 1e-3);
    CHECK_NEAR(shorty.elev_down, 0.0, 1e-3);
    free_track(&shorty);

    SUITE("gpx: track_calculate_mid_point");
    float four[4] = {0, 0, 0, 0};
    track = track_with_elevations(four, 4);
    for (int i = 0; i < 4; i++) {
        track.points[i].world_x = 100 + i * 100; // 100,200,300,400
        track.points[i].world_y = 1000;
    }
    track_calculate_mid_point(&track);
    CHECK_INT(track.mid_x, 250);
    CHECK_INT(track.mid_y, 1000);
    free_track(&track);

    SUITE("gpx: activity_type_from_string");
    // The converter writes "Running", other tools write "running", and the
    // comparison used to be case-sensitive -- which quietly filed every
    // lowercase run under Other.
    CHECK_INT(activity_type_from_string((const xmlChar *)"Running"), Run);
    CHECK_INT(activity_type_from_string((const xmlChar *)"running"), Run);
    CHECK_INT(activity_type_from_string((const xmlChar *)"RUNNING"), Run);
    CHECK_INT(activity_type_from_string((const xmlChar *)"Hiking"), Hike);
    CHECK_INT(activity_type_from_string((const xmlChar *)"hiking"), Hike);
    CHECK_INT(activity_type_from_string((const xmlChar *)"Cycling"), Cycling);
    CHECK_INT(activity_type_from_string((const xmlChar *)"cycling"), Cycling);
    // Anything else is Other, including the walks and the paddleboarding that
    // no category asks for.
    CHECK_INT(activity_type_from_string((const xmlChar *)"Walking"), Other);
    CHECK_INT(activity_type_from_string((const xmlChar *)"9"), Other);
    CHECK_INT(activity_type_from_string((const xmlChar *)""), Other);

    SUITE("gpx: a timestamp per point, alongside the elevations");
    // The elevation loop used to break out as soon as it had the <ele>, which
    // would leave every timestamp behind. Both have to come out of one pass.
    {
        GpxTrack recorded = {0};
        TrackTimes times = {0};
        parse_string(
            "<gpx><trk><trkseg>"
            "<trkpt lat='48.0' lon='11.0'><ele>500.0</ele><time>2025-05-01T06:00:00Z</time></trkpt>"
            "<trkpt lat='48.1' lon='11.0'><ele>510.0</ele><time>2025-05-01T06:00:10Z</time></trkpt>"
            "<trkpt lat='48.2' lon='11.0'><ele>520.0</ele><time>2025-05-01T06:00:25Z</time></trkpt>"
            "</trkseg></trk></gpx>",
            &recorded, &times);

        CHECK_INT(recorded.total_points, 3);
        CHECK_INT(times.count, recorded.total_points);
        CHECK_NEAR(recorded.points[0].elevation, 500.0, 1e-6);
        CHECK_NEAR(recorded.points[2].elevation, 520.0, 1e-6);
        CHECK_INT(times.at[0], iso8601_to_utc("2025-05-01T06:00:00Z"));
        CHECK_INT(times.at[2], iso8601_to_utc("2025-05-01T06:00:25Z"));
        // The order of the children must not matter either.
        CHECK(times.at[1] > times.at[0]);

        free(recorded.points);
        free(times.at);
    }

    SUITE("gpx: a planned route carries no times at all");
    // What the AllTrails exports in the library look like: coordinates and
    // elevations, no <time> anywhere. Every slot stays unset, which is what
    // keeps such a track out of the dated statistics and off the records.
    {
        GpxTrack route = {0};
        TrackTimes times = {0};
        parse_string(
            "<gpx><trk><trkseg>"
            "<trkpt lat='50.8' lon='14.7'><ele>400.0</ele></trkpt>"
            "<trkpt lat='50.9' lon='14.7'><ele>401.0</ele></trkpt>"
            "</trkseg></trk></gpx>",
            &route, &times);

        CHECK_INT(route.total_points, 2);
        CHECK_INT(times.count, 2);
        CHECK_INT(times.at[0], (time_t)-1);
        CHECK_INT(times.at[1], (time_t)-1);
        CHECK_NEAR(route.points[1].elevation, 401.0, 1e-6);

        free(route.points);
        free(times.at);
    }

    SUITE("gpx: times only in the middle of the track");
    // The first and last usable timestamps are what become start_utc and
    // end_utc, so a fix that lost its clock at either end must be skipped over
    // rather than taken as the bound.
    {
        GpxTrack patchy = {0};
        TrackTimes times = {0};
        parse_string(
            "<gpx><trk><trkseg>"
            "<trkpt lat='48.0' lon='11.0'><ele>500.0</ele></trkpt>"
            "<trkpt lat='48.1' lon='11.0'><time>2025-05-01T06:00:10Z</time></trkpt>"
            "<trkpt lat='48.2' lon='11.0'><time>2025-05-01T06:00:20Z</time></trkpt>"
            "<trkpt lat='48.3' lon='11.0'><ele>530.0</ele></trkpt>"
            "</trkseg></trk></gpx>",
            &patchy, &times);

        CHECK_INT(times.count, 4);
        CHECK_INT(times.at[0], (time_t)-1);
        CHECK_INT(times.at[3], (time_t)-1);

        time_t start = (time_t)-1, end = (time_t)-1;
        for (int i = 0; i < times.count; i++) {
            if (times.at[i] == (time_t)-1)
                continue;
            if (start == (time_t)-1)
                start = times.at[i];
            end = times.at[i];
        }
        CHECK_INT(start, iso8601_to_utc("2025-05-01T06:00:10Z"));
        CHECK_INT(end, iso8601_to_utc("2025-05-01T06:00:20Z"));

        free(patchy.points);
        free(times.at);
    }

    SUITE("gpx: a point with no coordinates keeps the arrays in step");
    // A malformed <trkpt> still counts as a point, so its timestamp slot has to
    // exist too -- otherwise every point after it would be paired with the
    // wrong time.
    {
        GpxTrack broken = {0};
        TrackTimes times = {0};
        parse_string(
            "<gpx><trk><trkseg>"
            "<trkpt lat='48.0' lon='11.0'><time>2025-05-01T06:00:00Z</time></trkpt>"
            "<trkpt><time>2025-05-01T06:00:10Z</time></trkpt>"
            "<trkpt lat='48.2' lon='11.0'><time>2025-05-01T06:00:20Z</time></trkpt>"
            "</trkseg></trk></gpx>",
            &broken, &times);

        CHECK_INT(broken.total_points, 3);
        CHECK_INT(times.count, 3);
        CHECK_INT(times.at[2], iso8601_to_utc("2025-05-01T06:00:20Z"));

        free(broken.points);
        free(times.at);
    }
}
