#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

// The handful of things the application asks the operating system for that
// POSIX and Win32 spell differently. Every question here is the same question
// on both; the two answers live side by side in platform.c.
//
// Nothing in here includes SDL, which is what lets the modules that call it go
// on being included by the test suite the way every other module is -- the
// suite links no SDL.

// Creates one directory. True when it exists afterwards, whether or not this
// call is what made it, which is the question every caller actually has.
bool platform_make_dir(const char *path);

// Creates `path` along with every missing directory above it.
bool platform_make_dirs(const char *path);

// Whether there is anything at `path` at all.
bool platform_file_exists(const char *path);

// How many threads the machine runs at once. Never below one, so a caller may
// divide by it.
int platform_cpu_count(void);

// Walking a directory. "." and ".." are skipped here rather than by each
// caller, and `is_dir` is answered whichever way the platform is cheapest at:
// the entry itself when it says, and a stat only when it does not.
typedef struct PlatformDir PlatformDir;

PlatformDir *platform_dir_open(const char *path);
bool platform_dir_next(PlatformDir *dir, char *name, size_t size, bool *is_dir);
void platform_dir_close(PlatformDir *dir);

#endif // PLATFORM_H
