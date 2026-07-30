#ifndef LOG_H
#define LOG_H

#include <stdio.h>

// Verbose per-file parse diagnostics. Off by default: parsing a thousand GPX
// files emitted roughly fifteen thousand lines at startup. Build with
// -DFOOTPRINTS_DEBUG_LOG (or `make OPT="-O2 -DFOOTPRINTS_DEBUG_LOG"`) to
// restore them.
#ifdef FOOTPRINTS_DEBUG_LOG
#define LOG_DEBUG(...) printf(__VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

#endif
