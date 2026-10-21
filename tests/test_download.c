/*
 * test_download.c — download_book() against a stub libcurl.
 *
 * The book download builds its own curl handle (download.c), separate from the
 * catalog fetches in http.c. It once skipped the shared TLS settings, so on
 * NSS firmware catalogs loaded but every book download failed with "issuer ...
 * not trusted". This checks the handle that downloads the book gets them.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "inkview.h"
#include <curl/curl.h>

#include "download.h"
#include "net.h"

static int failures;
#define CHECK(c, s) do { int passed = !!(c); \
    printf("  %s %s\n", passed ? "ok  " : "FAIL", s); \
    if (!passed) failures++; } while (0)

/* ---- libcurl stub: one handle at a time, remembers the TLS options ---- */

struct stub_curl {
    size_t (*write_cb)(char *, size_t, size_t, void *);
    void *write_data;
    long verify_peer, verify_host;
};

static long g_verify_peer = -1, g_verify_host = -1;

CURL *curl_easy_init(void)
{
    struct stub_curl *s = calloc(1, sizeof *s);
    if (s) s->verify_peer = s->verify_host = -1;   /* -1 = never set */
    return s;
}

CURLcode curl_easy_setopt(CURL *c, int opt, ...)
{
    struct stub_curl *s = c;
    va_list ap;
    va_start(ap, opt);
    if (opt == CURLOPT_WRITEFUNCTION)
        s->write_cb = va_arg(ap, size_t (*)(char *, size_t, size_t, void *));
    else if (opt == CURLOPT_WRITEDATA)
        s->write_data = va_arg(ap, void *);
    else if (opt == CURLOPT_SSL_VERIFYPEER)
        s->verify_peer = va_arg(ap, long);
    else if (opt == CURLOPT_SSL_VERIFYHOST)
        s->verify_host = va_arg(ap, long);
    va_end(ap);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *c)
{
    struct stub_curl *s = c;
    /* What the transfer ran with, as the server would have seen it. */
    g_verify_peer = s->verify_peer;
    g_verify_host = s->verify_host;
    static char body[] = "PK fake epub";
    if (s->write_cb) s->write_cb(body, 1, sizeof body - 1, s->write_data);
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *c, int info, ...)
{
    (void)c;
    va_list ap;
    va_start(ap, info);
    if (info == CURLINFO_RESPONSE_CODE) *va_arg(ap, long *) = 200;
    va_end(ap);
    return CURLE_OK;
}

void curl_easy_cleanup(CURL *c) { free(c); }
const char *curl_easy_strerror(CURLcode rc) { (void)rc; return "stub"; }

curl_version_info_data *curl_version_info(int v)
{
    (void)v;
    static curl_version_info_data d = { "stub", CURL_VERSION_SSL, "NSS/3.30.2" };
    return &d;
}

/* ---- InkView / network stubs ------------------------------------------ */

void BookPreparing(const char *path) { (void)path; }
void BookReady(const char *path) { (void)path; }
int NetConnect(const char *name) { (void)name; return 0; }
int QueryNetwork(void) { return NET_CONNECTED; }

int main(void)
{
    char dir[] = "/tmp/inkshelf-dl-test.XXXXXX";
    if (!mkdtemp(dir)) return 1;
    setenv("INKSHELF_LIBDIR", dir, 1);

    printf("download TLS:\n");
    char out[DL_PATH_MAX], err[DL_ERR_MAX];
    int rc = download_book("https://www.gutenberg.org/ebooks/84.epub3.images",
                           "Frankenstein", "application/epub+zip",
                           NULL, NULL, out, err);
    CHECK(rc == 0, "an https book download succeeds");
    CHECK(g_verify_peer == 0 && g_verify_host == 0,
          "the download handle gets the shared TLS settings, like catalog fetches");

    unlink(out);
    rmdir(dir);
    return failures ? 1 : 0;
}
