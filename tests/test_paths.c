#include "../src/paths.c"

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>

// Where the application's files are, which is the question that used to be
// answered by "the working directory" everywhere. The roots themselves come
// from the environment, so pointing that at a scratch directory is all it takes
// to exercise the resolution and the migration without touching a real profile.
#define SCRATCH "build/test-paths"

static bool ends_with_separator(const char *path) {
    size_t length = strlen(path);
    return length > 0 && (path[length - 1] == '/' || path[length - 1] == '\\');
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file) {
        fputs(text, file);
        fclose(file);
    }
}

static bool file_says(const char *path, const char *text) {
    char buffer[64] = {0};
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    size_t read_bytes = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[read_bytes] = '\0';
    return strcmp(buffer, text) == 0;
}

void run_paths_tests(void) {
    // The suite runs from the project root, which is where `make test` runs the
    // binary from, so a relative scratch directory lands somewhere known.
    CHECK(platform_make_dirs(SCRATCH));

#if defined(_WIN32)
    _putenv("APPDATA=" SCRATCH);
#else
    setenv("XDG_DATA_HOME", SCRATCH, 1);
#endif

    SUITE("paths: init resolves both roots and creates the data directory");
    CHECK(paths_init());
    CHECK(platform_file_exists(paths_data_root()));

    SUITE("paths: both roots end in a separator, so appending is a join");
    // The whole interface below is a concatenation. If a root ever came back
    // without its trailing separator, every path built from it would lose one.
    CHECK(ends_with_separator(paths_data_root()));
    CHECK(ends_with_separator(paths_exe_root()));

    SUITE("paths: a resource sits under resources/ beside the binary");
    char resource[PATHS_MAX];
    paths_resource(resource, sizeof(resource), "Roboto-Regular.ttf");
    CHECK(strstr(resource, paths_exe_root()) == resource);
    CHECK(strstr(resource, "Roboto-Regular.ttf") != NULL);
    CHECK(strstr(resource, "resources") != NULL);

    SUITE("paths: a data file sits directly under the data root");
    char data[PATHS_MAX];
    paths_data(data, sizeof(data), "settings.conf");
    CHECK(strstr(data, paths_data_root()) == data);
    CHECK(strstr(data, "settings.conf") != NULL);
    // No resources/ in between: this is the half of the split that is written,
    // not the half that ships with the binary.
    CHECK(strstr(data, "resources") == NULL);

    SUITE("paths: a helper script sits directly beside the binary");
    char script[PATHS_MAX];
    paths_exe(script, sizeof(script), "garmin_sync.py");
    CHECK(strstr(script, paths_exe_root()) == script);
    CHECK(strstr(script, "resources") == NULL);

    SUITE("paths: nothing to migrate is not a migration");
    CHECK(!paths_migrate_from_cwd("a-file-that-is-not-here.conf"));

    SUITE("paths: a file in the working directory is carried into the data dir");
    // This is the upgrade path: an install from before the data directory kept
    // its settings beside the binary, and would otherwise read as a first run.
    remove(SCRATCH "/migrate-me.conf");
    write_file("migrate-me.conf", "carried");
    CHECK(paths_migrate_from_cwd("migrate-me.conf"));

    char migrated[PATHS_MAX];
    paths_data(migrated, sizeof(migrated), "migrate-me.conf");
    CHECK(platform_file_exists(migrated));
    CHECK(file_says(migrated, "carried"));

    SUITE("paths: a data directory that already has the file is left alone");
    // Only ever a first-run step. Once the data directory has its own copy, the
    // one in the working directory is somebody else's file.
    write_file("migrate-me.conf", "newer");
    CHECK(!paths_migrate_from_cwd("migrate-me.conf"));
    CHECK(file_says(migrated, "carried"));

    remove("migrate-me.conf");
    remove(migrated);
}
