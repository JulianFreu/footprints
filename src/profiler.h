#ifndef PROFILER_H
#define PROFILER_H

#include <stdbool.h>

#include "config.h"

// Where a frame's time went: sampled by the frame loop, reduced for the overlay
// that draws it.
//
// Nothing here knows about SDL or the renderer -- the overlay is
// src/ui_profiler.c. That split is the same one stats.c and ui_stats.c keep,
// and it is what puts the ring arithmetic, which is where the corners are,
// under the test suite.

// The loop's phases, in the order they run. Each mark closes the phase before
// it, so everything between two marks is attributed to something and a frame's
// phases add up to the frame. A phase never marked is exactly zero, which is
// what the render phases of a frame the loop declined to draw look like.
typedef enum ProfilerPhase {
    PROF_EVENTS = 0, // draining the SDL event queue and dispatching it
    PROF_ADOPT,      // the frame's delta, and adopting what a worker finished
    PROF_UPDATE,     // app_update: the map's animations and the UI's
    PROF_MAP,        // clearing, choosing the visible tiles, blitting them
    PROF_TRACKS,     // the sidebar's graphs, the selected track's overlay
    PROF_HEAT,       // rasterising heat tiles, which is budgeted per frame
    PROF_UI,         // clay_draw_ui: the layout pass and Clay's renderer
    PROF_PRESENT,    // SDL_RenderPresent -- with vsync on, mostly the wait
    PROF_CAP,        // SDL_Delay under the frame cap -- idle by construction
    PROF_PHASE_COUNT
} ProfilerPhase;

typedef struct ProfilerFrame {
    float phase_seconds[PROF_PHASE_COUNT];
    // From the frame's first mark to the end of the frame. The phases above sum
    // to this, less the sliver between one frame ending and the next one's
    // first mark.
    float total_seconds;
    // Whether the render block ran, inferred from PROF_PRESENT having been
    // marked. Nothing has to pass it in.
    bool drawn;
} ProfilerFrame;

typedef struct Profiler {
    // Completed frames, newest last. Only these are ever read: the frame in
    // flight has no present and no delay yet, so drawing it would draw a column
    // short by however long the display makes the loop wait.
    ProfilerFrame frames[PROFILER_HISTORY_FRAMES];
    int count; // saturates at PROFILER_HISTORY_FRAMES
    int next;  // where the next completed frame goes

    // The frame being measured. Deliberately unreachable through the interface
    // below, so a partial column cannot be drawn even by mistake.
    ProfilerFrame current;
    double frame_start; // when its first mark landed, in monotonic seconds
    double phase_start; // when the open phase began
    ProfilerPhase open_phase;
    bool phase_open;
    unsigned marked; // a bit per phase marked this frame

    // Retained across frames so the loop's stamps can accumulate into it.
    // A zeroed Profiler is a valid empty one -- there is no profiler_init, the
    // same contract Anim keeps.
} Profiler;

// Closes whichever phase was open and opens `phase`. The first mark of a frame
// also starts the frame's clock. An out-of-range phase is ignored.
//
// The loop marks rather than wrapping each phase in a scope: the phases are
// strictly sequential and neither nest nor leave gaps, so a stamp at each
// boundary is one line and cannot leave a phase unclosed. Marking a phase twice
// in one frame adds to it rather than replacing it, which is what lets the loop
// return to a phase it has already been in.
void profiler_mark(Profiler *p, ProfilerPhase phase);

// Closes the open phase, files the frame, and readies the next one. Called at
// the very bottom of the loop, after the frame cap, so the newest completed
// frame includes its present and its delay.
void profiler_end_frame(Profiler *p);

// The same two against a caller-supplied clock, in seconds. This is what makes
// the ring and the reductions testable without a real one; the two above are
// wrappers that read CLOCK_MONOTONIC.
void profiler_mark_at(Profiler *p, ProfilerPhase phase, double now);
void profiler_end_frame_at(Profiler *p, double now);

// The completed frame `age` frames back; age 0 is the newest. NULL past what
// has been recorded, and for a negative age -- the overlay walks right to left
// and stops when this says to.
const ProfilerFrame *profiler_frame(const Profiler *p, int age);
int profiler_frame_count(const Profiler *p);

// Mean seconds per phase over the newest `frames` completed frames, clamped to
// what is actually there. Frames the loop declined to draw count, contributing
// zeros: the number answers what a frame costs, not what a drawn frame costs.
// All zeros when nothing has been recorded.
void profiler_average(const Profiler *p, int frames, float out[PROF_PHASE_COUNT]);

// The longest total over the newest `frames`, and how many of them drew. The
// first is the readout's worst case; the second is the loop's duty cycle, which
// is the number that says whether drawing on demand is doing its job.
float profiler_peak_total(const Profiler *p, int frames);
int profiler_drawn_count(const Profiler *p, int frames);

// What the legend writes against each phase. Distinct and never empty.
const char *profiler_phase_name(ProfilerPhase phase);

// Whether the phase is the loop waiting rather than the loop working. True for
// PROF_PRESENT and PROF_CAP only. The overlay draws these last and in grey, so
// the top of the coloured stack is one edge readable across the whole graph.
bool profiler_phase_is_idle(ProfilerPhase phase);

#endif
