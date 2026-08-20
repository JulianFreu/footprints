#ifndef PATHS_H
#define PATHS_H

#include <stdbool.h>
#include <stddef.h>

// Long enough for either root plus anything the application appends to it. One
// constant so every buffer a path is built into agrees on a size.
#define PATHS_MAX 1024

// Where the application's files are, resolved once at startup and read-only
// afterwards -- which is what lets the download thread and the import worker
// build paths without locking anything.
//
// Two roots, because the two kinds of file have opposite requirements. The
// fonts and icons ship with the binary and are never written, so they are found
// beside it. The tile cache, the settings file, the session tokens and the
// default library are written constantly and belong to the person running the
// program, not to the installation, so they live under the per-user data
// directory the platform nominates. Neither is the working directory: the
// binary used to be run from the project root, and no longer has to be.
//
// Deliberately no SDL here, even though SDL_GetBasePath and SDL_GetPrefPath
// answer the same two questions. The modules that build paths are included by
// the test suite, and the suite links no SDL.

// Fills both roots, creating the data directory if it is missing. False when
// the data directory could not be made, which leaves nothing writable and is
// worth stopping for.
bool paths_init(void);

// <directory holding the executable>/resources/<name>
void paths_resource(char *out, size_t size, const char *name);

// <per-user data directory>/<name>
void paths_data(char *out, size_t size, const char *name);

// <directory holding the executable>/<name> -- the Python import helpers, which
// ship beside the binary and are run rather than read.
void paths_exe(char *out, size_t size, const char *name);

// The roots themselves, for the few callers that build a deeper path of their
// own. Both end in a separator and neither changes after paths_init.
const char *paths_data_root(void);
const char *paths_exe_root(void);

// Carries `name` from the working directory into the data directory when the
// data directory has no copy of it yet. This is what an existing install
// upgrades through: the settings file used to be written beside the binary in
// the project root, and would otherwise look like a first run. True when the
// file was copied, so the caller can say what it did.
bool paths_migrate_from_cwd(const char *name);

#endif // PATHS_H
