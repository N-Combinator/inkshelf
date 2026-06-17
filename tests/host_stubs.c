/*
 * host_stubs.c — InkView + libcurl stubs for the host integration smoke test.
 *
 * Lets the real application (main.c + all screens) run on the build host with
 * no e-ink panel and no network: curl returns a canned OPDS feed, the
 * keyboard immediately yields a query, and InkViewMain drives a scripted
 * sequence of events. Run under ASan/UBSan it exercises the OPDS browser's
 * fetch/parse/build/drill/search/book-detail/free paths for real.
 *
 * It also asserts the search-scope behaviour (the reported bug: searching from
 * the catalog root returned nothing). By recording the URLs curl is asked for
 * and the text drawn to the screen, the script proves that:
 *   1. a search-bar tap at the ROOT issues a whole-library *server* search and
 *      renders results (not a local filter over the visible categories);
 *   2. a search from inside a category still issues a scoped server search;
 *   3. a search for a missing term returns an empty result list, not an error.
 */

#include <stdarg.h>
#include <stdio.h>
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

/* A valid but entry-less feed, returned for a search whose term matches
 * nothing, so the empty-result path is exercised for real. */
static const char EMPTY_FEED[] =
"<?xml version=\"1.0\"?>"
"<feed xmlns=\"http://www.w3.org/2005/Atom\">"
"<title>No results</title>"
"</feed>";

/* ---- test bookkeeping --------------------------------------------- */

static int g_fail;

#define CHECK(cond, msg) do {                                  \
    if (cond) { printf("  ok   %s\n", msg); }                  \
    else      { printf("  FAIL %s\n", msg); g_fail++; }        \
} while (0)

/* Every URL curl is asked to fetch, in order — so the script can assert which
 * requests a UI action did (or did not) trigger. */
#define MAXREQ 64
static char g_requests[MAXREQ][512];
static int  g_nreq;
static char g_last_url[512];

static void record_request(const char *url)
{
    snprintf(g_last_url, sizeof g_last_url, "%s", url ? url : "");
    if (g_nreq < MAXREQ) {
        snprintf(g_requests[g_nreq], sizeof g_requests[0], "%s", url ? url : "");
        g_nreq++;
    }
}

/* Was a request whose URL contains `needle` made at or after index `from`? */
static int requested_since(int from, const char *needle)
{
    for (int i = from; i < g_nreq; i++)
        if (strstr(g_requests[i], needle)) return 1;
    return 0;
}

/* Text drawn to the *current* screen (reset on ClearScreen, which every screen
 * calls before repainting) — lets the script see what the user would see. */
#define MAXDRAW 64
static char g_drawn[MAXDRAW][256];
static int  g_ndraw;

static int screen_has(const char *needle)
{
    for (int i = 0; i < g_ndraw; i++)
        if (strstr(g_drawn[i], needle)) return 1;
    return 0;
}

/* ---- InkView stubs ------------------------------------------------- */

