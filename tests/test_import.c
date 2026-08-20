// The helper is spawned through the interpreter and the provider's script name,
// so pointing those at a shell script is all it takes to exercise the spawn,
// the pipes, the line protocol and the exit codes without an account anywhere.
// config.h comes first and is guarded, so the two files included below pick
// these up rather than the real values.
#include "../src/config.h"

#undef IMPORT_PYTHON
#if defined(_WIN32)
// Found on PATH rather than named absolutely: a native Windows binary cannot
// start "/bin/sh", which is a path only the MSYS2 runtime understands, but the
// same shell is on PATH as sh.exe under the environment the tests build in.
#define IMPORT_PYTHON "sh"
#else
#define IMPORT_PYTHON "/bin/sh"
#endif
#undef GARMIN_SCRIPT
#define GARMIN_SCRIPT "tests/fake_helper.sh"
#undef STRAVA_SCRIPT
#define STRAVA_SCRIPT "tests/fake_helper.sh"

// The module the spawn moved into, included here rather than tested on its own:
// what it does is start a process, and the suite below already starts fourteen.
//
// The flag asks it for the Windows command-line joiner as well. That code only
// runs on Windows, which is the platform hardest to try a change on, so it is
// built and checked here whichever platform the suite is running on.
#define SUBPROCESS_TEST_COMMAND_LINE
#include "../src/subprocess.c"

#include "../src/import_job.c"

#include "harness.h"

#include <stdlib.h>
#include <time.h>

// Waits for a job to finish, so a test reads a settled result rather than
// whatever the worker had got to. Bounded: a job that never ends should fail
// the checks below rather than hang the suite.
static bool wait_for_job(ImportJob *job, double seconds) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000}; // 1 ms
    for (int i = 0; i < (int)(seconds * 1000.0); i++) {
        if (import_collect(job))
            return true;
        nanosleep(&pause, NULL);
    }
    return false;
}

// One argument vector joined into the command line CreateProcess is given.
static const char *joined(char *const argv[]) {
    static char line[SUBPROCESS_COMMAND_MAX];
    if (!build_command_line(argv, line, sizeof(line)))
        return "<did not fit>";
    return line;
}

static void run_command_line_tests(void) {
    SUITE("subprocess: an ordinary argument list is joined with spaces");
    {
        char *argv[] = {(char *)"python3", (char *)"strava_sync.py", (char *)"sync", NULL};
        CHECK_STR(joined(argv), "python3 strava_sync.py sync");
    }

    SUITE("subprocess: an argument with a space in it is quoted");
    {
        // The library folder and the folder the binary sits in are both chosen
        // by whoever installed it, and Windows profiles have spaces in them.
        char *argv[] = {(char *)"py", (char *)"C:\\Program Files\\fp\\sync.py", NULL};
        CHECK_STR(joined(argv), "py \"C:\\Program Files\\fp\\sync.py\"");
    }

    SUITE("subprocess: an empty argument is quoted rather than dropped");
    {
        // Unquoted it does not arrive as an empty argument, it does not arrive
        // at all -- which is what the interpreter probe passes.
        char *argv[] = {(char *)"py", (char *)"-c", (char *)"", NULL};
        CHECK_STR(joined(argv), "py -c \"\"");
    }

    SUITE("subprocess: a trailing backslash is doubled inside the quotes");
    {
        // Left alone it would escape the quote that closes the argument, and
        // the next argument would be swallowed into this one.
        char *argv[] = {(char *)"py", (char *)"C:\\dir with space\\", (char *)"sync", NULL};
        CHECK_STR(joined(argv), "py \"C:\\dir with space\\\\\" sync");
    }

    SUITE("subprocess: an embedded quote is escaped, with its backslashes doubled");
    {
        char *argv[] = {(char *)"x", (char *)"a\"b", NULL};
        CHECK_STR(joined(argv), "x \"a\\\"b\"");

        char *escaped[] = {(char *)"x", (char *)"a\\\"b", NULL};
        CHECK_STR(joined(escaped), "x \"a\\\\\\\"b\"");
    }

    SUITE("subprocess: a command line that will not fit is refused, not cut");
    {
        // A truncated command line is a different command. Refusing turns it
        // into "could not start the import helper", which is at least true.
        char big[600];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';

        char *argv[] = {(char *)"py", big, NULL};
        char line[64];
        CHECK(!build_command_line(argv, line, sizeof(line)));
    }
}

