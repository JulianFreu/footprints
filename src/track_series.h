#ifndef TRACK_SERIES_H
#define TRACK_SERIES_H

#include <stdbool.h>
#include <stddef.h>

#include "gpx_types.h"

// The numbers behind one graph in the sidebar: a value per track point, drawn
// against the distance covered.
//
// Only the arithmetic lives here. What the series looks like is tracks.c's
// business, and where it is drawn is ui.c's -- the same split stats.c and
// ui_stats.c are in, and for the same reason: this side is testable without
// SDL.

typedef enum TrackSeriesKind {
    TRACK_SERIES_ELEVATION,
    TRACK_SERIES_PACE,
    TRACK_SERIES_HEART_RATE,
    TRACK_SERIES_COUNT
} TrackSeriesKind;

typedef struct TrackSeries {
    TrackSeriesKind kind;
    float *values; // one per track point; NAN where the sample is missing
    int count;
    float min; // over the samples that are present
    float max;
    // False when the track carries none of this kind, which is how a run
    // recorded without a heart rate monitor ends up with two graphs instead of
    // three.
    bool present;
    // Pace reads better with the quickest kilometre at the top, which is the
    // opposite of every other series here.
    bool invert;
} TrackSeries;

// Builds one series over `track`. Returns whether there is anything to draw;
// `out` is left safe to free either way.
bool track_series_build(const GpxTrack *track, TrackSeriesKind kind, TrackSeries *out);
void track_series_free(TrackSeries *series);

// The range a graph is drawn over. Not simply the samples' own: a steady run
// spans a fraction of a second per kilometre, and stretching that to the height
// of the box would draw the noise on a flat line as a mountain range. Each kind
// has a floor under the range it is shown at, so a run held at one pace reads as
// one pace. Headroom is added at both ends on top of that, so the curve does not
// run along the edge.
void track_series_range(const TrackSeries *series, float *min, float *max);

// The round values a graph's horizontal rules are drawn at, ascending, written
// into `out` and returning how many there are. `min` and `max` are the range
// track_series_range gave, and `height` the pixels the graph is drawn in.
//
// Which values are round is the kind's business: 50 metres of climb, thirty
// seconds per kilometre, twenty beats. The step coarsens from there -- to 100m,
// 200m and up -- until the rules are far enough apart to be read at the height
// they are drawn at, so a run over a mountain does not come out hatched. It
// goes the other way only for a run held steady enough that the chosen step
// would draw almost nothing; a tall graph is room to space the chosen step out,
// not a reason to pick a finer one.
int track_series_grid_lines(TrackSeriesKind kind, float min, float max, int height,
                            float *out, int max_lines);

// Enough for any graph a window can be tall enough to show.
#define TRACK_SERIES_GRID_MAX 32

// The point nearest `fraction` of the way along the track's distance, clamped
// to the track's ends. This is what turns a position in a graph into a
// position on the map.
int track_series_index_at_fraction(const GpxTrack *track, float fraction);

// What the graph is called, and the unit its values are in.
const char *track_series_label(TrackSeriesKind kind);
const char *track_series_unit(TrackSeriesKind kind);

// The display form of one sample -- whole metres, a pace as mm:ss, whole bpm --
// written into `out` and returned, so the call reads inline at the point of
// use. A NAN sample gives "--", the same way a missing one is drawn as a gap.
const char *track_series_format(TrackSeriesKind kind, float value, char *out, size_t size);

// Enough for any of the above.
#define TRACK_SERIES_TEXT_MAX 16

#endif
