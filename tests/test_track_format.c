#include "../src/track_format.c"

#include "harness.h"

// The display strings used to be produced by the parser and stored on the
// track, where nothing could reach them without loading a file. Now that they
// are made on demand, the formatting is directly testable.

static const char *fmt(const GpxTrack *track, TrackText field) {
    static char buffer[TRACK_TEXT_MAX];
    return track_format(track, field, buffer, sizeof(buffer));
}

void run_track_format_tests(void) {
    GpxTrack track = {0};
    track.start_utc = iso8601_to_utc("2025-08-24T15:02:15Z");
    track.duration_secs = 5445.0f; // 01:30:45
    track.distance = 12.3456f;
    track.secs_per_km = 330.0f; // 5:30
    track.elev_up = 421.7f;
    track.elev_down = 405.2f;
    track.high_point = 1834.9f;
    track.low_point = 512.4f;

    SUITE("track_format: each attribute renders as the UI expects");
    CHECK_STR(fmt(&track, TRACK_TEXT_DATE), "24.08.2025");
    CHECK_STR(fmt(&track, TRACK_TEXT_TIME), "15:02");
    CHECK_STR(fmt(&track, TRACK_TEXT_DURATION), "01:30:45");
    CHECK_STR(fmt(&track, TRACK_TEXT_DISTANCE), "12.35");
    CHECK_STR(fmt(&track, TRACK_TEXT_PACE), "5:30");
    // Elevations truncate to whole metres, which is what the filter's
    // comparison epsilon exists to tolerate.
    CHECK_STR(fmt(&track, TRACK_TEXT_ELEV_UP), "421");
    CHECK_STR(fmt(&track, TRACK_TEXT_ELEV_DOWN), "405");
    CHECK_STR(fmt(&track, TRACK_TEXT_HIGH_POINT), "1834");
    CHECK_STR(fmt(&track, TRACK_TEXT_LOW_POINT), "512");

    SUITE("track_format: durations and paces pad their fields");
    track.duration_secs = 5.0f;
    CHECK_STR(fmt(&track, TRACK_TEXT_DURATION), "00:00:05");
    track.duration_secs = 3600.0f;
    CHECK_STR(fmt(&track, TRACK_TEXT_DURATION), "01:00:00");
    // Over a day, hours keep counting rather than wrapping.
    track.duration_secs = 90000.0f;
    CHECK_STR(fmt(&track, TRACK_TEXT_DURATION), "25:00:00");

    track.secs_per_km = 305.0f;
    CHECK_STR(fmt(&track, TRACK_TEXT_PACE), "5:05");
    track.secs_per_km = 59.0f;
    CHECK_STR(fmt(&track, TRACK_TEXT_PACE), "0:59");

    SUITE("track_format: a track with no timestamp renders blank, not garbage");
    GpxTrack undated = {0};
    undated.start_utc = (time_t)-1;
    CHECK_STR(fmt(&undated, TRACK_TEXT_DATE), "");
    CHECK_STR(fmt(&undated, TRACK_TEXT_TIME), "");
    // The numeric fields of a zeroed track still render.
    CHECK_STR(fmt(&undated, TRACK_TEXT_DURATION), "00:00:00");
    CHECK_STR(fmt(&undated, TRACK_TEXT_DISTANCE), "0.00");

    SUITE("track_format: never writes past the buffer it is given");
    // The UI hands out fixed TRACK_TEXT_MAX slices from a frame arena, so a
    // formatter that overran would corrupt the next string rather than crash.
    for (int field = TRACK_TEXT_DATE; field <= TRACK_TEXT_LOW_POINT; field++) {
        char small[8];
        memset(small, 0x7f, sizeof(small));
        track_format(&track, (TrackText)field, small, 4);
        CHECK(small[4] == 0x7f);
        // Whatever fits is still null-terminated.
        CHECK(memchr(small, '\0', 4) != NULL);
    }

    SUITE("track_format: a zero-sized buffer is left alone");
    char untouched[4];
    memset(untouched, 0x5a, sizeof(untouched));
    track_format(&track, TRACK_TEXT_DATE, untouched, 0);
    CHECK(untouched[0] == 0x5a);
}
