#include "profiler.h"

#include <time.h>

// The timing and the ring behind the frame-time overlay. No SDL: the clock is
// the same CLOCK_MONOTONIC the heat calculation already times itself with, and
// keeping it that way is what lets tests/test_profiler.c include this file.

// Names as the legend writes them. Indexed by ProfilerPhase, so the order here
// is the enum's order and not a choice.
static const char *const phase_names[PROF_PHASE_COUNT] = {
    "Events", "Adopt", "Update", "Map tiles", "Tracks",
    "Heat tiles", "Clay UI", "Present", "Frame cap"};

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Shuts the open phase at `now` and adds what it took to the frame. Adding
// rather than assigning is what lets a phase be returned to: the loop draws the
// track overlays either side of the heat tiles, and both belong to PROF_TRACKS.
static void close_open_phase(Profiler *p, double now) {
    if (!p->phase_open)
        return;

    double elapsed = now - p->phase_start;
    // A clock that steps backwards -- a suspend, or a caller passing an earlier
    // reading -- must not push a segment below the baseline it is stacked from.
    if (elapsed < 0.0)
        elapsed = 0.0;

    p->current.phase_seconds[p->open_phase] += (float)elapsed;
    p->phase_open = false;
}

void profiler_mark_at(Profiler *p, ProfilerPhase phase, double now) {
    if (phase < 0 || phase >= PROF_PHASE_COUNT)
        return;

    close_open_phase(p, now);

    // The first mark of a frame is where the frame starts. Nothing before it is
    // measured, which is the sliver between one frame's last mark and this one.
    if (p->marked == 0u)
        p->frame_start = now;

    p->marked |= 1u << phase;
    p->open_phase = phase;
    p->phase_start = now;
    p->phase_open = true;
}

void profiler_end_frame_at(Profiler *p, double now) {
    // A frame with no marks at all is not filed: it would be a column of
    // nothing, and it would push a real one out of the ring to say so.
    if (p->marked == 0u) {
        p->phase_open = false;
        return;
    }

    close_open_phase(p, now);

    double total = now - p->frame_start;
    if (total < 0.0)
        total = 0.0;
    p->current.total_seconds = (float)total;
    p->current.drawn = ((p->marked >> PROF_PRESENT) & 1u) != 0u;

    p->frames[p->next] = p->current;
    p->next = (p->next + 1) % PROFILER_HISTORY_FRAMES;
    if (p->count < PROFILER_HISTORY_FRAMES)
        p->count++;

    p->current = (ProfilerFrame){0};
    p->marked = 0u;
    p->phase_open = false;
    p->frame_start = 0.0;
}

void profiler_mark(Profiler *p, ProfilerPhase phase) {
    profiler_mark_at(p, phase, monotonic_seconds());
}

void profiler_end_frame(Profiler *p) {
    profiler_end_frame_at(p, monotonic_seconds());
}

const ProfilerFrame *profiler_frame(const Profiler *p, int age) {
    if (age < 0 || age >= p->count)
        return NULL;

    // `next` is one past the newest, so age 0 is the slot before it. The
    // addition keeps the dividend positive without depending on how C's
    // remainder treats a negative one.
    int index = (p->next - 1 - age + 2 * PROFILER_HISTORY_FRAMES) % PROFILER_HISTORY_FRAMES;
    return &p->frames[index];
}

int profiler_frame_count(const Profiler *p) {
    return p->count;
}

// How many of the newest `frames` there actually are to read. Asking for more
// than has been recorded is ordinary -- the graph asks for a full window from
// its first frame onwards -- so it is clamped rather than refused.
static int window(const Profiler *p, int frames) {
    if (frames > p->count)
        frames = p->count;
    if (frames < 0)
        frames = 0;
    return frames;
}

void profiler_average(const Profiler *p, int frames, float out[PROF_PHASE_COUNT]) {
    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++)
        out[phase] = 0.0f;

    int counted = window(p, frames);
    if (counted == 0)
        return;

    for (int age = 0; age < counted; age++) {
        const ProfilerFrame *frame = profiler_frame(p, age);
        for (int phase = 0; phase < PROF_PHASE_COUNT; phase++)
            out[phase] += frame->phase_seconds[phase];
    }

    // Divided by what was read rather than by what was asked for, so a
    // half-filled ring does not report half the cost.
    for (int phase = 0; phase < PROF_PHASE_COUNT; phase++)
        out[phase] /= (float)counted;
}

float profiler_peak_total(const Profiler *p, int frames) {
    float peak = 0.0f;
    int counted = window(p, frames);

    for (int age = 0; age < counted; age++) {
        const ProfilerFrame *frame = profiler_frame(p, age);
        if (frame->total_seconds > peak)
            peak = frame->total_seconds;
    }

    return peak;
}

int profiler_drawn_count(const Profiler *p, int frames) {
    int drawn = 0;
    int counted = window(p, frames);

    for (int age = 0; age < counted; age++)
        if (profiler_frame(p, age)->drawn)
            drawn++;

    return drawn;
}

const char *profiler_phase_name(ProfilerPhase phase) {
    if (phase < 0 || phase >= PROF_PHASE_COUNT)
        return "?";
    return phase_names[phase];
}

bool profiler_phase_is_idle(ProfilerPhase phase) {
    return phase == PROF_PRESENT || phase == PROF_CAP;
}
