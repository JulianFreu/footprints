#include "import_job.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

// The providers. Two tables of names rather than two modules: everything below
// this point is the same work whichever of them a job was pointed at.
const ImportProvider import_garmin = {
    .name = "Garmin Connect",
    .script = GARMIN_SCRIPT,
    .session_dir = GARMIN_SESSION_DIR,
    .token_file = GARMIN_TOKEN_FILE,
    .import_subdir = GARMIN_IMPORT_SUBDIR,
};

const ImportProvider import_strava = {
    .name = "Strava",
    .script = STRAVA_SCRIPT,
    .session_dir = STRAVA_SESSION_DIR,
    .token_file = STRAVA_TOKEN_FILE,
    .import_subdir = STRAVA_IMPORT_SUBDIR,
};

// Zeroes a buffer in a way the compiler is not allowed to drop. Wiping one that
// is never read again is exactly the store an optimiser removes, and the
// buffers here worth wiping are the password and the client secret.
static void wipe(void *data, size_t size) {
    volatile unsigned char *byte = (volatile unsigned char *)data;
    while (size--)
        *byte++ = 0;
}

// --- The helper process ---

// Started with fork rather than popen: popen would carry the credentials either
// on the command line, which ps shows to everyone, or through the environment,
// which the whole process tree inherits. A pipe is read by the helper alone.
static bool child_start(ImportJob *job, char *const argv[], const char *stdin_text,
                        int *out_fd) {
    int to_child[2], from_child[2];

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
    // below reports properly.
    signal(SIGPIPE, SIG_IGN);

    if (stdin_text) {
        // Three short lines, far under a pipe buffer, so this cannot block on a
        // helper that has not started reading yet. Closing the end is what
        // stops it waiting for a fourth.
        ssize_t written = write(to_child[1], stdin_text, strlen(stdin_text));
        (void)written;
    }
    close(to_child[1]);

    *out_fd = from_child[0];
    atomic_store(&job->child_pid, (int)pid);
    return true;
}

// --- Reading what it says ---

// One line of the helper's protocol. Anything unrecognised is left for the
// terminal: a newer helper saying more than this one understands should not
// stop the import.
static void handle_line(ImportJob *job, const char *line) {
    int value;

    if (sscanf(line, "total %d", &value) == 1)
        atomic_store(&job->total, value);
    else if (sscanf(line, "progress %d", &value) == 1)
        atomic_store(&job->completed, value);
    else if (sscanf(line, "imported %d", &value) == 1)
        job->imported = value;
    else if (strncmp(line, "error ", 6) == 0)
        // The precision is spelled out rather than left to snprintf's own
        // truncation: a message longer than the panel keeps is cut on purpose,
        // and without it the compiler warns about exactly that.
        snprintf(job->message, sizeof(job->message), "%.*s",
                 (int)sizeof(job->message) - 1, line + 6);
    else if (strncmp(line, "message ", 8) == 0)
        // Something to show that is not a failure: the Strava login says here
        // what it is waiting for the browser to do. It lands in the same field
        // an error would, since the stage is what tells the two apart.
        snprintf(job->message, sizeof(job->message), "%.*s",
                 (int)sizeof(job->message) - 1, line + 8);
    else
        LOG_DEBUG("import: %s\n", line);
}

static void read_output(ImportJob *job, FILE *out) {
    char line[IMPORT_LINE_MAX];

    while (fgets(line, sizeof(line), out)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0')
            handle_line(job, line);
    }
}

// What the helper's exit code says happened. Its own message stands where it
// left one; the fallbacks are for the ways it can fail without getting far
// enough to say anything.
static ImportStage stage_from_status(ImportJob *job, int status) {
    if (!WIFEXITED(status)) {
        // Cancelling kills it, so a job asked to stop lands here and has
        // nothing to report.
        if (atomic_load(&job->cancel))
            return IMPORT_IDLE;
        snprintf(job->message, sizeof(job->message), "the import helper was killed");
        return IMPORT_FAILED;
    }

    int code = WEXITSTATUS(status);
    if (code == 0)
        return IMPORT_DONE;
    if (code == 2)
        return IMPORT_MFA_REQUIRED;

    if (job->message[0] == '\0') {
        if (code == 127)
            snprintf(job->message, sizeof(job->message), "could not run %s %s",
                     IMPORT_PYTHON, job->provider->script);
        else
            snprintf(job->message, sizeof(job->message),
                     "the import helper failed (exit %d)", code);
    }
    return IMPORT_FAILED;
}

// --- The worker ---

