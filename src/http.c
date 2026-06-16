/*
 * http.c — libcurl-backed HTTP GET + simple file download (see http.h).
 *
 * For the richer progress-callback download used by the book-download screen
 * see download.c; http_download_file is a simpler no-progress streaming
 * variant for internal use.
 */

#define _POSIX_C_SOURCE 200809L   /* localtime_r */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>

#include "http.h"

#define HTTP_UA       "inkshelf/0.1 (PocketBook)"
#define HTTP_TIMEOUT  30L
#define HTTP_MAX_BODY (32 * 1024 * 1024)   /* 32 MB cap for in-memory feeds */
#define HTTP_MAX_FILE (512UL * 1024 * 1024) /* 512 MB cap for book downloads */

/* Diagnostic log on the user partition. Best-effort: if it can't be opened
 * (e.g. host build where /mnt/ext1 doesn't exist) logging is silently skipped.
 * Override at build time with -DHTTP_LOG_PATH=\"...\" for host testing. */
#ifndef HTTP_LOG_PATH
#define HTTP_LOG_PATH "/mnt/ext1/inkshelf.log"
#endif

static void http_log(const char *fmt, ...)
{
    FILE *lf = fopen(HTTP_LOG_PATH, "a");
    if (!lf) return;

    time_t now = time(NULL);
    struct tm tmv;
    char ts[32] = "";
    if (localtime_r(&now, &tmv))
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(lf, "%s ", ts);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);

    fputc('\n', lf);
    fclose(lf);
}

/* Log the libcurl build once: the SSL feature flag + backend answer the key
 * question of whether HTTPS catalogs can work at all on this firmware. */
static void http_log_env_once(void)
{
    static int done = 0;
    if (done) return;
    done = 1;

    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    if (!v) { http_log("libcurl: version info unavailable"); return; }
    http_log("libcurl %s | ssl=%s | backend=%s",
             v->version ? v->version : "?",
             (v->features & CURL_VERSION_SSL) ? "yes" : "NO",
             v->ssl_version ? v->ssl_version : "(none)");
}

/* Append a short, printable snippet of a response body to the log so a block
 * page or unexpected payload is visible after the fact. */
static void http_log_snippet(const char *data, size_t len)
{
    if (!data || len == 0) { http_log("  body: (empty)"); return; }
    size_t n = len < 200 ? len : 200;
    char snip[201];
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        snip[o++] = (c == '\n' || c == '\r' || c == '\t') ? ' '
                  : (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    snip[o] = '\0';
    http_log("  body[0..%zu]: %s", n, snip);
}

typedef struct {
    char *data;
    size_t len;
    int overflow;
} membuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    membuf *m = ud;
    size_t add = size * nmemb;

    if (m->len + add > HTTP_MAX_BODY) {
        m->overflow = 1;
        return 0;   /* abort transfer */
    }
    char *p = realloc(m->data, m->len + add + 1);
    if (!p) return 0;
    m->data = p;
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = '\0';
    return add;
}

static void set_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz) {
        strncpy(err, msg, errsz - 1);
        err[errsz - 1] = '\0';
    }
}

/* A path is usable as a CA bundle only if it is a regular file that is large
 * enough to plausibly hold real certificates. The size floor matters on
 * PocketBook: the firmware's /etc/ssl/certs/ca-certificates.crt exists (so a
 * bare stat() accepts it) but is empty/unusable, and curl then rejects it with
 * CURLE_SSL_CACERT_BADFILE (77). Requiring >= 1 KB skips that trap; the real
 * Mozilla bundle we ship is ~190 KB. */
static int ca_file_ok(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 1024;
}

/* The OPDS presets are HTTPS, but PocketBook firmware ships no usable CA bundle
 * where libcurl looks by default, so TLS verification fails and every catalog
 * load errors out. Find a real bundle: first one shipped next to the .app, then
 * well-known locations (our recommended install path first). If none qualifies
 * we leave curl's default and let the real TLS error surface (and be logged). */
static const char *http_ca_bundle(void)
{
    /* 1) cacert.pem sitting in the same directory as the running binary — the
     *    most self-contained install ("drop both files in applications/"). */
    static char appca[1100];
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            size_t dirlen = (size_t)(slash - exe) + 1;        /* keep the '/' */
            if (dirlen + sizeof "cacert.pem" <= sizeof appca) {
                memcpy(appca, exe, dirlen);
                memcpy(appca + dirlen, "cacert.pem", sizeof "cacert.pem");
                if (ca_file_ok(appca)) return appca;
            }
        }
    }

    /* 2) Well-known fixed locations; our recommended install path first. */
    static const char *const paths[] = {
        "/mnt/ext1/system/config/cacert.pem",   /* recommended install path  */
        "/mnt/ext1/applications/cacert.pem",     /* alongside the apps        */
        "/ebrmain/config/cacert.pem",            /* PocketBook system area    */
        "/etc/ssl/certs/ca-certificates.crt",    /* Debian / Ubuntu           */
        "/etc/ssl/cert.pem",                     /* BSD / musl / Alpine       */
        "/etc/pki/tls/certs/ca-bundle.crt",      /* RHEL / Fedora             */
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
        if (ca_file_ok(paths[i]))
            return paths[i];
    return NULL;
}

