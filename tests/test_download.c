/* Deterministic transfer tests: fake monotonic clock + incremental libcurl.
 * Exercise minutes of progress and silence without sleeping or a real server.
 */
#define _POSIX_C_SOURCE 200809L
/* Only production translation units use the stdio substitutions. */
#undef fwrite
#undef fclose
#undef rename
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "inkview.h"
#include <curl/curl.h>
#include "download.h"

static int failures;
#define CHECK(c, s) do { int passed = !!(c); \
    printf("  %s %s\n", passed ? "ok  " : "FAIL", s); \
    if (!passed) failures++; } while (0)

static time_t now_sec;
int dl_test_clock_gettime(clockid_t clock_id, struct timespec *ts)
{
    (void)clock_id;
    *ts = (struct timespec){ .tv_sec = now_sec };
    return 0;
}

struct stub_curl {
    size_t (*write_cb)(char *, size_t, size_t, void *);
    void *write_data;
    curl_progress_callback progress_cb;
    void *progress_data;
};
struct stub_multi { CURL *easy; CURLMsg msg; int has_msg; };
static long verify_peer, verify_host, total_timeout, connect_timeout;
static long http_status = 200;
static int send_byte, complete, online = 1, connect_calls, ready_calls, prepare_calls;
static int easy_handles, multi_handles;
static CURLcode transfer_result;
static double content_length = 1000000;

enum failure_kind { FAIL_NONE, FAIL_WRITE, FAIL_CLOSE, FAIL_RENAME, FAIL_ADD, FAIL_PERFORM };
static enum failure_kind injected_failure;
static int injected_calls;

size_t dl_test_fwrite(const void *ptr, size_t size, size_t count, FILE *fp)
{
    if (injected_failure == FAIL_WRITE) {
        injected_calls++;
        errno = ENOSPC;
        return count ? fwrite(ptr, size, count - 1, fp) : 0;
    }
    return fwrite(ptr, size, count, fp);
}

int dl_test_fclose(FILE *fp)
{
    int rc = fclose(fp);
    if (injected_failure == FAIL_CLOSE) {
        injected_calls++;
        errno = EIO;
        return EOF; /* A failed fclose still consumes the stream. */
    }
    return rc;
}

int dl_test_rename(const char *from, const char *to)
{
    if (injected_failure == FAIL_RENAME) {
        injected_calls++;
        errno = EACCES;
        return -1;
    }
    return rename(from, to);
}

