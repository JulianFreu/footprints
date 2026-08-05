#include "track_series.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// The pace at one point, in seconds per kilometre.
//
// Measured across a window of PACE_WINDOW_METERS centred on the point rather
// than between it and the one before it, and widened outwards until it spans
// that much ground -- so the answer is the same whether the watch recorded
// every second or every ten. The window is clamped at both ends of the track,
// which is why the first and last points read the pace of the stretch beside
// them rather than nothing at all.
static float pace_at(const GpxTrack *track, int index) {
    const GpxPoint *points = track->points;
    int lo = index;
    int hi = index;

    while (points[hi].partial_distance - points[lo].partial_distance < PACE_WINDOW_METERS) {
        bool widened = false;
        if (lo > 0) {
            lo--;
            widened = true;
        }
        if (hi < track->total_points - 1) {
            hi++;
            widened = true;
        }
        // A track shorter than the window is measured end to end.
        if (!widened)
            break;
    }

    float metres = points[hi].partial_distance - points[lo].partial_distance;
    float seconds = points[hi].elapsed_secs - points[lo].elapsed_secs;

    // Written as a negated comparison so that a NAN from an untimed point
    // takes this branch rather than falling through it.
    if (!(metres > 0.0f) || !(seconds > 0.0f))
        return NAN;

    // The same ceiling the record search uses, and for the same reason: a file
    // labelled "Running" whose second half is the drive home would otherwise
    // draw a pace of forty seconds per kilometre across the graph and flatten
    // the run into the bottom pixel.
    if (metres / seconds > SPLIT_MAX_SPEED_MPS)
        return NAN;

    return seconds / (metres / 1000.0f);
}

static float sample(const GpxTrack *track, TrackSeriesKind kind, int index) {
    switch (kind) {
    case TRACK_SERIES_ELEVATION:
        // Already smoothed by the parser, which replaces the raw series in
        // place before the climb is accumulated from it.
        return track->points[index].elevation;
    case TRACK_SERIES_PACE:
        return pace_at(track, index);
    case TRACK_SERIES_HEART_RATE:
        return track->points[index].heart_rate == 0
                   ? NAN
                   : (float)track->points[index].heart_rate;
    case TRACK_SERIES_COUNT:
        break;
    }
    return NAN;
}

// The narrowest range a graph is ever drawn over, per kind. A minute per
// kilometre, twenty beats and twenty metres are each about the smallest
// difference worth a whole graph; below that the curve is drawn small, which is
// the honest picture of a run that held its pace.
static float minimum_span(TrackSeriesKind kind) {
    switch (kind) {
    case TRACK_SERIES_ELEVATION:
        return 20.0f;
    case TRACK_SERIES_PACE:
        return 60.0f;
    case TRACK_SERIES_HEART_RATE:
        return 20.0f;
    case TRACK_SERIES_COUNT:
        break;
    }
    return 1.0f;
}

void track_series_range(const TrackSeries *series, float *min, float *max) {
    *min = series->min;
    *max = series->max;

    float span = minimum_span(series->kind);
    if (*max - *min < span) {
        // Widened about the middle, so a flat series sits across the centre of
        // the box rather than along one of its edges.
        float centre = (*min + *max) / 2.0f;
        *min = centre - span / 2.0f;
        *max = centre + span / 2.0f;
    }

    float headroom = (*max - *min) / 10.0f;
    *min -= headroom;
    *max += headroom;
}

// The steps a graph's rules may be drawn at, per kind, finest first, and which
// of them the kind is drawn at by choice: fifty metres of climb, thirty seconds
// per kilometre, twenty beats. The entries above the chosen one are for a track
// that covered enough ground to make it too dense to read; the ones below are
// for a run that held steady, where the minimum span above leaves a range
// narrower than a single step.
static const float *grid_steps(TrackSeriesKind kind, int *count, int *preferred) {
    static const float elevation[] = {10.0f, 20.0f, 50.0f, 100.0f,
                                      200.0f, 500.0f, 1000.0f, 2000.0f};
    static const float pace[] = {10.0f, 15.0f, 30.0f, 60.0f, 120.0f, 300.0f, 600.0f};
    static const float heart_rate[] = {5.0f, 10.0f, 20.0f, 50.0f, 100.0f};

    switch (kind) {
    case TRACK_SERIES_PACE:
        *count = (int)(sizeof(pace) / sizeof(*pace));
        *preferred = 2; // 30 seconds per kilometre
        return pace;
    case TRACK_SERIES_HEART_RATE:
        *count = (int)(sizeof(heart_rate) / sizeof(*heart_rate));
        *preferred = 2; // 20 beats
        return heart_rate;
    case TRACK_SERIES_ELEVATION:
    case TRACK_SERIES_COUNT:
        break;
    }
    *count = (int)(sizeof(elevation) / sizeof(*elevation));
    *preferred = 2; // 50 metres
    return elevation;
}