void run_import_tests(void) {
    run_command_line_tests();

    // The session folders and the helper script are resolved through paths.h
    // now, so the suite says where its own data directory is rather than
    // writing into whoever is running it. build/ is already ignored and already
    // removed by `make clean`.
#if defined(_WIN32)
    _putenv("APPDATA=build/test-import");
#else
    setenv("XDG_DATA_HOME", "build/test-import", 1);
#endif
    CHECK(paths_init());

    SUITE("import: an idle job is not busy and collects nothing");
    {
        ImportJob job = {.provider = &import_garmin};
        CHECK(!import_busy(&job));
        CHECK(!import_collect(&job));
        CHECK_INT(import_stage(&job), IMPORT_IDLE);
        CHECK_NEAR(import_fraction(&job), 0.0f, 1e-6);
        // Stopping something that never started has to be safe: it is what
        // quitting does, whether an import was running or not.
        import_stop(&job);
    }

    SUITE("import: an import reports its progress and what landed");
    {
        ImportJob job = {.provider = &import_garmin};
        CHECK(import_start_sync(&job, "/tmp"));
        CHECK(import_busy(&job));
        CHECK(wait_for_job(&job, 10.0));

        CHECK_INT(import_stage(&job), IMPORT_DONE);
        CHECK_INT(import_total(&job), 3);
        CHECK_INT(import_completed(&job), 3);
        CHECK_NEAR(import_fraction(&job), 1.0f, 1e-6);
        // Two of the three: one activity failed, and saying so is the point of
        // keeping the message alongside the count.
        CHECK_INT(import_imported(&job), 2);
        CHECK_STR(import_message(&job), "activity 99: 404 not found");
        CHECK(!import_busy(&job));
        // Exactly once per job, the same contract background_collect keeps.
        CHECK(!import_collect(&job));
    }

    SUITE("import: each provider runs its own job");
    {
        // Two jobs at once is what the panel allows, so nothing either of them
        // counts may reach the other.
        ImportJob garmin = {.provider = &import_garmin};
        ImportJob strava = {.provider = &import_strava};

        CHECK(import_start_sync(&strava, "/tmp"));
        CHECK(import_busy(&strava));
        CHECK(!import_busy(&garmin));
        CHECK(wait_for_job(&strava, 10.0));

        CHECK_INT(import_stage(&strava), IMPORT_DONE);
        CHECK_INT(import_imported(&strava), 2);
        CHECK_INT(import_stage(&garmin), IMPORT_IDLE);
        CHECK_INT(import_imported(&garmin), 0);
    }

    SUITE("import: a session is looked for where its provider keeps it");
    {
        // A provider of its own rather than one of the two real ones, so the
        // answer does not depend on whether whoever runs the suite has signed
        // in to anything.
        const ImportProvider provider = {
            .name = "Test",
            .script = "tests/fake_helper.sh",
            .session_dir = "test_session",
            .token_file = "token.json",
            .import_subdir = "test_import",
        };

        // A name rather than a path, like the two real providers: where a
        // session folder actually is, is the data directory's business.
        char session_dir[PATHS_MAX], token_path[PATHS_MAX];
        paths_data(session_dir, sizeof(session_dir), provider.session_dir);
        paths_data(token_path, sizeof(token_path), "test_session/token.json");

        CHECK(platform_make_dirs(session_dir));
        remove(token_path);
        CHECK(!import_have_session(&provider));

        FILE *token = fopen(token_path, "w");
        CHECK(token != NULL);
        if (token)
            fclose(token);
        CHECK(import_have_session(&provider));

        remove(token_path);
    }

    SUITE("import: a second job is refused while one is running");
    {
        ImportJob job = {.provider = &import_garmin};
        CHECK(import_start_sync(&job, "SLOW"));
        CHECK(!import_start_sync(&job, "/tmp"));
        CHECK(!import_start_login(&job, "a@b.c", "secret", ""));
        import_stop(&job);
        CHECK(!import_busy(&job));
    }

    SUITE("import: stop ends a running import rather than waiting it out");
    {
        ImportJob job = {.provider = &import_garmin};
        CHECK(import_start_sync(&job, "SLOW"));

        // The helper sleeps for thirty seconds. Stopping has to end it, not
        // wait for it, or quitting mid-import would hang the window.
        time_t began = time(NULL);
        import_stop(&job);
        CHECK(time(NULL) - began < 5);

        CHECK(!import_busy(&job));
        CHECK_INT(import_stage(&job), IMPORT_IDLE);
    }

    SUITE("import: a login carries the credentials in over stdin");
    {
        ImportJob job = {.provider = &import_garmin};
        CHECK(import_start_login(&job, "runner@example.com", "secret", ""));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(import_stage(&job), IMPORT_DONE);

        // The secret does not outlive the process it was handed to.
        CHECK_INT(job.secret[0], 0);
        CHECK_INT(job.extra[0], 0);
    }

    SUITE("import: a login with no credentials fails and says so");
    {
        ImportJob job = {.provider = &import_garmin};
        CHECK(import_start_login(&job, "", "", ""));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(import_stage(&job), IMPORT_FAILED);
        CHECK_STR(import_message(&job), "an email address and a password are needed");
    }

    SUITE("import: a login Garmin wants a code for ends at MFA required");
    {
        ImportJob job = {.provider = &import_garmin};
        CHECK(import_start_login(&job, "runner@example.com", "needs-mfa", ""));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(import_stage(&job), IMPORT_MFA_REQUIRED);

        // The retry is the same call with the code filled in, which is why no
        // half-finished login has to be carried between the two.
        CHECK(import_start_login(&job, "runner@example.com", "needs-mfa", "123456"));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(import_stage(&job), IMPORT_DONE);
    }

    SUITE("import: a message is kept without the job counting as failed");
    {
        // What the Strava login says while the browser has it: something to
        // show, on a job that is still going to succeed.
        ImportJob job = {.provider = &import_strava};
        CHECK(import_start_login(&job, "12345", "needs-browser", ""));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(import_stage(&job), IMPORT_DONE);
        CHECK_STR(import_message(&job), "waiting for the browser");
    }

    SUITE("import: an import with no saved session fails");
    {
        ImportJob job = {.provider = &import_garmin};
        CHECK(import_start_sync(&job, "NOAUTH"));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(import_stage(&job), IMPORT_FAILED);
        CHECK_STR(import_message(&job), "not logged in: no token files found");
    }

    SUITE("import: the fraction stays in range whatever the counts say");
    {
        ImportJob job = {.provider = &import_garmin};
        // Nothing known yet: a bar drawn from this must not divide by zero.
        CHECK_NEAR(import_fraction(&job), 0.0f, 1e-6);

        atomic_store(&job.total, 4);
        atomic_store(&job.completed, 2);
        CHECK_NEAR(import_fraction(&job), 0.5f, 1e-6);

        // More done than there was to do is what a helper that miscounts looks
        // like, and it must not run the bar past its trough.
        atomic_store(&job.completed, 9);
        CHECK_NEAR(import_fraction(&job), 1.0f, 1e-6);
    }
}
