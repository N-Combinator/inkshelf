/* Non-blocking OPDS book transfer, driven by the InkView UI timer. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include "inkview.h"
#include "download.h"
#include "http.h"
#include "library.h"
#include "library_scan.h"
#include "net.h"

struct download_job {
    CURL *curl;
    CURLM *multi;
    FILE *fp;
    int attached;
    int pct;
    size_t written;
    struct timespec started, last_data;
    char path[DL_PATH_MAX], tmp[DL_PATH_MAX];
    char curlerr[CURL_ERROR_SIZE];
};

static double elapsed(struct timespec since)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - since.tv_sec) +
           (double)(now.tv_nsec - since.tv_nsec) / 1000000000.0;
}

static size_t dl_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    download_job *job = ud;
    size_t n = size * nmemb;
    size_t wrote = fwrite(ptr, 1, n, job->fp);
    job->written += wrote;
    /* Bytes reset the idle deadline even when the percentage hasn't changed
     * or Content-Length is unknown. */
    if (wrote) clock_gettime(CLOCK_MONOTONIC, &job->last_data);
    return wrote;
}

static int dl_progress(void *ud, double total, double now,
                       double ultotal, double ulnow)
{
    (void)ultotal; (void)ulnow;
    download_job *job = ud;
    job->pct = total > 0 ? (int)(now * 100.0 / total) : 0;
    if (job->pct < 0) job->pct = 0;
    if (job->pct > 99) job->pct = 99;
    return 0;
}

void download_destroy(download_job *job)
{
    if (!job) return;
    if (job->attached) curl_multi_remove_handle(job->multi, job->curl);
    if (job->curl) curl_easy_cleanup(job->curl);
    if (job->multi) curl_multi_cleanup(job->multi);
    if (job->fp) fclose(job->fp);
    if (job->tmp[0]) remove(job->tmp);
    free(job);
}

download_job *download_start(const char *url, const char *title, const char *mime,
                             char errbuf[DL_ERR_MAX])
{
    errbuf[0] = '\0';
    download_job *job = calloc(1, sizeof *job);
#define FAIL(msg) do { snprintf(errbuf, DL_ERR_MAX, "%s", (msg)); \
    download_destroy(job); return NULL; } while (0)
    if (!job) FAIL("Out of memory");
    char dir[DL_PATH_MAX], fname[256];
    if (library_dir(dir, sizeof dir) != 0) FAIL("Cannot determine library directory");
    if (library_ensure_dir(dir) != 0) FAIL("Cannot create library directory");
    if (library_build_filename(title, mime, url, fname, sizeof fname) != 0)
        FAIL("Cannot build filename");
    int n = snprintf(job->path, sizeof job->path, "%s/%s", dir, fname);
    if (n < 0 || (size_t)n >= sizeof job->path) FAIL("Library path too long");
    n = snprintf(job->tmp, sizeof job->tmp, "%s/.%s.part", dir, fname);
    if (n < 0 || (size_t)n >= sizeof job->tmp) {
        job->tmp[0] = '\0';
        FAIL("Library path too long");
    }
    job->fp = fopen(job->tmp, "wb");
    if (!job->fp) FAIL(strerror(errno));
    job->curl = curl_easy_init();
    job->multi = curl_multi_init();
    if (!job->curl || !job->multi) FAIL("curl init failed");

    CURL *curl = job->curl;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, job);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, dl_progress);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, job);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "inkshelf/0.1 (PocketBook)");
    /* Slow downloads may take arbitrarily long if bytes keep arriving. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, job->curlerr);
    http_apply_tls(curl);
    clock_gettime(CLOCK_MONOTONIC, &job->started);
    if (!(QueryNetwork() & NET_CONNECTED)) net_ensure_online();
    return job;
#undef FAIL
}

int download_step(download_job *job, char out_path[DL_PATH_MAX],
                  char errbuf[DL_ERR_MAX])
{
    out_path[0] = errbuf[0] = '\0';
#define FAIL(msg) do { snprintf(errbuf, DL_ERR_MAX, "%s", (msg)); return -1; } while (0)
    if (!job->attached) {
        /* WiFi wake-up is polled too; never sleep in the event handler. */
        if (!(QueryNetwork() & NET_CONNECTED)) {
            if (elapsed(job->started) * 1000 >= NET_WAIT_TIMEOUT_MS)
                FAIL("No network connection — check WiFi");
            return 1;
        }
        if (curl_multi_add_handle(job->multi, job->curl) != CURLM_OK)
            FAIL("Cannot start download");
        job->attached = 1;
        clock_gettime(CLOCK_MONOTONIC, &job->last_data);
    }

    int running;
    CURLMcode mc = curl_multi_perform(job->multi, &running);
    if (mc == CURLM_CALL_MULTI_PERFORM) return 1; /* older firmware libcurl */
    if (mc != CURLM_OK) FAIL(curl_multi_strerror(mc));
    int remaining;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(job->multi, &remaining))) {
        if (msg->msg != CURLMSG_DONE) continue;
        CURLcode rc = msg->data.result;
        if (rc != CURLE_OK)
            FAIL(job->curlerr[0] ? job->curlerr : curl_easy_strerror(rc));
        long status = 0;
        curl_easy_getinfo(job->curl, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 400) {
            snprintf(errbuf, DL_ERR_MAX, "HTTP %ld", status);
            return -1;
        }
        if (!job->written) FAIL("Empty response");
        int close_rc = fclose(job->fp);
        job->fp = NULL;
        if (close_rc != 0) FAIL(strerror(errno));
        BookPreparing(job->path);
        if (rename(job->tmp, job->path) != 0) FAIL(strerror(errno));
        job->tmp[0] = '\0';
        snprintf(out_path, DL_PATH_MAX, "%s", job->path);
        BookReady(job->path);
        library_scan_request();
        return 0;
    }
    if (elapsed(job->last_data) >= DL_IDLE_TIMEOUT_SEC) {
        snprintf(errbuf, DL_ERR_MAX,
                 "Download timed out: no data received for %d seconds. Please try again.",
                 DL_IDLE_TIMEOUT_SEC);
        return -1;
    }
    return 1;
#undef FAIL
}

int download_percent(const download_job *job) { return job->pct; }