/* Apply shared TLS options to a handle (CA bundle if we can find one). */
static void http_apply_tls(CURL *curl)
{
    const char *ca = http_ca_bundle();
    if (ca) curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
}

int http_get_mem(const char *url, char **out_buf, size_t *out_len,
                 char *err, size_t errsz)
{
    if (!url || !out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;

    CURL *curl = curl_easy_init();
    if (!curl) {
        set_err(err, errsz, "curl init failed");
        return -1;
    }

    membuf m = {0};
    char curlerr[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_UA);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlerr);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* allow gzip */
    http_apply_tls(curl);

    http_log_env_once();
    http_log("GET %s", url ? url : "(null)");

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    const char *ca = http_ca_bundle();
    http_log("  -> rc=%d (%s) | http=%ld | bytes=%zu | ca=%s",
             rc, curl_easy_strerror(rc), status, m.len, ca ? ca : "(curl default)");
    http_log_snippet(m.data, m.len);

    if (rc != CURLE_OK) {
        if (m.overflow) {
            set_err(err, errsz, "response too large");
        } else {
            /* Lead the message with the numeric CURLcode so a photo of the
             * dialog is enough to diagnose (60=cert, 6=DNS, 7=connect, ...).
             * Sized to the curl error buffer; set_err truncates into errsz. */
            char cmsg[CURL_ERROR_SIZE + 32];
            snprintf(cmsg, sizeof cmsg, "curl %d: %s", rc,
                     curlerr[0] ? curlerr : curl_easy_strerror(rc));
            set_err(err, errsz, cmsg);
        }
        free(m.data);
        curl_easy_cleanup(curl);
        return -1;
    }
    if (status >= 400) {
        char msg[HTTP_ERR_LEN];
        snprintf(msg, sizeof(msg), "HTTP %ld from server", status);
        set_err(err, errsz, msg);
        free(m.data);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_cleanup(curl);

    if (!m.data) {
        /* Empty but successful response. */
        m.data = calloc(1, 1);
        if (!m.data) { set_err(err, errsz, "out of memory"); return -1; }
    }
    *out_buf = m.data;
    *out_len = m.len;
    return 0;
}

/* ---- streaming file download --------------------------------------- */

typedef struct { FILE *fp; size_t written; } filebuf;

static size_t file_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    filebuf *f = ud;
    size_t n = size * nmemb;
    size_t wrote = fwrite(ptr, 1, n, f->fp);
    f->written += wrote;
    return wrote;
}

int http_download_file(const char *url, const char *dest_path,
                       char *err, size_t errsz)
{
    if (!url || !dest_path) return -1;

    /* Write to a temp file; rename into place on success. */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.part", dest_path);

    FILE *fp = fopen(tmp, "wb");
    if (!fp) { set_err(err, errsz, "cannot open output file"); return -1; }

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(fp); remove(tmp); set_err(err, errsz, "curl init failed"); return -1; }

    filebuf fb = { .fp = fp };
    char curlerr[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_UA);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlerr);
    http_apply_tls(curl);

    http_log_env_once();
    http_log("DL  %s", url ? url : "(null)");

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    fclose(fp);

    http_log("  -> rc=%d (%s) | http=%ld | bytes=%zu",
             rc, curl_easy_strerror(rc), status, fb.written);

    if (rc != CURLE_OK) {
        remove(tmp);
        char cmsg[CURL_ERROR_SIZE + 32];
        snprintf(cmsg, sizeof cmsg, "curl %d: %s", rc,
                 curlerr[0] ? curlerr : curl_easy_strerror(rc));
        set_err(err, errsz, cmsg);
        return -1;
    }
    if (status >= 400) {
        remove(tmp);
        char msg[HTTP_ERR_LEN];
        snprintf(msg, sizeof(msg), "HTTP %ld from server", status);
        set_err(err, errsz, msg);
        return -1;
    }
    if (fb.written == 0) {
        remove(tmp);
        set_err(err, errsz, "Empty response");
        return -1;
    }
    if (rename(tmp, dest_path) != 0) {
        remove(tmp);
        set_err(err, errsz, "rename failed");
        return -1;
    }
    return 0;
}
