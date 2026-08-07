#ifndef HARNESS_H
#define HARNESS_H

#include <math.h>
#include <stdio.h>
#include <string.h>

// A test harness small enough to read in one sitting and with no dependency
// beyond the C library, so `make test` needs nothing installed that `make`
// does not already need.
//
// A suite is a plain function that calls CHECK*; the counters below are the
// only shared state. Failures do not abort -- one broken expectation should
// not hide the twenty after it.

extern int tests_run;
extern int tests_failed;
extern const char *current_suite;

#define FAIL(fmt, ...)                                                    \
    do {                                                                  \
        tests_failed++;                                                   \
        printf("  FAIL %s:%d (%s)\n       " fmt "\n", __FILE__, __LINE__, \
               current_suite, __VA_ARGS__);                               \
    } while (0)

#define CHECK(cond)                      \
    do {                                 \
        tests_run++;                     \
        if (!(cond))                     \
            FAIL("expected: %s", #cond); \
    } while (0)

#define CHECK_INT(actual, expected)                                     \
    do {                                                                \
        tests_run++;                                                    \
        long long a_ = (long long)(actual), e_ = (long long)(expected); \
        if (a_ != e_)                                                   \
            FAIL("%s: got %lld, want %lld", #actual, a_, e_);           \
    } while (0)

#define CHECK_STR(actual, expected)                               \
    do {                                                          \
        tests_run++;                                              \
        const char *a_ = (actual), *e_ = (expected);              \
        if (strcmp(a_, e_) != 0)                                  \
            FAIL("%s: got \"%s\", want \"%s\"", #actual, a_, e_); \
    } while (0)

// Floating point comparisons are all approximate here: the values under test
// come from trigonometry and accumulated sums, so an exact match would only
// ever be testing the libm build.
#define CHECK_NEAR(actual, expected, tolerance)                 \
    do {                                                        \
        tests_run++;                                            \
        double a_ = (double)(actual), e_ = (double)(expected);  \
        if (!(fabs(a_ - e_) <= (tolerance)))                    \
            FAIL("%s: got %g, want %g +/- %g", #actual, a_, e_, \
                 (double)(tolerance));                          \
    } while (0)

#define SUITE(name)       \
    current_suite = name; \
    printf("- %s\n", name)

void run_time_util_tests(void);
void run_settings_tests(void);
void run_filters_tests(void);
void run_gpx_tests(void);
void run_fifo_tests(void);
void run_heat_tests(void);
void run_point_index_tests(void);
void run_track_format_tests(void);
void run_track_splits_tests(void);
void run_track_series_tests(void);
void run_records_tests(void);
void run_stats_tests(void);
void run_background_tests(void);
void run_garmin_tests(void);
void run_anim_tests(void);
void run_zoom_tests(void);
void run_profiler_tests(void);

#endif
