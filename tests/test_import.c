// The helper is spawned through the interpreter and the provider's script name,
// so pointing those at a shell script is all it takes to exercise the fork, the
// pipes, the line protocol and the exit codes without an account anywhere.
// config.h comes first and is guarded, so including import_job.c below picks
// these up rather than the real values.
#include "../src/config.h"

#undef IMPORT_PYTHON
#define IMPORT_PYTHON "/bin/sh"
#undef GARMIN_SCRIPT
#define GARMIN_SCRIPT "tests/fake_helper.sh"
#undef STRAVA_SCRIPT
#define STRAVA_SCRIPT "tests/fake_helper.sh"

#include "../src/import_job.c"

#include "harness.h"

#include <sys/stat.h>
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

void run_import_tests(void) {
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
            .session_dir = "/tmp/footprints-test-session",
            .token_file = "token.json",
            .import_subdir = "test_import",
        };

        mkdir(provider.session_dir, 0700);
        unlink("/tmp/footprints-test-session/token.json");
        CHECK(!import_have_session(&provider));

        FILE *token = fopen("/tmp/footprints-test-session/token.json", "w");
        CHECK(token != NULL);
        if (token)
            fclose(token);
        CHECK(import_have_session(&provider));

        unlink("/tmp/footprints-test-session/token.json");
        rmdir(provider.session_dir);
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