CURL *curl_easy_init(void)
{
    easy_handles++;
    return calloc(1, sizeof(struct stub_curl));
}
CURLcode curl_easy_setopt(CURL *c, int opt, ...)
{
    struct stub_curl *s = c;
    va_list ap;
    va_start(ap, opt);
    switch (opt) {
    case CURLOPT_WRITEFUNCTION: s->write_cb = va_arg(ap, size_t (*)(char *, size_t, size_t, void *)); break;
    case CURLOPT_WRITEDATA: s->write_data = va_arg(ap, void *); break;
    case CURLOPT_PROGRESSFUNCTION: s->progress_cb = va_arg(ap, curl_progress_callback); break;
    case CURLOPT_PROGRESSDATA: s->progress_data = va_arg(ap, void *); break;
    case CURLOPT_SSL_VERIFYPEER: verify_peer = va_arg(ap, long); break;
    case CURLOPT_SSL_VERIFYHOST: verify_host = va_arg(ap, long); break;
    case CURLOPT_TIMEOUT: total_timeout = va_arg(ap, long); break;
    case CURLOPT_CONNECTTIMEOUT: connect_timeout = va_arg(ap, long); break;
    default: break;
    }
    va_end(ap);
    return CURLE_OK;
}
CURLcode curl_easy_perform(CURL *c)
{
    (void)c;
    CHECK(0, "book downloads must not call blocking curl_easy_perform");
    return CURLE_OK;
}
CURLcode curl_easy_getinfo(CURL *c, int info, ...)
{
    (void)c;
    va_list ap;
    va_start(ap, info);
    if (info == CURLINFO_RESPONSE_CODE) *va_arg(ap, long *) = http_status;
    va_end(ap);
    return CURLE_OK;
}
void curl_easy_cleanup(CURL *c) { easy_handles--; free(c); }
const char *curl_easy_strerror(CURLcode rc) { (void)rc; return "stub transfer error"; }
curl_version_info_data *curl_version_info(int v)
{
    (void)v;
    static curl_version_info_data d = { "stub", CURL_VERSION_SSL, "NSS/3.30.2" };
    return &d;
}
CURLM *curl_multi_init(void) { multi_handles++; return calloc(1, sizeof(struct stub_multi)); }
CURLMcode curl_multi_add_handle(CURLM *m, CURL *c)
{
    if (injected_failure == FAIL_ADD) {
        injected_calls++;
        return CURLM_INTERNAL_ERROR;
    }
    ((struct stub_multi *)m)->easy = c;
    return CURLM_OK;
}
CURLMcode curl_multi_remove_handle(CURLM *m, CURL *c)
{
    CHECK(((struct stub_multi *)m)->easy == c, "active transfer detached on cleanup");
    ((struct stub_multi *)m)->easy = NULL;
    return CURLM_OK;
}
CURLMcode curl_multi_cleanup(CURLM *m)
{
    CHECK(((struct stub_multi *)m)->easy == NULL, "multi cleanup has no attached transfer");
    multi_handles--;
    free(m);
    return CURLM_OK;
}
const char *curl_multi_strerror(CURLMcode rc) { (void)rc; return "stub multi error"; }
CURLMcode curl_multi_perform(CURLM *multi, int *running)
{
    struct stub_multi *m = multi;
    struct stub_curl *s = m->easy;
    if (injected_failure == FAIL_PERFORM) {
        injected_calls++;
        return CURLM_INTERNAL_ERROR;
    }
    CURLcode result = transfer_result;
    if (send_byte) {
        char byte = 'x';
        if (s->write_cb(&byte, 1, 1, s->write_data) != 1)
            result = CURLE_WRITE_ERROR;
    }
    /* Deliberately unchanged percentage despite new bytes. */
    s->progress_cb(s->progress_data, content_length, 1, 0, 0);
    *running = !complete && result == CURLE_OK;
    if (!*running) {
        m->msg.msg = CURLMSG_DONE;
        m->msg.easy_handle = m->easy;
        m->msg.data.result = result;
        m->has_msg = 1;
    }
    return CURLM_OK;
}
CURLMsg *curl_multi_info_read(CURLM *multi, int *remaining)
{
    struct stub_multi *m = multi;
    *remaining = 0;
    if (!m->has_msg) return NULL;
    m->has_msg = 0;
    return &m->msg;
}
void BookPreparing(const char *path) { (void)path; prepare_calls++; }
void BookReady(const char *path) { (void)path; ready_calls++; }
int NetConnect(const char *name) { (void)name; connect_calls++; return 0; }
int QueryNetwork(void) { return online ? NET_CONNECTED : 0; }

static int file_equals(const char *path, const char *expected)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf, fp);
    int ok = !ferror(fp) && n == strlen(expected) && memcmp(buf, expected, n) == 0;
    fclose(fp);
    return ok;
}

