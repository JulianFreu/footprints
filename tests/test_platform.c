#include "../src/platform.c"

#include "harness.h"

#include <stdio.h>

// The shims themselves rather than what calls them: these are the four
// questions gpx_parser.c, map.c and heat.c stopped asking POSIX directly, and
// the Win32 answers have to agree with the POSIX ones on every one of them.
//
// Everything is built under build/, which `make clean` already removes and
// .gitignore already excludes, so a run leaves nothing behind worth tidying.
#define SCRATCH "build/test-scratch"

static void write_file(const char *path) {
    FILE *file = fopen(path, "wb");
    if (file) {
        fputs("x", file);
        fclose(file);
    }
}

// How many entries the walk hands back, and how many of those it called
// directories. The names themselves are checked separately -- what matters
// here is that "." and ".." are not among them.
static void count_entries(const char *path, int *files, int *dirs) {
    *files = 0;
    *dirs = 0;

    PlatformDir *dir = platform_dir_open(path);
    if (!dir)
        return;

    char name[256];
    bool is_dir;
    while (platform_dir_next(dir, name, sizeof(name), &is_dir)) {
        if (is_dir)
            (*dirs)++;
        else
            (*files)++;
    }
    platform_dir_close(dir);
}

void run_platform_tests(void) {
    SUITE("platform: a directory is made, and making it twice still succeeds");
    CHECK(platform_make_dirs(SCRATCH "/walk/nested"));
    CHECK(platform_file_exists(SCRATCH "/walk/nested"));
    // Every caller asks "is it there afterwards", not "did I make it", so the
    // second call is a success rather than an EEXIST.
    CHECK(platform_make_dir(SCRATCH "/walk/nested"));
    CHECK(platform_make_dirs(SCRATCH "/walk/nested"));

    SUITE("platform: a trailing separator still checks the last component");
    {
        // Every data root is handed over with one of these on the end, so a
        // version of this that reported success without looking would make
        // paths_init unable to fail.
        CHECK(platform_make_dirs(SCRATCH "/trailing/"));
        CHECK(platform_file_exists(SCRATCH "/trailing"));

        // Under a file rather than a directory: the last component cannot be
        // made, and saying otherwise is the bug above.
        write_file(SCRATCH "/walk-blocker");
        CHECK(!platform_make_dirs(SCRATCH "/walk-blocker/under-a-file/"));
    }

    SUITE("platform: a path that does not exist says so");
    CHECK(!platform_file_exists(SCRATCH "/walk/nothing-here"));
    CHECK(!platform_file_exists(SCRATCH "/no/such/tree"));

    SUITE("platform: a directory that cannot be opened is NULL, not a crash");
    CHECK(platform_dir_open(SCRATCH "/no/such/tree") == NULL);

    write_file(SCRATCH "/walk/one.gpx");
    write_file(SCRATCH "/walk/two.gpx");

    SUITE("platform: the walk reports files and directories apart");
    int files = 0, dirs = 0;
    count_entries(SCRATCH "/walk", &files, &dirs);
    CHECK_INT(files, 2);
    CHECK_INT(dirs, 1);

    SUITE("platform: \".\" and \"..\" are never handed to the caller");
    // Both would walk a recursive scan back up the tree, so the walk drops them
    // itself rather than leaving every caller to remember to.
    PlatformDir *dir = platform_dir_open(SCRATCH "/walk");
    CHECK(dir != NULL);
    if (dir) {
        char name[256];
        bool is_dir;
        while (platform_dir_next(dir, name, sizeof(name), &is_dir)) {
            CHECK(strcmp(name, ".") != 0);
            CHECK(strcmp(name, "..") != 0);
        }
        platform_dir_close(dir);
    }

    SUITE("platform: an empty directory ends the walk immediately");
    CHECK(platform_make_dirs(SCRATCH "/empty"));
    count_entries(SCRATCH "/empty", &files, &dirs);
    CHECK_INT(files, 0);
    CHECK_INT(dirs, 0);

    SUITE("platform: a name with no room in the caller's buffer is skipped");
    // Skipped rather than truncated: a cut name is a path to a different file,
    // which the scan would then try to parse.
    dir = platform_dir_open(SCRATCH "/walk");
    CHECK(dir != NULL);
    if (dir) {
        char tiny[4]; // shorter than "one.gpx", "two.gpx" and "nested"
        bool is_dir;
        CHECK(!platform_dir_next(dir, tiny, sizeof(tiny), &is_dir));
        platform_dir_close(dir);
    }

    SUITE("platform: the machine always has at least one core to divide by");
    CHECK(platform_cpu_count() >= 1);
}
