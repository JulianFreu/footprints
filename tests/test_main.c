#include "../src/settings.h"

#include "harness.h"

int tests_run = 0;
int tests_failed = 0;
const char *current_suite = "";

int main(void) {
    // The heat calculation and the parser read the settings global, which the
    // application fills in from the settings file before anything else runs.
    // Nothing here loads a file, so the defaults stand in for it -- without
    // this the suites below would be measuring a zeroed radius.
    settings_defaults(&settings);

    run_platform_tests();
    run_paths_tests();
    run_time_util_tests();
    run_settings_tests();
    run_fifo_tests();
    run_filters_tests();
    run_gpx_tests();
    run_gpx_scan_tests();
    run_track_splits_tests();
    run_track_series_tests();
    run_heat_tests();
    run_point_index_tests();
    run_track_format_tests();
    run_stats_tests();
    run_records_tests();
    run_background_tests();
    run_import_tests();
    run_anim_tests();
    run_zoom_tests();
    run_profiler_tests();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
