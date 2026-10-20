/* BookReady does not start indexing on firmware 6.8. Run the firmware scanner
 * in a separate supervisor process, without blocking the app or its HTTP
 * worker. Only a scanner child started here may be terminated here. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "library_scan.h"

extern char **environ;

#define SCAN_MODE "--inkshelf-library-scan"
#define SCAN_LOCK "/tmp/inkshelf-library-scan.lock"
#define SCAN_LOG  "/mnt/ext1/inkshelf-library-scan.log"

static void pause_ms(int ms)
{
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static int at_or_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec < b->tv_sec ||
           (a->tv_sec == b->tv_sec && a->tv_nsec <= b->tv_nsec);
}

/* The vendor scanner can remain alive after finishing. Its completion line is
 * used by other PocketBook sync tools too. Keep an overlap between reads so a
 * marker split at a buffer boundary is still recognised. */
static int finished(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[4096];
    size_t keep = 0, n;
    int found = 0;
    while ((n = fread(buf + keep, 1, sizeof buf - keep - 1, fp)) > 0) {
        n += keep;
        buf[n] = '\0';
        if (strstr(buf, "Scan finished")) { found = 1; break; }
        keep = n < 32 ? n : 32;
        memmove(buf, buf + n - keep, keep);
    }
    fclose(fp);
    return found;
}

static void log_result(const char *path, const char *message, int result)
{
    FILE *fp = fopen(path, "a");
    if (fp) {
        fprintf(fp, "\ninkshelf: %s (%d)\n", message, result);
        fclose(fp);
    }
}

static int reap(pid_t pid, int *status, int flags)
{
    int rc;
    do { rc = waitpid(pid, status, flags); } while (rc < 0 && errno == EINTR);
    return rc;
}

static void stop_child(pid_t pid)
{
    int status;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (reap(pid, &status, WNOHANG) != 0) return;
        pause_ms(100);
    }
    kill(pid, SIGKILL);
    reap(pid, &status, 0);
}

int library_scan_run(const char *scanner, const char *lock_path,
                     const char *log_path, const struct timespec *requested,
                     int debounce_ms, int timeout_ms)
{
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (lock_fd < 0) return -1;
    fcntl(lock_fd, F_SETFD, FD_CLOEXEC);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int rc;
    do { rc = fcntl(lock_fd, F_SETLKW, &lock); } while (rc < 0 && errno == EINTR);
    if (rc < 0) { close(lock_fd); return -1; }

    /* Serialise supervisors, and skip requests covered by a successful scan
     * that STARTED after their book landed. Requests during a scan must get
     * another pass. /tmp resets this monotonic-clock stamp on device reboot. */
    char stamp[96] = {0};
    long long sec;
    long ns;
    if (pread(lock_fd, stamp, sizeof stamp - 1, 0) > 0 &&
        sscanf(stamp, "%lld %ld", &sec, &ns) == 2) {
        struct timespec previous = {(time_t)sec, ns};
        if (at_or_before(requested, &previous)) { close(lock_fd); return 0; }
    }
    pause_ms(debounce_ms);
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);

    posix_spawn_file_actions_t actions;
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) { close(lock_fd); return -1; }
    rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path,
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (rc == 0)
        rc = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (rc == 0)
        rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                             "/dev/null", O_RDONLY, 0);
    pid_t pid;
    char *args[] = {(char *)scanner, NULL};
    if (rc == 0) rc = posix_spawn(&pid, scanner, &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        log_result(log_path, "could not start firmware scanner", rc);
        close(lock_fd);
        return -1;
    }

    int ok = 0, status = 0;
    for (;;) {
        int waited = reap(pid, &status, WNOHANG);
        if (waited == pid) {
            ok = finished(log_path);
            log_result(log_path, ok ? "scanner finished and exited" :
                       "scanner exited without completion marker", status);
            break;
        }
        if (waited < 0) { log_result(log_path, "waitpid failed", errno); break; }
        if (finished(log_path)) {
            ok = 1;
            stop_child(pid);
            log_result(log_path, "scan complete; dismissed scanner", 0);
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed = (now.tv_sec - started.tv_sec) * 1000LL +
                            (now.tv_nsec - started.tv_nsec) / 1000000;
        if (elapsed >= timeout_ms) {
            stop_child(pid);
            log_result(log_path, "scanner timeout", timeout_ms);
            break;
        }
        pause_ms(100);
    }
    if (ok) {
        int n = snprintf(stamp, sizeof stamp, "%lld %ld\n",
                         (long long)started.tv_sec, started.tv_nsec);
        if (pwrite(lock_fd, stamp, (size_t)n, 0) == n) ftruncate(lock_fd, n);
    }
    close(lock_fd);
    return ok ? 0 : -1;
}

int library_scan_request(void)
{
    struct timespec requested;
    clock_gettime(CLOCK_MONOTONIC, &requested);
    char sec[32], ns[32];
    snprintf(sec, sizeof sec, "%lld", (long long)requested.tv_sec);
    snprintf(ns, sizeof ns, "%ld", requested.tv_nsec);
    char *args[] = {"inkshelf.app", SCAN_MODE, sec, ns, NULL};
    pid_t pid;
    /* Re-exec before forking: the new process has no InkView/HTTP threads or
     * inherited userspace locks. The short bootstrap exits immediately. */
    int rc = posix_spawn(&pid, "/proc/self/exe", NULL, NULL, args, environ);
    if (rc != 0) { log_result(SCAN_LOG, "could not start supervisor", rc); return -1; }
    int status;
    if (reap(pid, &status, 0) != pid) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    log_result(SCAN_LOG, "supervisor bootstrap failed", status);
    return -1;
}

int library_scan_handle_args(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], SCAN_MODE) != 0) return -1;
    if (argc != 4) return 1;
    struct timespec requested;
    char *end;
    requested.tv_sec = (time_t)strtoll(argv[2], &end, 10);
    if (!argv[2][0] || *end || requested.tv_sec < 0) return 1;
    requested.tv_nsec = strtol(argv[3], &end, 10);
    if (!argv[3][0] || *end || requested.tv_nsec < 0 || requested.tv_nsec >= 1000000000L)
        return 1;
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid > 0) return 0;
    setsid();
    /* InkView may have installed/ignored signals or blocked them. This new
     * supervisor must be able to reap and dismiss its own scanner child. */
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    /* Do not retain the HTTP listening/client sockets or app output pipes. */
    DIR *fds = opendir("/proc/self/fd");
    if (fds) {
        struct dirent *entry;
        while ((entry = readdir(fds))) {
            char *tail;
            long fd = strtol(entry->d_name, &tail, 10);
            if (entry->d_name[0] && !*tail && fd != dirfd(fds)) close((int)fd);
        }
        closedir(fds);
    } else {
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0) maxfd = 1024;
        for (long fd = 0; fd < maxfd; fd++) close((int)fd);
    }
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
        dup2(nullfd, STDIN_FILENO);
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
        if (nullfd > STDERR_FILENO) close(nullfd);
    }
    const char *scanner = access("/ebrmain/bin/scanner.app", X_OK) == 0
                        ? "/ebrmain/bin/scanner.app" : "/ebrmain/cramfs/bin/scanner.app";
    _exit(library_scan_run(scanner, SCAN_LOCK, SCAN_LOG, &requested, 2000, 120000) == 0 ? 0 : 1);
}
