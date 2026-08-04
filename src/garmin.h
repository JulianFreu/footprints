#ifndef GARMIN_H
#define GARMIN_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "config.h"

// The Garmin Connect import: runs garmin_sync.py, feeds it credentials and
// reads its progress back a line at a time.
//
// A job of its own rather than a BackgroundJob, for two reasons. It never
// touches the collection -- it only writes files, and reading them is the
// ordinary library load that follows -- so it has no use for that module's
// ownership rule. And the frame loop replaces every panel with the progress
// panel while a BackgroundJob runs, which would take away the panel this job
// reports into.
//
// The stage a finished job ended on stays until the next one starts, so the
// panel can go on showing what happened.

typedef enum GarminStage {
    GARMIN_IDLE,
    GARMIN_LOGGING_IN,
    GARMIN_IMPORTING,
    GARMIN_DONE,
    GARMIN_MFA_REQUIRED,
    GARMIN_FAILED,
} GarminStage;

typedef struct GarminJob {
    pthread_t thread;
    bool thread_started; // main thread only

    _Atomic int stage;     // GarminStage
    _Atomic int completed; // activities dealt with
    _Atomic int total;     // activities to deal with, 0 while unknown
    _Atomic bool cancel;
    _Atomic bool finished; // worker is done; main thread has yet to join
    // The helper, so that asking a worker blocked on its output to stop can end
    // what it is blocked on. Zero when there is no child.
    _Atomic int child_pid;

    // Written by the worker before `finished` is published and read by the main
    // thread after it, which is what makes them safe without a lock of their own.
    char message[GARMIN_MESSAGE_MAX];
    int imported;

    // Handed over at the start of a job and wiped as soon as the helper has
    // them. The password is never written anywhere else.
    char email[GARMIN_CREDENTIAL_MAX];
    char password[GARMIN_CREDENTIAL_MAX];
    char mfa_code[GARMIN_CREDENTIAL_MAX];

    char output_dir[GPX_PATH_MAX];
    bool logging_in;
} GarminJob;

// Mints an OAuth token from an email and a password and saves it, so no later
// import needs either. `mfa_code` may be empty: when Garmin asks for one the
// job ends at GARMIN_MFA_REQUIRED, and the same call is made again with it.
bool garmin_start_login(GarminJob *job, const char *email, const char *password,
                        const char *mfa_code);

// Downloads every activity not already in `output_dir`.
bool garmin_start_sync(GarminJob *job, const char *output_dir);

// True while a job is running. One at a time; a second request is dropped.
bool garmin_busy(const GarminJob *job);

GarminStage garmin_stage(const GarminJob *job);
// What to show while it runs: 0..1 through the import, and the two counts it is
// made of. A total of zero means the helper has not said how much there is yet.
float garmin_fraction(const GarminJob *job);
int garmin_completed(const GarminJob *job);
int garmin_total(const GarminJob *job);

// How many activities landed on disk, and whatever the helper last had to say.
int garmin_imported(const GarminJob *job);
const char *garmin_message(const GarminJob *job);

// Whether the last job was a login rather than an import. The two finish the
// same way and have nothing to say in common, so what to report about a job
// that succeeded depends on which it was.
bool garmin_was_login(const GarminJob *job);

// Main thread: if the worker has finished, joins it. Returns true exactly once
// per job, on the frame the result lands.
bool garmin_collect(GarminJob *job);

// Asks a running job to stop and waits for it. Safe to call when idle.
void garmin_stop(GarminJob *job);

// Whether a token has already been minted. This is what says an import can be
// run without asking for a password again.
bool garmin_have_session(void);

#endif
