#include "harness.h"

int tests_run = 0;
int tests_failed = 0;
const char *current_suite = "";

int main(void) {
    run_time_util_tests();
    run_fifo_tests();
    run_filters_tests();
    run_gpx_tests();
    run_heat_tests();
    run_point_index_tests();
    run_track_format_tests();
    run_stats_tests();
    run_background_tests();
    run_anim_tests();
    run_zoom_tests();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
