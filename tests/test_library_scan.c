#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "library_scan.h"

static int failures;
#define CHECK(c, s) do { int passed = !!(c); \
    printf("  %s %s\n", passed ? "ok  " : "FAIL", s); \
    if (!passed) failures++; } while (0)

static void delay(void)
{
    struct timespec ts = {0, 10000000};
    nanosleep(&ts, NULL);
}

static long size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : 0;
}

int main(void)
{
    char dir[] = "/tmp/inkshelf-scan-test.XXXXXX";
    if (!mkdtemp(dir)) return 1;
    char scanner[256], lock[256], log[256], counter[256];
    snprintf(scanner, sizeof scanner, "%s/scanner", dir);
    snprintf(lock, sizeof lock, "%s/lock", dir);
    snprintf(log, sizeof log, "%s/log", dir);
    snprintf(counter, sizeof counter, "%s/count", dir);
    FILE *fp = fopen(scanner, "w");
    if (!fp) return 1;
    fprintf(fp, "#!/bin/sh\nprintf x >> '%s'\nsleep 0.2\n"
                "printf 'Scan finished\\n'\nexec sleep 30\n", counter);
    fclose(fp);
    chmod(scanner, 0700);

    struct timespec first, during;
    clock_gettime(CLOCK_MONOTONIC, &first);
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0)
        _exit(library_scan_run(scanner, lock, log, &first, 0, 3000) == 0 ? 0 : 1);
    for (int i = 0; i < 300 && size(counter) == 0; i++) delay();
    CHECK(size(counter) == 1, "supervisor starts the firmware scanner");
    clock_gettime(CLOCK_MONOTONIC, &during);
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "completion marker dismisses and reaps a persistent scanner");

    int rc = library_scan_run(scanner, lock, log, &first, 0, 3000);
    CHECK(rc == 0 && size(counter) == 1, "requests covered by a scan are coalesced");
    rc = library_scan_run(scanner, lock, log, &during, 0, 3000);
    CHECK(rc == 0 && size(counter) == 2,
          "a book arriving during scanning triggers another pass");

    struct timespec later;
    clock_gettime(CLOCK_MONOTONIC, &later);
    rc = library_scan_run("/nonexistent/inkshelf-scanner", lock, log, &later, 0, 3000);
    CHECK(rc == -1, "missing scanner is reported as a failure");
    rc = library_scan_run(scanner, lock, log, &later, 0, 3000);
    CHECK(rc == 0 && size(counter) == 3, "failed scans do not consume a request");

    fp = fopen(scanner, "w");
    if (!fp) return 1;
    fputs("#!/bin/sh\nexit 0\n", fp);
    fclose(fp);
    clock_gettime(CLOCK_MONOTONIC, &later);
    rc = library_scan_run(scanner, lock, log, &later, 0, 3000);
    CHECK(rc == -1, "a clean exit alone does not prove indexing completed");

    fp = fopen(scanner, "w");
    if (!fp) return 1;
    fputs("#!/bin/sh\nexec sleep 30\n", fp);
    fclose(fp);
    clock_gettime(CLOCK_MONOTONIC, &later);
    rc = library_scan_run(scanner, lock, log, &later, 0, 50);
    CHECK(rc == -1, "a hung scanner is stopped and reported as a timeout");
    CHECK(waitpid(-1, &status, WNOHANG) == -1, "no scanner children remain");
    unlink(scanner); unlink(lock); unlink(log); unlink(counter); rmdir(dir);
    return failures ? 1 : 0;
}
