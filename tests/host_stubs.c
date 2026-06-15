/*
 * host_stubs.c — InkView + libcurl stubs for the host integration smoke test.
 *
 * Lets the real application (main.c + all screens) run on the build host with
 * no e-ink panel and no network: curl returns a canned OPDS feed, the
 * keyboard immediately yields a query, and InkViewMain drives a scripted
 * sequence of events. Run under ASan/UBSan it exercises the OPDS browser's
 * fetch/parse/build/drill/search/book-detail/free paths for real.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "inkview.h"
#include <curl/curl.h>

/* A feed with a navigation entry, a book entry, a templated search link and
 * a next-page link — so build_items, paging, search and book detail all
 * have something to chew on. */
static const char SAMPLE_FEED[] =
"<?xml version=\"1.0\"?>"
"<feed xmlns=\"http://www.w3.org/2005/Atom\">"
"<title>Sample Catalog</title>"
"<link rel=\"search\" type=\"application/atom+xml\" href=\"/search?q={searchTerms}\"/>"
"<link rel=\"next\" type=\"application/atom+xml\" href=\"/page/2\"/>"
"<entry><title>Fiction</title>"
"  <link rel=\"subsection\" href=\"fiction.xml\" type=\"application/atom+xml\"/></entry>"
"<entry><title>A Book</title><author><name>An Author</name></author>"
"  <summary>A short summary.</summary>"
"  <link rel=\"http://opds-spec.org/acquisition\" href=\"/dl/a.epub\" type=\"application/epub+zip\"/></entry>"
"</feed>";

/* ---- InkView stubs ------------------------------------------------- */

int ScreenWidth(void) { return 758; }
int ScreenHeight(void) { return 1024; }
void ClearScreen(void) {}
ifont *OpenFont(const char *n, int s, int a) { (void)n; (void)s; (void)a; return (ifont *)1; }
void CloseFont(ifont *f) { (void)f; }
void SetFont(ifont *f, int c) { (void)f; (void)c; }
void DrawTextRect(int x, int y, int w, int h, const char *s, int f) { (void)x; (void)y; (void)w; (void)h; (void)s; (void)f; }
void DrawString(int x, int y, const char *s) { (void)x; (void)y; (void)s; }
void DrawLine(int a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void DrawRect(int a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void FillArea(int a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void FullUpdate(void) {}
void PartialUpdate(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
void CloseApp(void) {}
int Message(int i, const char *t, const char *x, int to) { (void)i; (void)t; (void)x; (void)to; return 0; }

/* Keyboard immediately returns a query, driving the search path. */
void OpenKeyboard(const char *t, char *b, int m, int f, iv_kbdhandler h)
{
    (void)t; (void)f;
    strncpy(b, "hello world", (size_t)m);
    b[m] = '\0';
    if (h) h(b);
}

void SendEvent(void *hproc, int type, int par1, int par2)
{
    (void)hproc; (void)type; (void)par1; (void)par2;
}

static int (*g_handler)(int, int, int);

void InkViewMain(int (*h)(int, int, int))
{
    g_handler = h;
    h(EVT_INIT, 0, 0);              /* main menu */
    h(EVT_KEYPRESS, KEY_OK, 0);     /* -> OPDS catalog picker */
    h(EVT_KEYPRESS, KEY_OK, 0);     /* -> browse preset feed (Standard Ebooks) */
    h(EVT_KEYPRESS, KEY_DOWN, 0);   /* select book entry */
    h(EVT_KEYPRESS, KEY_OK, 0);     /* -> book detail */
    h(EVT_KEYPRESS, KEY_OK, 0);     /* -> trigger download (stubbed) */
    h(EVT_KEYPRESS, KEY_BACK, 0);   /* pop book detail */
    h(EVT_KEYPRESS, KEY_MENU, 0);   /* search -> push results browse */
    h(EVT_KEYPRESS, KEY_DOWN, 0);   /* move to book in results */
    h(EVT_KEYPRESS, KEY_OK, 0);     /* -> book detail in results */
    h(EVT_KEYPRESS, KEY_BACK, 0);   /* pop results book */
    h(EVT_KEYPRESS, KEY_BACK, 0);   /* pop results browse */
    h(EVT_KEYPRESS, KEY_BACK, 0);   /* pop original browse */
    h(EVT_KEYPRESS, KEY_BACK, 0);   /* pop catalog picker */
    /* Exercise the WiFi Drop screen: enter, refresh (any key), then back. */
    h(EVT_KEYPRESS, KEY_DOWN, 0);   /* main menu: select WiFi Book Drop */
    h(EVT_KEYPRESS, KEY_OK, 0);     /* -> WiFi Drop screen (starts stub server) */
    h(EVT_KEYPRESS, KEY_NEXT, 0);   /* refresh (any non-Back key) */
    h(EVT_KEYPRESS, KEY_BACK, 0);   /* -> back to main menu (stops stub server) */
    h(EVT_EXIT, 0, 0);
}

/* ---- httpd stubs (so the smoke test doesn't open a real socket) ----- */

#include <stdio.h>
#include "httpd.h"

int httpd_start(int port, char *err, size_t errsz)
{
    (void)port; (void)err; (void)errsz;
    return 0;
}
void httpd_stop(void) {}
void httpd_status(httpd_status_t *out)
{
    if (!out) return;
    *out = (httpd_status_t){
        .running = 1, .port = 8080,
        .ip = "192.168.0.1",
        .uploads = 0,
    };
}
int httpd_local_ipv4(char *out, size_t outsz)
{
    if (out && outsz) snprintf(out, outsz, "192.168.0.1");
    return 0;
}

/* ---- download stubs (so the smoke test doesn't hit the network) ----- */

#include <stdio.h>
#include "download.h"

int download_book(const char *url, const char *title, const char *mime,
                  dl_progress_cb cb, void *ud,
                  char out_path[DL_PATH_MAX], char errbuf[DL_ERR_MAX])
{
    (void)url; (void)mime;
    if (cb) { cb(50, ud); cb(100, ud); }
    snprintf(out_path, DL_PATH_MAX, "/tmp/%s.epub", title ? title : "book");
    errbuf[0] = '\0';
    return 0;
}

int download_rescan_library(void) { return 0; }

/* ---- libcurl stubs ------------------------------------------------- */

struct stub_curl {
    size_t (*write_cb)(char *, size_t, size_t, void *);
    void *write_data;
};

CURL *curl_easy_init(void)
{
    return calloc(1, sizeof(struct stub_curl));
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
    va_end(ap);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *c)
{
    struct stub_curl *s = c;
    if (s->write_cb)
        s->write_cb((char *)SAMPLE_FEED, 1, sizeof(SAMPLE_FEED) - 1, s->write_data);
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *c, int info, ...)
{
    (void)c;
    va_list ap;
    va_start(ap, info);
    if (info == CURLINFO_RESPONSE_CODE) {
        long *out = va_arg(ap, long *);
        *out = 200;
    }
    va_end(ap);
    return CURLE_OK;
}

void curl_easy_cleanup(CURL *c) { free(c); }
const char *curl_easy_strerror(CURLcode rc) { (void)rc; return "stub"; }
