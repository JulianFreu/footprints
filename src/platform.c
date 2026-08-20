#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Long enough for any path the application builds, which is bounded by the
// library folder plus one entry name. Local to this file: the interface hands
// the caller's own buffer back, so nobody else has to agree on a size.
#define PLATFORM_PATH_MAX 1024

// --- Directories and files ---

bool platform_make_dir(const char *path) {
#if defined(_WIN32)
    if (CreateDirectoryA(path, NULL))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (mkdir(path, 0755) == 0)
        return true;
    return errno == EEXIST;
#endif
}

// Whether `c` ends a directory component. Windows accepts either, and a path
// built from an environment variable and one built here will not agree on
// which, so both are separators wherever a path is taken apart.
static bool is_separator(char c) {
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool platform_make_dirs(const char *path) {
    char partial[PLATFORM_PATH_MAX];
    size_t length = strlen(path);

    if (length == 0 || length >= sizeof(partial))
        return false;
    memcpy(partial, path, length + 1);

    // Trimmed first, or the last real component would be made by the loop below
    // as though it were a parent -- and the loop ignores what those answer, so
    // the one directory the caller actually asked for would go unchecked. Every
    // data root is handed over with one of these on the end.
    while (length > 1 && is_separator(partial[length - 1]))
        partial[--length] = '\0';

    // From the second character on, so a leading "/" is not taken for a
    // directory to create, and neither is the "C:" of an absolute Windows path
    // -- both are already there, and asking for them fails.
    for (size_t i = 1; i < length; i++) {
        if (!is_separator(partial[i]))
            continue;

        char separator = partial[i];
        partial[i] = '\0';
        // A parent that cannot be made is only fatal if the last component
        // fails too: a drive letter or a mount point answers "no" here and is
        // still a perfectly good place to create something under.
        platform_make_dir(partial);
        partial[i] = separator;
    }

    return platform_make_dir(partial);
}

bool platform_file_exists(const char *path) {
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path, F_OK) == 0;
#endif
}

// --- How wide the machine is ---

int platform_cpu_count(void) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    int count = (int)info.dwNumberOfProcessors;
#else
    int count = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return count > 0 ? count : 1;
}

// --- Walking a directory ---

#if defined(_WIN32)

struct PlatformDir {
    HANDLE handle;
    WIN32_FIND_DATAA entry;
    // FindFirstFileA both opens the search and returns its first result, so the
    // first entry is in hand before anything asks for one.
    bool holding_entry;
};

PlatformDir *platform_dir_open(const char *path) {
    char pattern[PLATFORM_PATH_MAX];
    int written = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (written <= 0 || (size_t)written >= sizeof(pattern))
        return NULL;

    PlatformDir *dir = (PlatformDir *)calloc(1, sizeof(*dir));
    if (!dir)
        return NULL;

    dir->handle = FindFirstFileA(pattern, &dir->entry);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->holding_entry = true;
    return dir;
}

bool platform_dir_next(PlatformDir *dir, char *name, size_t size, bool *is_dir) {
    for (;;) {
        if (dir->holding_entry)
            dir->holding_entry = false;
        else if (!FindNextFileA(dir->handle, &dir->entry))
            return false;

        const char *found = dir->entry.cFileName;
        if (strcmp(found, ".") == 0 || strcmp(found, "..") == 0)
            continue;

        int written = snprintf(name, size, "%s", found);
        if (written <= 0 || (size_t)written >= size)
            continue; // a name the caller has no room for is skipped, not cut

        *is_dir = (dir->entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return true;
    }
}

void platform_dir_close(PlatformDir *dir) {
    if (!dir)
        return;
    FindClose(dir->handle);
    free(dir);
}

#else

struct PlatformDir {
    DIR *dir;
    // Kept so the stat fallback below has something to stat: d_type answers for
    // nearly every filesystem, and the few that say DT_UNKNOWN need the path.
    char path[PLATFORM_PATH_MAX];
};

PlatformDir *platform_dir_open(const char *path) {
    size_t length = strlen(path);
    if (length >= PLATFORM_PATH_MAX)
        return NULL;

    DIR *handle = opendir(path);
    if (!handle)
        return NULL;

    PlatformDir *dir = (PlatformDir *)calloc(1, sizeof(*dir));
    if (!dir) {
        closedir(handle);
        return NULL;
    }
    dir->dir = handle;
    memcpy(dir->path, path, length + 1);
    return dir;
}

static bool entry_is_dir(const PlatformDir *dir, const struct dirent *entry) {
    if (entry->d_type != DT_UNKNOWN)
        return entry->d_type == DT_DIR;

    char full_path[PLATFORM_PATH_MAX];
    int written = snprintf(full_path, sizeof(full_path), "%s/%s", dir->path, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(full_path))
        return false;

    struct stat info;
    return stat(full_path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool platform_dir_next(PlatformDir *dir, char *name, size_t size, bool *is_dir) {
    struct dirent *entry;

    while ((entry = readdir(dir->dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        int written = snprintf(name, size, "%s", entry->d_name);
        if (written <= 0 || (size_t)written >= size)
            continue; // a name the caller has no room for is skipped, not cut

        *is_dir = entry_is_dir(dir, entry);
        return true;
    }
    return false;
}

void platform_dir_close(PlatformDir *dir) {
    if (!dir)
        return;
    closedir(dir->dir);
    free(dir);
}

#endif
