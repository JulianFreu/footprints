#ifndef IMPORT_JOB_H
#define IMPORT_JOB_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

// An activity import: runs a provider's helper script, feeds it credentials and
// reads its progress back a line at a time.
//
// Garmin Connect and Strava differ only in which script is run and where its
// token is kept, so both are the same job with a different ImportProvider. The
// line protocol, the exit codes and the argument list are the helper's side of
// that contract; see garmin_sync.py and strava_sync.py.
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

typedef enum ImportStage {
    IMPORT_IDLE,
    IMPORT_LOGGING_IN,
    IMPORT_IMPORTING,
    IMPORT_DONE,
    IMPORT_MFA_REQUIRED,
    IMPORT_FAILED,
} ImportStage;

// Everything that differs between one provider and the next. The two instances
// below are all there are; a job points at one for its lifetime.
typedef struct ImportProvider {
    const char *name;          // as the panel spells it
    const char *script;        // the helper run for it
    const char *session_dir;   // where that helper keeps its token
    const char *token_file;    // the file inside it that says there is one
    const char *import_subdir; // under the library folder, where the GPX lands
} ImportProvider;

extern const ImportProvider import_garmin;
extern const ImportProvider import_strava;

typedef struct ImportJob {
    // Which provider this job runs for, set once where the job is declared and
    // never changed. A job without one has nothing to run.
    const ImportProvider *provider;

    pthread_t thread;
    bool thread_started; // main thread only

    _Atomic int stage;     // ImportStage
    _Atomic int completed; // activities dealt with
    _Atomic int total;     // activities to deal with, 0 while unknown
    _Atomic bool cancel;
    _Atomic bool finished; // worker is done; main thread has yet to join
    // The helper, so that asking a worker blocked on its output to stop can end
    // what it is blocked on. Zero when there is no child. Wide enough for a pid
    // or a Windows process handle, since which one it holds is subprocess.c's
    // business rather than this module's.
    _Atomic uintptr_t child_id;

    // Written by the worker before `finished` is published and read by the main
    // thread after it, which is what makes them safe without a lock of their own.
    char message[IMPORT_MESSAGE_MAX];
    int imported;

    // Handed over at the start of a job and wiped as soon as the helper has
    // them. The secret of the pair is never written anywhere else.
    char user[IMPORT_CREDENTIAL_MAX];
    char secret[IMPORT_CREDENTIAL_MAX];
    char extra[IMPORT_CREDENTIAL_MAX];

    char output_dir[GPX_PATH_MAX];
    bool logging_in;
} ImportJob;

// Mints a token from the credentials and saves it, so no later import needs
// them. What the three stand for is the provider's business: Garmin takes an
// email, a password and an MFA code, Strava an application id and secret.
// `extra` may be empty: when Garmin asks for a code the job ends at
// IMPORT_MFA_REQUIRED, and the same call is made again with it.
bool import_start_login(ImportJob *job, const char *user, const char *secret,
                        const char *extra);

// Downloads every activity not already in `output_dir`.
bool import_start_sync(ImportJob *job, const char *output_dir);

// True while a job is running. One at a time per provider; a second request is
// dropped.
bool import_busy(const ImportJob *job);

ImportStage import_stage(const ImportJob *job);
// What to show while it runs: 0..1 through the import, and the two counts it is
// made of. A total of zero means the helper has not said how much there is yet.
float import_fraction(const ImportJob *job);
int import_completed(const ImportJob *job);
int import_total(const ImportJob *job);

// How many activities landed on disk, and whatever the helper last had to say.
int import_imported(const ImportJob *job);
const char *import_message(const ImportJob *job);

// Whether the last job was a login rather than an import. The two finish the
// same way and have nothing to say in common, so what to report about a job
// that succeeded depends on which it was.
bool import_was_login(const ImportJob *job);

// Main thread: if the worker has finished, joins it. Returns true exactly once
// per job, on the frame the result lands.
bool import_collect(ImportJob *job);

// Asks a running job to stop and waits for it. Safe to call when idle.
void import_stop(ImportJob *job);

// Whether a token has already been minted for `provider`. This is what says an
// import can be run without asking for credentials again.
bool import_have_session(const ImportProvider *provider);

#endif