int ScreenWidth(void) { return 758; }
int ScreenHeight(void) { return 1024; }
void ClearScreen(void) { g_ndraw = 0; }   /* new screen -> forget old text */
ifont *OpenFont(const char *n, int s, int a) { (void)n; (void)s; (void)a; return (ifont *)1; }
void CloseFont(ifont *f) { (void)f; }
void SetFont(ifont *f, int c) { (void)f; (void)c; }
void DrawTextRect(int x, int y, int w, int h, const char *s, int f)
{
    (void)x; (void)y; (void)w; (void)h; (void)f;
    if (s && g_ndraw < MAXDRAW) {
        snprintf(g_drawn[g_ndraw], sizeof g_drawn[0], "%s", s);
        g_ndraw++;
    }
}
void DrawString(int x, int y, const char *s) { (void)x; (void)y; (void)s; }
void DrawLine(int a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void DrawRect(int a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void FillArea(int a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void FullUpdate(void) {}
void PartialUpdate(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
void CloseApp(void) {}
int Message(int i, const char *t, const char *x, int to) { (void)i; (void)t; (void)x; (void)to; return 0; }
int NetConnect(const char *name) { (void)name; return 0; }
/* Host: always report the link as connected so net_wait_online() takes its
 * fast path (no polling / nanosleep) and the tests stay instant. */
int QueryNetwork(void) { return NET_CONNECTED; }

/* Keyboard immediately returns a query, driving the search/filter path. The
 * query depends on the prompt title so the script controls each search:
 *   - the root bar's first search uses a term present in the catalog, its
 *     second uses a term that matches nothing (to exercise the empty path);
 *   - the category search uses a present term.
 */
static int g_root_search_n;
void OpenKeyboard(const char *t, char *b, int m, int f, iv_kbdhandler h)
{
    (void)f;
    const char *q = "tolkien";
    if (t && strcmp(t, "Search whole library") == 0)
        q = (g_root_search_n++ == 0) ? "tolkien" : "zzznomatch";
    strncpy(b, q, (size_t)m);
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

    /* y inside the search bar band ([header, header+SEARCHBAR_H]); same tap the
     * original smoke test used. */
    const int BARX = 120, BARY = 96;

    h(EVT_INIT, 0, 0);                  /* main menu */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> OPDS catalog picker */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> browse preset feed as ROOT (Gutenberg) */

    printf("opds search scope:\n");

    /* --- 1) ROOT search via the bar runs a whole-library server search ---- */
    int r0 = g_nreq;
    h(EVT_POINTERUP, BARX, BARY);       /* tap bar at root -> server search "tolkien" */
    CHECK(requested_since(r0, "search?q=tolkien"),
          "root bar tap issues a whole-library server search");
    CHECK(screen_has("A Book"),
          "root search renders results (the reported bug: returned nothing)");

    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);    /* select the book in the results */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> book detail */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> trigger download (stubbed) */
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* pop book detail */
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* pop results -> back at ROOT */

    /* --- 2) Search from inside a CATEGORY still issues a scoped search ----- */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* open "Fiction" -> category browse (not root) */
    int r2 = g_nreq;
    h(EVT_KEYPRESS, IV_KEY_MENU, 0);    /* Menu -> "Search this category" "tolkien" */
    CHECK(requested_since(r2, "search?q=tolkien"),
          "category search issues a scoped server search");
    CHECK(screen_has("A Book"), "category search still renders results");
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* pop results -> category */
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* pop category -> ROOT */

    /* --- 3) A search for a missing term -> empty result, not an error ----- */
    int r3 = g_nreq;
    h(EVT_POINTERUP, BARX, BARY);       /* tap bar at root -> server search "zzznomatch" */
    CHECK(requested_since(r3, "search?q=zzznomatch"),
          "missing-term search still queries the server");
    CHECK(!screen_has("A Book"),
          "missing-term search yields an empty list, no book and no error");
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* pop empty results -> ROOT */

    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* pop ROOT -> catalog picker */
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* pop catalog picker -> main menu */

    /* Exercise the WiFi Drop screen: enter, refresh (any key), then back. */
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);    /* main menu: select WiFi Book Drop */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> WiFi Drop screen (starts stub server) */
    h(EVT_KEYPRESS, IV_KEY_NEXT, 0);    /* refresh (any non-Back key) */
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* -> back to main menu (stops stub server) */
    h(EVT_EXIT, 0, 0);

    if (g_fail) { printf("opds search scope: FAILED (%d)\n", g_fail); exit(1); }
    printf("opds search scope: all passed\n");
}

/* ---- httpd stubs (so the smoke test doesn't open a real socket) ----- */

#include "httpd.h"

int httpd_start(int port, char *err, size_t errsz)
{
    (void)port; (void)err; (void)errsz;
    return 0;
}
void httpd_stop(void) {}
void httpd_set_pin(const char *pin) { (void)pin; }
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
    else if (opt == CURLOPT_URL)
        record_request(va_arg(ap, const char *));
    va_end(ap);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *c)
{
    struct stub_curl *s = c;
    /* A search whose term matched nothing gets an entry-less feed; every other
     * fetch gets the sample catalog. */
    const char *feed = SAMPLE_FEED;
    size_t n = sizeof(SAMPLE_FEED) - 1;
    if (strstr(g_last_url, "zzznomatch")) {
        feed = EMPTY_FEED;
        n = sizeof(EMPTY_FEED) - 1;
    }
    if (s->write_cb)
        s->write_cb((char *)feed, 1, n, s->write_data);
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

curl_version_info_data *curl_version_info(int v)
{
    (void)v;
    static curl_version_info_data d = {
        .version = "stub", .features = CURL_VERSION_SSL, .ssl_version = "stub-ssl",
    };
    return &d;
}