static void reset_environment(void)
{
    CHECK(!easy_handles && !multi_handles, "previous scenario released curl handles");
    now_sec = 0;
    verify_peer = verify_host = total_timeout = connect_timeout = -1;
    http_status = 200;
    send_byte = complete = 0;
    online = 1;
    connect_calls = ready_calls = prepare_calls = 0;
    transfer_result = CURLE_OK;
    content_length = 1000000;
    injected_failure = FAIL_NONE;
    injected_calls = 0;
}
static void test_failure_recovery(const char *final, const char *tmp)
{
    const char *names[] = { "", "write failure", "close failure", "rename failure",
                            "multi add failure", "multi perform failure" };
    const char *errors[] = { "", "stub transfer error", strerror(EIO), strerror(EACCES),
                             "Cannot start download", "stub multi error" };
    for (int kind = FAIL_WRITE; kind <= FAIL_PERFORM; kind++) {
        reset_environment();
        printf("%s and recovery:\n", names[kind]);
        FILE *fp = fopen(final, "wb");
        CHECK(fp != NULL, "existing book fixture opens");
        if (!fp) return;
        fputs("original book", fp);
        fclose(fp);
        online = send_byte = 1;
        complete = kind != FAIL_WRITE; /* A short write must abort by itself. */
        transfer_result = CURLE_OK;
        injected_calls = 0;
        injected_failure = (enum failure_kind)kind;
        int ready_before = ready_calls;
        char out[DL_PATH_MAX], err[DL_ERR_MAX];
        download_job *job = download_start("https://example.org/book.epub", "Test",
                                            "application/epub+zip", err);
        CHECK(job != NULL, "failure scenario starts a job");
        if (!job) { injected_failure = FAIL_NONE; continue; }
        CHECK(download_step(job, out, err) == -1 && strstr(err, errors[kind]) && !out[0],
              "injected failure returns an error without a saved path");
        CHECK(injected_calls == 1, "intended failure was exercised exactly once");
        download_destroy(job);
        injected_failure = FAIL_NONE;
        CHECK(!easy_handles && !multi_handles, "failure releases every curl handle");
        CHECK(access(tmp, F_OK) != 0, "destroy removes the failed partial download");
        CHECK(file_equals(final, "original book"), "failure preserves the existing book byte for byte");
        CHECK(ready_calls == ready_before, "failure never announces a ready book");

        complete = 1;
        job = download_start("https://example.org/book.epub", "Test",
                             "application/epub+zip", err);
        CHECK(job != NULL, "retry starts after failure");
        if (!job) continue;
        CHECK(download_step(job, out, err) == 0 && !err[0] && strcmp(out, final) == 0,
              "retry succeeds and returns the saved path");
        download_destroy(job);
        CHECK(file_equals(final, "x") && ready_calls == ready_before + 1,
              "retry saves fresh bytes and announces the book once");
        CHECK(!easy_handles && !multi_handles && access(tmp, F_OK) != 0,
              "retry leaves no handles or partial file");
        unlink(final);
    }
}

static download_job *start_download(char err[DL_ERR_MAX])
{
    download_job *job = download_start("https://example.org/book.epub", "Test",
                                      "application/epub+zip", err);
    CHECK(job != NULL, "download job created");
    return job;
}
static void test_slow_download(const char *final, const char *tmp)
{
    char out[DL_PATH_MAX], err[DL_ERR_MAX];
    download_job *job;
    reset_environment();
    printf("incremental book downloads:\n");
    job = start_download(err);
    if (!job) return;
    CHECK(job != NULL, "download starts without waiting for completion");
    CHECK(total_timeout == 0 && connect_timeout == 15, "no total deadline; connection timeout retained");
    CHECK(verify_peer == 0 && verify_host == 0, "shared firmware TLS settings retained");
    send_byte = 1;
    int rc = 1;
    for (int i = 0; i < 10; i++) {
        now_sec += 30;
        rc = download_step(job, out, err);
        if (rc != 1) break;
    }
    CHECK(rc == 1 && now_sec == 300 && download_percent(job) == 0,
          "five-minute transfer survives while bytes arrive, even at unchanged 0%");
    complete = 1;
    send_byte = 0;
    CHECK(download_step(job, out, err) == 0 && strcmp(out, final) == 0,
          "slow download completes and returns the saved path");
    CHECK(prepare_calls == 1 && ready_calls == 1 && access(tmp, F_OK) != 0,
          "only a completed book is announced and .part is renamed");
    download_destroy(job);
    FILE *fp = fopen(final, "rb");
    CHECK(fp != NULL, "destroying the completed job preserves the book");
    if (fp) { fseek(fp, 0, SEEK_END); CHECK(ftell(fp) == 10, "every received byte is saved"); fclose(fp); }
    unlink(final);

}

