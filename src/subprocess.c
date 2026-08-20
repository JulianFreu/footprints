#include "subprocess.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// --- Joining the arguments back up, for Windows ---

// CreateProcess takes one command line rather than a vector, so the arguments
// have to be joined back into the form the child's own C runtime will split
// again. The rules are the runtime's, not the shell's: a backslash only escapes
// something in front of a quote, and an empty argument has to be quoted or it
// vanishes from the child's argv rather than arriving empty.
//
// Compiled wherever the tests are built rather than only on Windows. There is
// no Win32 call in any of it -- it is string handling, of the fiddly kind that
// is wrong in one case out of eight -- and Windows is the platform hardest to
// try a change on, so the suite checks it on whichever platform it runs.
#if defined(_WIN32) || defined(SUBPROCESS_TEST_COMMAND_LINE)

static bool needs_quotes(const char *arg) {
    if (arg[0] == '\0')
        return true;
    return strpbrk(arg, " \t\"") != NULL;
}

// Appends one character, refusing rather than truncating: a command line cut
// short is a different command, and the library folder is user-chosen and long.
static bool append(char *out, size_t size, size_t *length, char c) {
    if (*length + 1 >= size)
        return false;
    out[(*length)++] = c;
    return true;
}

static bool append_argument(char *out, size_t size, size_t *length, const char *arg) {
    if (!needs_quotes(arg)) {
        for (const char *c = arg; *c; c++)
            if (!append(out, size, length, *c))
                return false;
        return true;
    }

    if (!append(out, size, length, '"'))
        return false;

    for (const char *c = arg;; c++) {
        size_t backslashes = 0;
        while (*c == '\\') {
            backslashes++;
            c++;
        }

        if (*c == '\0') {
            // Doubled, because the quote that closes the argument would
            // otherwise be escaped by the last of them.
            for (size_t i = 0; i < backslashes * 2; i++)
                if (!append(out, size, length, '\\'))
                    return false;
            break;
        }

        if (*c == '"') {
            // Doubled for the same reason, and one more to escape the quote
            // itself so it arrives as a character rather than ending the
            // argument.
            for (size_t i = 0; i < backslashes * 2 + 1; i++)
                if (!append(out, size, length, '\\'))
                    return false;
        } else {
            for (size_t i = 0; i < backslashes; i++)
                if (!append(out, size, length, '\\'))
                    return false;
        }

        if (!append(out, size, length, *c))
            return false;
    }

    return append(out, size, length, '"');
}

static bool build_command_line(char *const argv[], char *out, size_t size) {
    size_t length = 0;

    for (int i = 0; argv[i]; i++) {
        if (i > 0 && !append(out, size, &length, ' '))
            return false;
        if (!append_argument(out, size, &length, argv[i]))
            return false;
    }

    if (length >= size)
        return false;
    out[length] = '\0';
    return true;
}

#endif

#if defined(_WIN32)

// --- Windows ---

