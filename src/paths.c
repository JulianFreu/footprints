#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "platform.h"

// The folder the per-user data directory is named after, under whatever the
// platform's own convention puts it in.
#define PATHS_APP_NAME "footprints"

#if defined(_WIN32)
#define PATHS_SEPARATOR '\\'
#else
#define PATHS_SEPARATOR '/'
#endif

// Both end in a separator, so appending a name is a concatenation and nobody
// has to remember which side of the join owns the slash.
static char exe_root[PATHS_MAX] = {0};
static char data_root[PATHS_MAX] = {0};

// --- Finding the roots ---

// Cuts a path back to the directory holding it, keeping the separator. False
// when there is no separator to cut at, which means the caller was handed a
// bare name and has no directory to offer.
static bool keep_directory(char *path) {
    for (size_t i = strlen(path); i > 0; i--) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') {
            path[i] = '\0';
            return true;
        }
    }
    return false;
}

// Where the running binary is. Asked of the operating system rather than worked
// out from argv[0], which says whatever the caller's shell felt like and is
// empty often enough to matter.
static void resolve_exe_root(void) {
#if defined(_WIN32)
    char buffer[PATHS_MAX];
    DWORD length = GetModuleFileNameA(NULL, buffer, sizeof(buffer));
    if (length > 0 && length < sizeof(buffer) && keep_directory(buffer)) {
        snprintf(exe_root, sizeof(exe_root), "%s", buffer);
        return;
    }
#else
    char buffer[PATHS_MAX];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0 && (size_t)length < sizeof(buffer)) {
        buffer[length] = '\0';
        if (keep_directory(buffer)) {
            snprintf(exe_root, sizeof(exe_root), "%s", buffer);
            return;
        }
    }
#endif
    // Nothing to do but look where the program was started from. This is what
    // the application did everywhere before these roots existed, so a platform
    // that will not answer behaves the way it always used to rather than
    // failing to find its font.
    snprintf(exe_root, sizeof(exe_root), ".%c", PATHS_SEPARATOR);
}

// An environment variable that is set and not empty. Empty counts as unset: it
// is what a shell leaves behind after clearing one, and joining a name onto it
// would build a path into the root of the filesystem.
static const char *env_or_null(const char *name) {
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : NULL;
}

// Where this user's data goes, by each platform's own convention: %APPDATA% on
// Windows, and the XDG data directory on everything else.
static void resolve_data_root(void) {
#if defined(_WIN32)
    const char *appdata = env_or_null("APPDATA");
    if (appdata) {
        snprintf(data_root, sizeof(data_root), "%s\\%s\\", appdata, PATHS_APP_NAME);
        return;
    }
#else
    const char *xdg = env_or_null("XDG_DATA_HOME");
    if (xdg) {
        snprintf(data_root, sizeof(data_root), "%s/%s/", xdg, PATHS_APP_NAME);
        return;
    }
    const char *home = env_or_null("HOME");
    if (home) {
        snprintf(data_root, sizeof(data_root), "%s/.local/share/%s/", home, PATHS_APP_NAME);
        return;
    }
#endif
    // No home to write into. The working directory is the only thing left, and
    // is where all of this used to be kept anyway.
    snprintf(data_root, sizeof(data_root), ".%c", PATHS_SEPARATOR);
}

bool paths_init(void) {
    resolve_exe_root();
    resolve_data_root();
    return platform_make_dirs(data_root);
}

// --- Building a path ---

void paths_resource(char *out, size_t size, const char *name) {
    snprintf(out, size, "%sresources%c%s", exe_root, PATHS_SEPARATOR, name);
}

void paths_data(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s%s", data_root, name);
}

void paths_exe(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s%s", exe_root, name);
}

const char *paths_data_root(void) {
    return data_root;
}

const char *paths_exe_root(void) {
    return exe_root;
}

// --- Coming from an older install ---

bool paths_migrate_from_cwd(const char *name) {
    char destination[PATHS_MAX];
    paths_data(destination, sizeof(destination), name);

    // Only ever a first-run step. Once the data directory has its own copy, the
    // one in the working directory is somebody else's file.
    if (platform_file_exists(destination) || !platform_file_exists(name))
        return false;

    FILE *from = fopen(name, "rb");
    if (!from)
        return false;

    FILE *to = fopen(destination, "wb");
    if (!to) {
        fclose(from);
        return false;
    }

    char chunk[4096];
    size_t read_bytes;
    bool copied = true;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), from)) > 0) {
        if (fwrite(chunk, 1, read_bytes, to) != read_bytes) {
            copied = false;
            break;
        }
    }

    if (ferror(from))
        copied = false;
    if (fclose(to) != 0)
        copied = false;
    fclose(from);

    // A half-written settings file would be read back as a corrupt one, which
    // is worse than looking like a first run.
    if (!copied)
        remove(destination);
    return copied;
}
