#include "track_format.h"

#include <stdio.h>

#include "time_util.h"

const char *track_format(const GpxTrack *track, TrackText field, char *out, size_t size) {
    if (size == 0)
        return out;
    out[0] = '\0';

    switch (field) {
    case TRACK_TEXT_DATE:
    case TRACK_TEXT_TIME: {
        char date[16], time_of_day[16];
        if (!utc_to_display_strings(track->start_utc, date, sizeof(date),
                                    time_of_day, sizeof(time_of_day)))
            return out; // no usable timestamp; the cell stays blank
        snprintf(out, size, "%s", field == TRACK_TEXT_DATE ? date : time_of_day);
        return out;
    }

    case TRACK_TEXT_DURATION: {
        int seconds = (int)track->duration_secs;
        snprintf(out, size, "%02d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60,
                 seconds % 60);
        return out;
    }

    case TRACK_TEXT_DISTANCE:
        snprintf(out, size, "%.2f", track->distance);
        return out;

    case TRACK_TEXT_PACE: {
        int seconds = (int)track->secs_per_km;
        snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
        return out;
    }

    // Elevations are shown as whole metres; the filter comparison allows for
    // that rounding rather than the two disagreeing at the boundary.
    case TRACK_TEXT_ELEV_UP:
        snprintf(out, size, "%d", (int)track->elev_up);
        return out;
    case TRACK_TEXT_ELEV_DOWN:
        snprintf(out, size, "%d", (int)track->elev_down);
        return out;
    case TRACK_TEXT_HIGH_POINT:
        snprintf(out, size, "%d", (int)track->high_point);
        return out;
    case TRACK_TEXT_LOW_POINT:
        snprintf(out, size, "%d", (int)track->low_point);
        return out;
    }
    return out;
}