bool subprocess_start(char *const argv[], const char *stdin_text, Subprocess *proc) {
    proc->id = 0;
    proc->out = NULL;

    char command_line[SUBPROCESS_COMMAND_MAX];
    if (!build_command_line(argv, command_line, sizeof(command_line)))
        return false;

    // Inheritable by default, because the child needs its two ends. The
    // parent's ends are taken back out of the inherit set below.
    SECURITY_ATTRIBUTES inherit = {
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .bInheritHandle = TRUE,
    };

    HANDLE to_child_read = NULL, to_child_write = NULL;
    HANDLE from_child_read = NULL, from_child_write = NULL;

    if (!CreatePipe(&to_child_read, &to_child_write, &inherit, 0))
        return false;
    if (!CreatePipe(&from_child_read, &from_child_write, &inherit, 0)) {
        CloseHandle(to_child_read);
        CloseHandle(to_child_write);
        return false;
    }

    // Without this the child holds a copy of the parent's ends, and the read
    // below never sees end-of-file because one writer is still open -- in this
    // process, waiting for the child that is waiting for it.
    SetHandleInformation(to_child_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(from_child_read, HANDLE_FLAG_INHERIT, 0);

    // The build asks for no console, so on the shipped binary there is no
    // standard error to hand down -- and naming an invalid handle while
    // STARTF_USESTDHANDLES is set gives the child a broken stderr rather than
    // none. Pointing it at the output pipe instead keeps whatever the helper
    // complains about: handle_line passes anything it does not recognise to
    // the debug log rather than treating it as protocol.
    HANDLE parent_stderr = GetStdHandle(STD_ERROR_HANDLE);
    if (parent_stderr == NULL || parent_stderr == INVALID_HANDLE_VALUE)
        parent_stderr = from_child_write;

    STARTUPINFOA startup = {
        .cb = sizeof(STARTUPINFOA),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = to_child_read,
        .hStdOutput = from_child_write,
        .hStdError = parent_stderr,
    };
    PROCESS_INFORMATION child = {0};

    // NULL for the application name so the command line's first word is looked
    // up on PATH, which is how the interpreter is found by name.
    BOOL started = CreateProcessA(NULL, command_line, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL, &startup, &child);

    CloseHandle(to_child_read);
    CloseHandle(from_child_write);

    if (!started) {
        CloseHandle(to_child_write);
        CloseHandle(from_child_read);
        return false;
    }
    CloseHandle(child.hThread);

    if (stdin_text) {
        // Three short lines, far under a pipe buffer, so this cannot block on a
        // helper that has not started reading yet.
        DWORD written = 0;
        WriteFile(to_child_write, stdin_text, (DWORD)strlen(stdin_text), &written, NULL);
    }
    // Closing the end is what stops the helper waiting for a line that is not
    // coming.
    CloseHandle(to_child_write);

    // The C runtime takes ownership of the handle here, and gives it back as a
    // stream so that reading the helper's output is the same loop it is on
    // POSIX.
    int fd = _open_osfhandle((intptr_t)from_child_read, _O_RDONLY);
    if (fd == -1) {
        CloseHandle(from_child_read);
        TerminateProcess(child.hProcess, SUBPROCESS_KILLED_CODE);
        CloseHandle(child.hProcess);
        return false;
    }

    proc->out = _fdopen(fd, "r");
    if (!proc->out) {
        _close(fd);
        TerminateProcess(child.hProcess, SUBPROCESS_KILLED_CODE);
        CloseHandle(child.hProcess);
        return false;
    }

    proc->id = (uintptr_t)child.hProcess;
    return true;
}

void subprocess_terminate(uintptr_t id) {
    if (id)
        TerminateProcess((HANDLE)id, SUBPROCESS_KILLED_CODE);
}

int subprocess_wait(Subprocess *proc) {
    if (proc->out) {
        fclose(proc->out);
        proc->out = NULL;
    }
    if (!proc->id)
        return -1;

    HANDLE handle = (HANDLE)proc->id;
    WaitForSingleObject(handle, INFINITE);

    DWORD code = 0;
    bool exited = GetExitCodeProcess(handle, &code) != 0;
    CloseHandle(handle);
    proc->id = 0;

    // The one Windows cannot say for itself: a terminated process reports the
    // exit code its killer passed, with nothing to mark it as killed. This is
    // the code subprocess_terminate uses and nothing else does, so it stands in
    // for the "did not exit on its own" that POSIX reports outright.
    if (!exited || code == SUBPROCESS_KILLED_CODE)
        return -1;
    return (int)code;
}

#else

// --- POSIX ---

bool subprocess_start(char *const argv[], const char *stdin_text, Subprocess *proc) {
    int to_child[2], from_child[2];

    proc->id = 0;
    proc->out = NULL;

    if (pipe(to_child) != 0) {
        perror("pipe");
        return false;
    }
    if (pipe(from_child) != 0) {
        perror("pipe");
        close(to_child[0]);
        close(to_child[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return false;
    }

    if (pid == 0) {
        // Nothing but dup2 and execvp between the fork and the exec. Anything
        // else would be running in a child that inherited locks the other
        // threads of this process were holding when it was forked.
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        execvp(argv[0], argv);
        _exit(127); // read back as "could not run the interpreter"
    }

    close(to_child[0]);
    close(from_child[1]);

    // A helper that died before reading would otherwise end this process with
    // SIGPIPE. Ignoring it turns that into a short write, which the exit code
    // reports properly.
    signal(SIGPIPE, SIG_IGN);

    if (stdin_text) {
        // Three short lines, far under a pipe buffer, so this cannot block on a
        // helper that has not started reading yet.
        ssize_t written = write(to_child[1], stdin_text, strlen(stdin_text));
        (void)written;
    }
    // Closing the end is what stops the helper waiting for a line that is not
    // coming.
    close(to_child[1]);

    proc->out = fdopen(from_child[0], "r");
    if (!proc->out) {
        close(from_child[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return false;
    }

    proc->id = (uintptr_t)pid;
    return true;
}

void subprocess_terminate(uintptr_t id) {
    if (id)
        kill((pid_t)id, SIGTERM);
}

int subprocess_wait(Subprocess *proc) {
    if (proc->out) {
        fclose(proc->out);
        proc->out = NULL;
    }
    if (!proc->id)
        return -1;

    int status = 0;
    waitpid((pid_t)proc->id, &status, 0);
    proc->id = 0;

    // Killed rather than finished, which is what cancelling a job looks like
    // from here.
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

#endif

// --- Which Python ---

const char *subprocess_python(void) {
    static const char *interpreter = NULL;
    static bool probed = false;

    // Resolved on the first call, which startup makes before any import can be
    // started, and never again -- probing costs a process per candidate.
    if (probed)
        return interpreter;
    probed = true;

#if defined(IMPORT_PYTHON)
    // Named outright at compile time. This is how the test suite points the
    // helpers at a shell script instead.
    interpreter = IMPORT_PYTHON;
#else
    static const char *const candidates[] = IMPORT_PYTHON_CANDIDATES;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        // Run rather than merely looked for on PATH: what Windows installs
        // under the name "python" after no install at all is a stub that opens
        // the Store, and it is only telling that apart from an interpreter by
        // asking it to execute something.
        char *const probe_argv[] = {(char *)candidates[i], (char *)"-c", (char *)"", NULL};
        Subprocess probe;

        if (!subprocess_start(probe_argv, NULL, &probe))
            continue;
        if (subprocess_wait(&probe) == 0) {
            interpreter = candidates[i];
            break;
        }
    }
#endif

    return interpreter;
}