int track_series_grid_lines(TrackSeriesKind kind, float min, float max, int height,
                            float *out, int max_lines) {
    if (!out || max_lines <= 0 || height <= 0 || !(max > min))
        return 0;

    int step_count, chosen;
    const float *steps = grid_steps(kind, &step_count, &chosen);
    float span = max - min;

    // Coarsen while the rules would land too close together to be told apart.
    // The top of the ladder is the fallback rather than the error case: a range
    // wider than it reaches is better drawn with a few rules than with none.
    while (chosen < step_count - 1 &&
           (float)height * steps[chosen] / span < (float)TRACK_GRAPH_GRID_MIN_SPACING)
        chosen++;

    // And refine, but only far enough to put a couple of rules on the graph,
    // and never past the spacing the loop above just satisfied. A tall window
    // is room to draw the chosen step further apart, not a reason to pick a
    // finer one.
    while (chosen > 0 && span / steps[chosen] < (float)TRACK_GRAPH_GRID_MIN_RULES &&
           (float)height * steps[chosen - 1] / span >= (float)TRACK_GRAPH_GRID_MIN_SPACING)
        chosen--;

    float step = steps[chosen];

    // Counted off in whole multiples rather than accumulated, so the hundredth
    // rule is at exactly a hundred steps and not at whatever adding the step to
    // itself a hundred times comes to.
    //
    // Strictly inside the range: track_series_range leaves headroom at both
    // ends, and a rule on the edge would be a line along the graph's border
    // rather than something to measure the curve against.
    int count = 0;
    for (long i = (long)floorf(min / step) + 1; count < max_lines; i++) {
        float value = (float)i * step;
        if (value >= max)
            break;
        out[count++] = value;
    }

    return count;
}

bool track_series_build(const GpxTrack *track, TrackSeriesKind kind, TrackSeries *out) {
    *out = (TrackSeries){.kind = kind, .invert = kind == TRACK_SERIES_PACE};

    // Two points is what it takes to have covered any ground, and a series
    // over no distance has nowhere to be drawn.
    if (!track || track->total_points < 2)
        return false;

    out->values = malloc((size_t)track->total_points * sizeof(float));
    if (!out->values)
        return false;
    out->count = track->total_points;

    for (int i = 0; i < out->count; i++) {
        float value = sample(track, kind, i);
        out->values[i] = value;

        if (isnan(value))
            continue;

        if (!out->present) {
            out->min = value;
            out->max = value;
            out->present = true;
        } else if (value < out->min) {
            out->min = value;
        } else if (value > out->max) {
            out->max = value;
        }
    }

    return out->present;
}

void track_series_free(TrackSeries *series) {
    free(series->values);
    *series = (TrackSeries){0};
}

int track_series_index_at_fraction(const GpxTrack *track, float fraction) {
    if (!track || track->total_points < 1)
        return -1;

    if (fraction < 0.0f)
        fraction = 0.0f;
    if (fraction > 1.0f)
        fraction = 1.0f;

    float total = track->points[track->total_points - 1].partial_distance;
    // A track that never moved has every point at the same place, so the first
    // of them is as good an answer as any.
    if (!(total > 0.0f))
        return 0;

    float target = fraction * total;

    // partial_distance only ever grows, so the point is found by halving
    // rather than by walking a track that may hold tens of thousands of them.
    int low = 0;
    int high = track->total_points - 1;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (track->points[mid].partial_distance < target)
            low = mid + 1;
        else
            high = mid;
    }

    // The search lands on the first point at or past the target; the one
    // before it may well be the nearer of the two.
    if (low > 0) {
        float here = track->points[low].partial_distance - target;
        float before = target - track->points[low - 1].partial_distance;
        if (before < here)
            low--;
    }
    return low;
}

const char *track_series_label(TrackSeriesKind kind) {
    switch (kind) {
    case TRACK_SERIES_ELEVATION:
        return "Elevation";
    case TRACK_SERIES_PACE:
        return "Pace";
    case TRACK_SERIES_HEART_RATE:
        return "Heart rate";
    case TRACK_SERIES_COUNT:
        break;
    }
    return "";
}

const char *track_series_unit(TrackSeriesKind kind) {
    switch (kind) {
    case TRACK_SERIES_ELEVATION:
        return "m";
    case TRACK_SERIES_PACE:
        return "min/km";
    case TRACK_SERIES_HEART_RATE:
        return "bpm";
    case TRACK_SERIES_COUNT:
        break;
    }
    return "";
}

const char *track_series_format(TrackSeriesKind kind, float value, char *out, size_t size) {
    if (size == 0)
        return out;

    if (isnan(value)) {
        snprintf(out, size, "--");
        return out;
    }

    if (kind == TRACK_SERIES_PACE) {
        // The same mm:ss the track's average pace is written in.
        int seconds = (int)value;
        snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
        return out;
    }

    snprintf(out, size, "%d", (int)value);
    return out;
}