static void *import_worker(void *arg) {
    ImportJob *job = (ImportJob *)arg;

    // Built before the fork so the child does nothing but exec, and wiped as
    // soon as the helper has it.
    char credentials[3 * IMPORT_CREDENTIAL_MAX + 4];
    snprintf(credentials, sizeof(credentials), "%s\n%s\n%s\n",
             job->user, job->secret, job->extra);

    char *const script = (char *)job->provider->script;
    char *const session_dir = (char *)job->provider->session_dir;
    char *const login_argv[] = {(char *)IMPORT_PYTHON, script,
                                (char *)"login", session_dir, NULL};
    char *const sync_argv[] = {(char *)IMPORT_PYTHON, script,
                               (char *)"sync", session_dir,
                               job->output_dir, NULL};

    // Only the login needs credentials; an import runs off the saved token.
    int stdout_fd = -1;
    bool started = job->logging_in
                       ? child_start(job, login_argv, credentials, &stdout_fd)
                       : child_start(job, sync_argv, NULL, &stdout_fd);

    wipe(credentials, sizeof(credentials));
    wipe(job->secret, sizeof(job->secret));
    wipe(job->extra, sizeof(job->extra));

    if (!started) {
        snprintf(job->message, sizeof(job->message), "could not start the import helper");
        atomic_store(&job->stage, IMPORT_FAILED);
        atomic_store(&job->finished, true);
        return NULL;
    }

    pid_t pid = (pid_t)atomic_load(&job->child_pid);

    // A stop that arrived while the child was being started would have found no
    // pid to end, so it is checked once more now that there is one.
    if (atomic_load(&job->cancel))
        kill(pid, SIGTERM);

    FILE *out = fdopen(stdout_fd, "r");
    if (out) {
        read_output(job, out);
        fclose(out);
    } else {
        close(stdout_fd);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    atomic_store(&job->child_pid, 0);

    // Published before `finished`, so a main thread that sees the job is done
    // sees everything the worker wrote about it.
    atomic_store(&job->stage, (int)stage_from_status(job, status));
    atomic_store(&job->finished, true);
    return NULL;
}

// --- Starting and collecting ---

static bool start(ImportJob *job, bool logging_in) {
    if (job->thread_started)
        return false; // one at a time; the caller's request is dropped

    job->logging_in = logging_in;
    job->imported = 0;
    job->message[0] = '\0';

    atomic_store(&job->cancel, false);
    atomic_store(&job->finished, false);
    atomic_store(&job->child_pid, 0);
    atomic_store(&job->completed, 0);
    atomic_store(&job->total, 0);
    atomic_store(&job->stage, logging_in ? IMPORT_LOGGING_IN : IMPORT_IMPORTING);

    if (pthread_create(&job->thread, NULL, import_worker, job) != 0) {
        perror("pthread_create failed");
        snprintf(job->message, sizeof(job->message), "could not start the import");
        atomic_store(&job->stage, IMPORT_FAILED);
        return false;
    }

    job->thread_started = true;
    return true;
}

bool import_start_login(ImportJob *job, const char *user, const char *secret,
                        const char *extra) {
    if (job->thread_started)
        return false;

    snprintf(job->user, sizeof(job->user), "%s", user ? user : "");
    snprintf(job->secret, sizeof(job->secret), "%s", secret ? secret : "");
    snprintf(job->extra, sizeof(job->extra), "%s", extra ? extra : "");
    return start(job, true);
}

bool import_start_sync(ImportJob *job, const char *output_dir) {
    if (job->thread_started)
        return false;

    snprintf(job->output_dir, sizeof(job->output_dir), "%s", output_dir);
    return start(job, false);
}

bool import_busy(const ImportJob *job) {
    return job->thread_started;
}

ImportStage import_stage(const ImportJob *job) {
    return (ImportStage)atomic_load(&job->stage);
}

int import_completed(const ImportJob *job) {
    return atomic_load(&job->completed);
}

int import_total(const ImportJob *job) {
    return atomic_load(&job->total);
}

float import_fraction(const ImportJob *job) {
    int total = atomic_load(&job->total);
    if (total <= 0)
        return 0.0f;

    int completed = atomic_load(&job->completed);
    if (completed >= total)
        return 1.0f;
    return (float)completed / (float)total;
}

int import_imported(const ImportJob *job) {
    return job->imported;
}

const char *import_message(const ImportJob *job) {
    return job->message;
}

bool import_was_login(const ImportJob *job) {
    return job->logging_in;
}

bool import_collect(ImportJob *job) {
    if (!job->thread_started || !atomic_load(&job->finished))
        return false;

    pthread_join(job->thread, NULL);
    job->thread_started = false;
    atomic_store(&job->finished, false);
    return true;
}

void import_stop(ImportJob *job) {
    if (!job->thread_started)
        return;

    atomic_store(&job->cancel, true);

    // The worker is blocked reading the helper's output, so asking it to stop
    // means ending what it is reading from. Without this, quitting during an
    // import would wait for however long the download had left.
    pid_t pid = (pid_t)atomic_load(&job->child_pid);
    if (pid > 0)
        kill(pid, SIGTERM);

    pthread_join(job->thread, NULL);
    job->thread_started = false;
    atomic_store(&job->stage, IMPORT_IDLE);
}

bool import_have_session(const ImportProvider *provider) {
    char path[GPX_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", provider->session_dir, provider->token_file);
    return access(path, R_OK) == 0;
}