static void test_idle_timeout(const char *final, const char *tmp)
{
    char out[DL_PATH_MAX], err[DL_ERR_MAX];
    download_job *job;
    reset_environment();
    content_length = 0; /* A server streaming without Content-Length. */
    complete = 0;
    send_byte = 1;
    job = start_download(err);
    if (!job) return;
    download_step(job, out, err);
    now_sec += 45;
    CHECK(download_step(job, out, err) == 1 && download_percent(job) == 0,
          "unknown-length response accepts bytes without a percentage update");
    send_byte = 0;
    now_sec += 59;
    CHECK(download_step(job, out, err) == 1, "59 seconds without new bytes is allowed");
    now_sec++;
    CHECK(download_step(job, out, err) == -1 && strstr(err, "no data"),
          "progress callbacks without bytes do not reset the 60-second idle timer");
    download_destroy(job);
    CHECK(access(tmp, F_OK) != 0 && access(final, F_OK) != 0 && ready_calls == 0,
          "idle timeout removes partial file without announcing a book");

}

static void test_cancel_and_retry(const char *final, const char *tmp)
{
    char out[DL_PATH_MAX], err[DL_ERR_MAX];
    download_job *job;
    reset_environment();
    job = start_download(err);
    if (!job) return;
    send_byte = 1;
    download_step(job, out, err);
    download_destroy(job); /* Same path as lock/sleep/Back/exit. */
    CHECK(access(tmp, F_OK) != 0 && access(final, F_OK) != 0 && ready_calls == 0,
          "cancellation immediately removes the unfinished book");
    job = start_download(err);
    if (!job) return;
    complete = 1;
    CHECK(download_step(job, out, err) == 0, "a fresh download after cancellation succeeds");
    download_destroy(job);
    FILE *fp = fopen(final, "rb");
    if (fp) { fseek(fp, 0, SEEK_END); CHECK(ftell(fp) == 1, "retry starts from zero, without resuming old bytes"); fclose(fp); }
    unlink(final);

}

static void test_response_errors(const char *final, const char *tmp)
{
    char out[DL_PATH_MAX], err[DL_ERR_MAX];
    download_job *job;
    reset_environment();
    (void)final; (void)tmp;
    complete = send_byte = 1;
    http_status = 503;
    job = start_download(err);
    if (!job) return;
    CHECK(download_step(job, out, err) == -1 && strstr(err, "HTTP 503"), "HTTP failures are reported");
    download_destroy(job);
    http_status = 200;
    send_byte = 0;
    job = start_download(err);
    if (!job) return;
    CHECK(download_step(job, out, err) == -1 && strstr(err, "Empty"), "empty responses are rejected");
    download_destroy(job);
    transfer_result = CURLE_RECV_ERROR;
    job = start_download(err);
    if (!job) return;
    CHECK(download_step(job, out, err) == -1 && strstr(err, "transfer error"), "network failures are reported");
    download_destroy(job);
    CHECK(ready_calls == 0 && prepare_calls == 0, "failed transfers never announce a book");

}

static void test_wifi_timeout(const char *final, const char *tmp)
{
    char out[DL_PATH_MAX], err[DL_ERR_MAX];
    download_job *job;
    reset_environment();
    (void)final;
    online = 0;
    job = start_download(err);
    if (!job) return;
    CHECK(connect_calls == 1 && download_step(job, out, err) == 1, "WiFi wake-up is polled without blocking");
    now_sec += 4;
    CHECK(download_step(job, out, err) == -1 && strstr(err, "No network"), "WiFi wake-up has a bounded wait");
    download_destroy(job);
    CHECK(!easy_handles && !multi_handles && access(tmp, F_OK) != 0,
          "all transfer handles and temporary files are released");
    job = start_download(err);
    if (!job) return;
    download_destroy(job);
    CHECK(!easy_handles && !multi_handles && access(tmp, F_OK) != 0,
          "cancelling during WiFi wake-up also releases the pending job");
}

int main(void)
{
    char dir[] = "/tmp/inkshelf-dl-test.XXXXXX";
    if (!mkdtemp(dir)) return 1;
    setenv("INKSHELF_LIBDIR", dir, 1);
    char tmp[DL_PATH_MAX], final[DL_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/.Test.epub.part", dir);
    snprintf(final, sizeof final, "%s/Test.epub", dir);
    test_slow_download(final, tmp);
    test_idle_timeout(final, tmp);
    test_cancel_and_retry(final, tmp);
    test_response_errors(final, tmp);
    test_wifi_timeout(final, tmp);
    test_failure_recovery(final, tmp);
    rmdir(dir);
    return failures ? 1 : 0;
}
