#include "track_splits.h"

#include <math.h>
#include <stdbool.h>

#include "gpx_types.h"

// The fastest stretch of a track covering a benchmark distance.
//
// Sorting the tracks by pace would answer a different question: it would find
// the fastest 10 km run, not the fastest 10 km run inside a run. A record set
// between the 5 and 15 km marks of a half marathon is still a record, so the
// search here slides a window of fixed distance along the track rather than
// measuring it end to end.
//
// Nothing here knows about SDL, Clay or the parser that calls it, which is what
// puts the window arithmetic -- where the corners are -- under the test suite.
//
// One caveat that belongs on the record: the distances come from a haversine
// sum over raw GPS fixes, so noise inflates them slightly and every split comes
// out a shade fast. That is the same distance the run list and the pace column
// already show, so at least everything is wrong in the same direction.

static const float split_metres[SPLIT_COUNT] = {
    [SPLIT_5K] = 5000.0f,
    [SPLIT_10K] = 10000.0f,
    [SPLIT_HALF_MARATHON] = 21097.5f,
    [SPLIT_MARATHON] = 42195.0f,
};

float track_splits_metres(SplitDistance split) {
    return (split >= 0 && split < SPLIT_COUNT) ? split_metres[split] : 0.0f;
}

// --- The window ---

// Zero is "nothing found yet", which is also what an unset split reads as, so a
// candidate has to be positive before it can beat anything.
static void keep_if_better(double elapsed, double *best) {
    if (elapsed > 0.0 && (*best == 0.0 || elapsed < *best))
        *best = elapsed;
}

// The fastest window of `target` metres inside points [first, last], against
// the best already found elsewhere in the track.
//
// Two passes, and both are needed. Elapsed time as a function of where a window
// of fixed length sits is piecewise linear, with a corner wherever either end
// crosses a sample, and a piecewise-linear function takes its minimum at a
// corner -- so a scan whose windows always end on a sample sees half of them.
static void scan_run(const GpxTrack *track, const time_t *times,
                     int first, int last, float target, double *best) {
    const GpxPoint *p = track->points;

    // Nothing in this stretch is long enough, so neither pass has a window.
    if (p[last].partial_distance - p[first].partial_distance < target)
        return;

    // Pass A: the window ends on a sample, its start is interpolated.
    int i = first;
    for (int j = first + 1; j <= last; j++) {
        // Advance the start while the window would still be long enough
        // without point i + 1 in it. What is left is the pair bracketing the
        // start: dist[i] <= dist[j] - target < dist[i + 1]. i never moves
        // backwards across the whole loop, so the pass is linear.
        while (i + 1 < j && p[j].partial_distance - p[i + 1].partial_distance >= target)
            i++;
        if (p[j].partial_distance - p[i].partial_distance < target)
            continue; // the run has not reached the target distance yet

        float start_at = p[j].partial_distance - target;
        float span = p[i + 1].partial_distance - p[i].partial_distance;
        // Interpolating across the bracketing segment, rather than snapping to
        // whichever sample is nearer, is the point of the exercise: fixes come
        // as much as fifteen seconds apart, and snapping would add all of that
        // to a five-kilometre time.
        //
        // span is positive -- the bracket is strict on the right -- but it is
        // guarded all the same, since partial_distance is a float sum and two
        // fixes taken while standing still can land on the same value. The
        // window then starts at the moment the runner left the mark.
        double fraction = (span > 0.0f)
                              ? (double)(start_at - p[i].partial_distance) / span
                              : 1.0;
        // Differences, not absolutes: a time_t is 1.7e9 and a float carries 24
        // bits of mantissa, so casting a timestamp itself would round it to the
        // nearest couple of minutes.
        keep_if_better(difftime(times[j], times[i]) -
                           fraction * difftime(times[i + 1], times[i]),
                       best);
    }

    // Pass B: the window starts on a sample, its end is interpolated. k is
    // monotonic for the same reason i is -- end_at only grows with s.
    int k = first + 1;
    for (int s = first; s < last; s++) {
        float end_at = p[s].partial_distance + target;
        if (k <= s)
            k = s + 1;
        while (k <= last && p[k].partial_distance < end_at)
            k++;
        if (k > last)
            break; // no later start can reach the target either

        float span = p[k].partial_distance - p[k - 1].partial_distance;
        // The mirror of pass A's guard: standing still on the finish mark ends
        // the window the moment it was first reached.
        double fraction = (span > 0.0f)
                              ? (double)(end_at - p[k - 1].partial_distance) / span
                              : 0.0;
        keep_if_better(difftime(times[k - 1], times[s]) +
                           fraction * difftime(times[k], times[k - 1]),
                       best);
    }
}

// Every distance over one run, carrying in what earlier runs of the same track
// already found so several stretches of one file compete properly.
static void scan_all_distances(GpxTrack *track, const time_t *times,
                               int first, int last) {
    for (int s = 0; s < SPLIT_COUNT; s++) {
        double best = (double)track->splits[s];
        scan_run(track, times, first, last, split_metres[s], &best);
        track->splits[s] = (float)best;
    }
}

// --- Runs of usable samples ---

// Whether point i continues the stretch before it. A clock or an odometer that
// went backwards ends the run here and starts the next one at this point: the
// window arithmetic divides by a distance and subtracts two times, and both
// have to be going one way.
//
// So does a stretch covered faster than anyone runs. That one is not a
// robustness guard but a correctness one: see SPLIT_MAX_SPEED_MPS.
static bool point_joins_run(const GpxTrack *track, const time_t *times,
                            int i, bool first_of_run) {
    if (times[i] == (time_t)-1)
        return false;
    if (!isfinite(track->points[i].partial_distance))
        return false;
    if (first_of_run)
        return true;

    if (times[i] < times[i - 1])
        return false;

    float advanced = track->points[i].partial_distance -
                     track->points[i - 1].partial_distance;
    if (!(advanced >= 0.0f))
        return false;

    // Two fixes sharing a second are how a coarse clock records standing still,
    // and the window arithmetic copes with that. Any actual distance covered in
    // no time at all is not something to believe.
    double seconds = difftime(times[i], times[i - 1]);
    if (seconds <= 0.0)
        return advanced == 0.0f;

    return (double)advanced <= SPLIT_MAX_SPEED_MPS * seconds;
}

void track_splits_compute(GpxTrack *track, const time_t *point_times) {
    if (!track)
        return;

    for (int s = 0; s < SPLIT_COUNT; s++)
        track->splits[s] = 0.0f;

    // Two points is the fewest a window can be measured across, and it also
    // rules out the single-point track whose partial_distance[0]
    // track_calculate_distance never writes.
    if (!track->points || !point_times || track->total_points < 2)
        return;

    // The track is split into maximal stretches of usable samples before any
    // window is measured. A lost GPS fix, a file stitched together from two
    // activities, or the uninitialised GpxPoint a <trkpt> with no lat/lon
    // leaves behind would otherwise hand the window a negative denominator.
    int total_points = track->total_points;
    int begin = -1;
    for (int i = 0; i <= total_points; i++) {
        if (i < total_points && point_joins_run(track, point_times, i, begin < 0)) {
            if (begin < 0)
                begin = i;
            continue;
        }

        if (begin >= 0 && i - begin >= 2)
            scan_all_distances(track, point_times, begin, i - 1);

        // The point that ended the run starts the next one -- unless what
        // ended it was having nothing usable of its own to start one with.
        begin = (i < total_points && point_joins_run(track, point_times, i, true))
                    ? i
                    : -1;
    }
}
