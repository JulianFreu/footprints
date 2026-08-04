#include "garmin.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

// Zeroes a buffer in a way the compiler is not allowed to drop. Wiping one that
// is never read again is exactly the store an optimiser removes, and the one
// buffer here worth wiping is the password.
static void wipe(void *data, size_t size) {
    volatile unsigned char *byte = (volatile unsigned char *)data;
    while (size--)
        *byte++ = 0;
}

// --- The helper process ---

// Started with fork rather than popen: popen would carry the password either on
// the command line, which ps shows to everyone, or through the environment,
// which the whole process tree inherits. A pipe is read by the helper alone.
static bool child_start(GarminJob *job, char *const argv[], const char *stdin_text,
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
static void handle_line(GarminJob *job, const char *line) {
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
    else
        LOG_DEBUG("garmin: %s\n", line);
}

static void read_output(GarminJob *job, FILE *out) {
    char line[GARMIN_LINE_MAX];

    while (fgets(line, sizeof(line), out)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0')
            handle_line(job, line);
    }
}

// What the helper's exit code says happened. Its own message stands where it
// left one; the fallbacks are for the ways it can fail without getting far
// enough to say anything.
static GarminStage stage_from_status(GarminJob *job, int status) {
    if (!WIFEXITED(status)) {
        // Cancelling kills it, so a job asked to stop lands here and has
        // nothing to report.
        if (atomic_load(&job->cancel))
            return GARMIN_IDLE;
        snprintf(job->message, sizeof(job->message), "the import helper was killed");
        return GARMIN_FAILED;
    }

    int code = WEXITSTATUS(status);
    if (code == 0)
        return GARMIN_DONE;
    if (code == 2)
        return GARMIN_MFA_REQUIRED;

    if (job->message[0] == '\0') {
        if (code == 127)
            snprintf(job->message, sizeof(job->message),
                     "could not run " GARMIN_PYTHON " " GARMIN_SCRIPT);
        else
            snprintf(job->message, sizeof(job->message),
                     "the import helper failed (exit %d)", code);
    }
    return GARMIN_FAILED;
}

// --- The worker ---

static void *garmin_worker(void *arg) {
    GarminJob *job = (GarminJob *)arg;

    // Built before the fork so the child does nothing but exec, and wiped as
    // soon as the helper has it.
    char credentials[3 * GARMIN_CREDENTIAL_MAX + 4];
    snprintf(credentials, sizeof(credentials), "%s\n%s\n%s\n",
             job->email, job->password, job->mfa_code);

    char *const login_argv[] = {(char *)GARMIN_PYTHON, (char *)GARMIN_SCRIPT,
                                (char *)"login", (char *)GARMIN_SESSION_DIR, NULL};
    char *const sync_argv[] = {(char *)GARMIN_PYTHON, (char *)GARMIN_SCRIPT,
                               (char *)"sync", (char *)GARMIN_SESSION_DIR,
                               job->output_dir, NULL};

    // Only the login needs credentials; an import runs off the saved token.
    int stdout_fd = -1;
    bool started = job->logging_in
                       ? child_start(job, login_argv, credentials, &stdout_fd)
                       : child_start(job, sync_argv, NULL, &stdout_fd);

    wipe(credentials, sizeof(credentials));
    wipe(job->password, sizeof(job->password));
    wipe(job->mfa_code, sizeof(job->mfa_code));

    if (!started) {
        snprintf(job->message, sizeof(job->message), "could not start the import helper");
        atomic_store(&job->stage, GARMIN_FAILED);
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

static bool start(GarminJob *job, bool logging_in) {
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
    atomic_store(&job->stage, logging_in ? GARMIN_LOGGING_IN : GARMIN_IMPORTING);

    if (pthread_create(&job->thread, NULL, garmin_worker, job) != 0) {
        perror("pthread_create failed");
        snprintf(job->message, sizeof(job->message), "could not start the import");
        atomic_store(&job->stage, GARMIN_FAILED);
        return false;
    }

    job->thread_started = true;
    return true;
}

bool garmin_start_login(GarminJob *job, const char *email, const char *password,
                        const char *mfa_code) {
    if (job->thread_started)
        return false;

    snprintf(job->email, sizeof(job->email), "%s", email ? email : "");
    snprintf(job->password, sizeof(job->password), "%s", password ? password : "");
    snprintf(job->mfa_code, sizeof(job->mfa_code), "%s", mfa_code ? mfa_code : "");
    return start(job, true);
}

bool garmin_start_sync(GarminJob *job, const char *output_dir) {
    if (job->thread_started)
        return false;

    snprintf(job->output_dir, sizeof(job->output_dir), "%s", output_dir);
    return start(job, false);
}

bool garmin_busy(const GarminJob *job) {
    return job->thread_started;
}

GarminStage garmin_stage(const GarminJob *job) {
    return (GarminStage)atomic_load(&job->stage);
}

int garmin_completed(const GarminJob *job) {
    return atomic_load(&job->completed);
}

int garmin_total(const GarminJob *job) {
    return atomic_load(&job->total);
}

float garmin_fraction(const GarminJob *job) {
    int total = atomic_load(&job->total);
    if (total <= 0)
        return 0.0f;

    int completed = atomic_load(&job->completed);
    if (completed >= total)
        return 1.0f;
    return (float)completed / (float)total;
}

int garmin_imported(const GarminJob *job) {
    return job->imported;
}

const char *garmin_message(const GarminJob *job) {
    return job->message;
}

bool garmin_was_login(const GarminJob *job) {
    return job->logging_in;
}

bool garmin_collect(GarminJob *job) {
    if (!job->thread_started || !atomic_load(&job->finished))
        return false;

    pthread_join(job->thread, NULL);
    job->thread_started = false;
    atomic_store(&job->finished, false);
    return true;
}

void garmin_stop(GarminJob *job) {
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
    atomic_store(&job->stage, GARMIN_IDLE);
}

bool garmin_have_session(void) {
    char path[GPX_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", GARMIN_SESSION_DIR, GARMIN_TOKEN_FILE);
    return access(path, R_OK) == 0;
}
