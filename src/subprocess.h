#ifndef SUBPROCESS_H
#define SUBPROCESS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Running a helper program and reading what it says.
//
// Split out of import_job.c so that the one thing POSIX and Win32 disagree
// about completely -- starting a process -- is answered twice here rather than
// splitting the import worker into two copies of itself.
//
// The contract the import depends on is that the credentials reach the helper
// over its standard input and nowhere else. Not on the command line, which ps
// and Task Manager show to everyone, and not through the environment, which the
// whole process tree inherits. That is why this is a spawn with pipes rather
// than a call to popen or system.

typedef struct Subprocess {
    // The child, in whatever a platform calls one: a pid on POSIX, a process
    // handle on Windows. Zero when there is none. Wide enough for either, so
    // that the caller can keep it in one atomic and end the child from another
    // thread without knowing which it is holding.
    uintptr_t id;
    // The child's standard output. Owned by the Subprocess and closed by
    // subprocess_wait.
    FILE *out;
} Subprocess;

// Starts argv[0], looked up on PATH, with `argv` as its arguments. When
// `stdin_text` is given it is written to the child and its input is then
// closed, so a helper reading a fixed number of lines is not left waiting for
// one more. False when the child could not be started at all.
bool subprocess_start(char *const argv[], const char *stdin_text, Subprocess *proc);

// Ends a running child. Takes the id rather than the Subprocess because the
// thread that cancels a job is not the one that owns it -- it reads the id out
// of an atomic and calls this, which is safe at any point after the start.
void subprocess_terminate(uintptr_t id);

// Waits for the child to finish and closes its output. Returns the exit code,
// or -1 if it was killed rather than exiting -- which is what cancelling a job
// looks like from here.
int subprocess_wait(Subprocess *proc);

// The Python interpreter the helpers run under: the first of the platform's
// candidate names that is present and actually runs. NULL when none is, which
// is a normal outcome on a machine without Python rather than an error.
//
// Probed once on the first call and remembered. Defining IMPORT_PYTHON at
// compile time names one outright and skips the probe.
const char *subprocess_python(void);

#endif // SUBPROCESS_H
