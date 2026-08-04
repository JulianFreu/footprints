// The helper is spawned through two macros, so pointing them at a shell script
// is all it takes to exercise the fork, the pipes, the line protocol and the
// exit codes without a Garmin account. config.h comes first and is guarded, so
// including garmin.c below picks these up rather than the real values.
#include "../src/config.h"

#undef GARMIN_PYTHON
#define GARMIN_PYTHON "/bin/sh"
#undef GARMIN_SCRIPT
#define GARMIN_SCRIPT "tests/fake_garmin.sh"

#include "../src/garmin.c"

#include "harness.h"

#include <time.h>

// Waits for a job to finish, so a test reads a settled result rather than
// whatever the worker had got to. Bounded: a job that never ends should fail
// the checks below rather than hang the suite.
static bool wait_for_job(GarminJob *job, double seconds) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000}; // 1 ms
    for (int i = 0; i < (int)(seconds * 1000.0); i++) {
        if (garmin_collect(job))
            return true;
        nanosleep(&pause, NULL);
    }
    return false;
}

void run_garmin_tests(void) {
    SUITE("garmin: an idle job is not busy and collects nothing");
    {
        GarminJob job = {0};
        CHECK(!garmin_busy(&job));
        CHECK(!garmin_collect(&job));
        CHECK_INT(garmin_stage(&job), GARMIN_IDLE);
        CHECK_NEAR(garmin_fraction(&job), 0.0f, 1e-6);
        // Stopping something that never started has to be safe: it is what
        // quitting does, whether an import was running or not.
        garmin_stop(&job);
    }

    SUITE("garmin: an import reports its progress and what landed");
    {
        GarminJob job = {0};
        CHECK(garmin_start_sync(&job, "/tmp"));
        CHECK(garmin_busy(&job));
        CHECK(wait_for_job(&job, 10.0));

        CHECK_INT(garmin_stage(&job), GARMIN_DONE);
        CHECK_INT(garmin_total(&job), 3);
        CHECK_INT(garmin_completed(&job), 3);
        CHECK_NEAR(garmin_fraction(&job), 1.0f, 1e-6);
        // Two of the three: one activity failed, and saying so is the point of
        // keeping the message alongside the count.
        CHECK_INT(garmin_imported(&job), 2);
        CHECK_STR(garmin_message(&job), "activity 99: 404 not found");
        CHECK(!garmin_busy(&job));
        // Exactly once per job, the same contract background_collect keeps.
        CHECK(!garmin_collect(&job));
    }

    SUITE("garmin: a second job is refused while one is running");
    {
        GarminJob job = {0};
        CHECK(garmin_start_sync(&job, "SLOW"));
        CHECK(!garmin_start_sync(&job, "/tmp"));
        CHECK(!garmin_start_login(&job, "a@b.c", "secret", ""));
        garmin_stop(&job);
        CHECK(!garmin_busy(&job));
    }

    SUITE("garmin: stop ends a running import rather than waiting it out");
    {
        GarminJob job = {0};
        CHECK(garmin_start_sync(&job, "SLOW"));

        // The helper sleeps for thirty seconds. Stopping has to end it, not
        // wait for it, or quitting mid-import would hang the window.
        time_t began = time(NULL);
        garmin_stop(&job);
        CHECK(time(NULL) - began < 5);

        CHECK(!garmin_busy(&job));
        CHECK_INT(garmin_stage(&job), GARMIN_IDLE);
    }

    SUITE("garmin: a login carries the credentials in over stdin");
    {
        GarminJob job = {0};
        CHECK(garmin_start_login(&job, "runner@example.com", "secret", ""));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(garmin_stage(&job), GARMIN_DONE);

        // The password does not outlive the process it was handed to.
        CHECK_INT(job.password[0], 0);
        CHECK_INT(job.mfa_code[0], 0);
    }

    SUITE("garmin: a login with no credentials fails and says so");
    {
        GarminJob job = {0};
        CHECK(garmin_start_login(&job, "", "", ""));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(garmin_stage(&job), GARMIN_FAILED);
        CHECK_STR(garmin_message(&job), "an email address and a password are needed");
    }

    SUITE("garmin: a login Garmin wants a code for ends at MFA required");
    {
        GarminJob job = {0};
        CHECK(garmin_start_login(&job, "runner@example.com", "needs-mfa", ""));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(garmin_stage(&job), GARMIN_MFA_REQUIRED);

        // The retry is the same call with the code filled in, which is why no
        // half-finished login has to be carried between the two.
        CHECK(garmin_start_login(&job, "runner@example.com", "needs-mfa", "123456"));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(garmin_stage(&job), GARMIN_DONE);
    }

    SUITE("garmin: an import with no saved session fails");
    {
        GarminJob job = {0};
        CHECK(garmin_start_sync(&job, "NOAUTH"));
        CHECK(wait_for_job(&job, 10.0));
        CHECK_INT(garmin_stage(&job), GARMIN_FAILED);
        CHECK_STR(garmin_message(&job), "not logged in: no token files found");
    }

    SUITE("garmin: the fraction stays in range whatever the counts say");
    {
        GarminJob job = {0};
        // Nothing known yet: a bar drawn from this must not divide by zero.
        CHECK_NEAR(garmin_fraction(&job), 0.0f, 1e-6);

        atomic_store(&job.total, 4);
        atomic_store(&job.completed, 2);
        CHECK_NEAR(garmin_fraction(&job), 0.5f, 1e-6);

        // More done than there was to do is what a helper that miscounts looks
        // like, and it must not run the bar past its trough.
        atomic_store(&job.completed, 9);
        CHECK_NEAR(garmin_fraction(&job), 1.0f, 1e-6);
    }
}
