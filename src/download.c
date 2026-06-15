/*
 * download.c — book download + library rescan (see download.h).
 *
 * Uses library.h for path / naming logic, libcurl for the transfer, and the
 * older CURLOPT_PROGRESSFUNCTION API (available in every SDK libcurl version,
 * avoiding the newer CURLOPT_XFERINFOFUNCTION / curl_off_t dependency).
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "inkview.h"
#include "download.h"
#include "library.h"

#define HTTP_UA "inkshelf/0.1 (PocketBook)"

/* ---- curl state --------------------------------------------------- */

typedef struct {
    FILE *fp;
    long  written;
    dl_progress_cb cb;
    void *ud;
    int   aborted;
} dl_state;

static size_t dl_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    dl_state *s = ud;
    size_t n = size * nmemb;
    if (fwrite(ptr, 1, n, s->fp) != n) return 0;
    s->written += (long)n;
    return n;
}

/* CURLOPT_PROGRESSFUNCTION signature (older, always available). */
static int dl_progress(void *ud, double dltotal, double dlnow,
                       double ultotal, double ulnow)
{
    (void)ultotal; (void)ulnow;
    dl_state *s = ud;
    if (!s->cb) return 0;

    int pct = 0;
    if (dltotal > 0.0)
        pct = (int)(dlnow * 100.0 / dltotal);
    if (pct < 0)  pct = 0;
    if (pct > 99) pct = 99;

    if (s->cb(pct, s->ud) != 0) { s->aborted = 1; return 1; }
    return 0;
}

/* ---- public API --------------------------------------------------- */

int download_book(const char *url,
                  const char *title,
                  const char *mime,
                  dl_progress_cb cb, void *ud,
                  char out_path[DL_PATH_MAX],
                  char errbuf[DL_ERR_MAX])
{
    out_path[0] = '\0';
    errbuf[0]   = '\0';

#define FAIL(msg) do { \
    strncpy(errbuf, (msg), DL_ERR_MAX - 1); \
    errbuf[DL_ERR_MAX - 1] = '\0'; \
    return -1; } while (0)

    /* Resolve the library directory and make sure it exists. */
    char dir[DL_PATH_MAX];
    if (library_dir(dir, sizeof(dir)) != 0) FAIL("Cannot determine library directory");
    if (library_ensure_dir(dir) != 0) FAIL("Cannot create library directory");

    /* Derive the target filename. */
    char fname[256];
    if (library_build_filename(title, mime, url, fname, sizeof(fname)) != 0)
        FAIL("Cannot build filename");

    snprintf(out_path, DL_PATH_MAX, "%s/%s", dir, fname);

    /* Temp file (same directory -> atomic rename on success). */
    char tmp_path[DL_PATH_MAX];
    snprintf(tmp_path, DL_PATH_MAX, "%s/.%s.part", dir, fname);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) FAIL(strerror(errno));

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(fp); remove(tmp_path); FAIL("curl init failed"); }

    dl_state st = { .fp = fp, .cb = cb, .ud = ud };
    char curlerr[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, dl_progress);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &st);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_UA);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlerr);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (st.aborted) { remove(tmp_path); FAIL("Cancelled"); }
    if (rc != CURLE_OK) {
        remove(tmp_path);
        FAIL(curlerr[0] ? curlerr : curl_easy_strerror(rc));
    }
    if (status >= 400) {
        remove(tmp_path);
        char msg[DL_ERR_MAX];
        snprintf(msg, sizeof(msg), "HTTP %ld", status);
        FAIL(msg);
    }
    if (st.written == 0) { remove(tmp_path); FAIL("Empty response"); }

    if (rename(tmp_path, out_path) != 0) {
        remove(tmp_path);
        FAIL(strerror(errno));
    }

    if (cb) cb(100, ud);

    download_rescan_library();
    return 0;

#undef FAIL
}

int download_rescan_library(void)
{
    /* EVT_RESCAN (6) is the accepted community approach for triggering a
     * PocketBook library index refresh from SDK apps. Not officially
     * documented; may silently do nothing on some firmware versions — the
     * user will see the book in the library after a manual rescan or reboot
     * regardless, since the file is on disk. */
#ifndef EVT_RESCAN
#define EVT_RESCAN 6
#endif
    SendEvent(NULL, EVT_RESCAN, 0, 0);
    return 0;
}
