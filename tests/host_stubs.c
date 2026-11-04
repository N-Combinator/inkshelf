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
 *
 * And the saved-catalog behaviour: a custom URL that opens is remembered and
 * listed in the picker, one that fails is not (but stays in the keyboard for
 * fixing), and a saved catalog can be removed again.
 */

#define _POSIX_C_SOURCE 200809L   /* mkstemp */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "inkview.h"
#include <curl/curl.h>

#include "catalogs.h"

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
int StringWidth(const char *s) { return (int)strlen(s) * 18; }
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
static int g_closeapp_calls;
static int  g_book_ready_calls;
static char g_book_calls[32];      /* "P"/"R" per call, in order */
static char g_book_ready_last[256];
void BookReady(const char *path)
{
    g_book_ready_calls++;
    snprintf(g_book_ready_last, sizeof g_book_ready_last, "%s", path);
    strncat(g_book_calls, "R", sizeof g_book_calls - strlen(g_book_calls) - 1);
}
void BookPreparing(const char *path)
{
    (void)path;
    strncat(g_book_calls, "P", sizeof g_book_calls - strlen(g_book_calls) - 1);
}
char *GetSoftwareVersion(void) { static char v[] = "U743G.6.11.1023"; return v; }
void SetPanelType(int type) { (void)type; }
int PanelHeight(void) { return 0; }
void CloseApp(void) { g_closeapp_calls++; }
static int g_messages;
static char g_message[600];
int Message(int i, const char *t, const char *x, int to)
{
    (void)i; (void)t; (void)to;
    g_messages++;
    snprintf(g_message, sizeof g_message, "%s", x);
    return 0;
}
static void (*g_timer)(void);
void SetHardTimer(const char *name, void (*cb)(void), int ms)
{ (void)name; (void)ms; g_timer = cb; }
void ClearTimer(void (*cb)(void)) { if (g_timer == cb) g_timer = NULL; }
static void timer_tick(void)
{
    void (*cb)(void) = g_timer;
    g_timer = NULL;
    if (cb) cb();
}
static int g_dl_starts, g_dl_active, g_dl_complete, g_dl_error;
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
/* What the custom-URL keyboard will "type" next, and the text it was opened
 * with (the app pre-fills it with the last address that failed to open). */
static const char *g_custom_url_input = "";
static char g_custom_url_prefill[512];
void OpenKeyboard(const char *t, char *b, int m, int f, iv_kbdhandler h)
{
    (void)f;
    const char *q = "tolkien";
    if (t && strcmp(t, "Search whole library") == 0)
        q = (g_root_search_n++ == 0) ? "tolkien" : "zzznomatch";
    if (t && strcmp(t, "OPDS catalog URL") == 0) {
        snprintf(g_custom_url_prefill, sizeof g_custom_url_prefill, "%s", b);
        q = g_custom_url_input;
    }
    strncpy(b, q, (size_t)m);
    b[m] = '\0';
    if (h) h(b);
}

void SendEvent(void *hproc, int type, int par1, int par2)
{
    (void)hproc; (void)type; (void)par1; (void)par2;
}

static int (*g_handler)(int, int, int);

static const char *g_fake_received;

void InkViewMain(int (*h)(int, int, int))
{
    g_handler = h;

    /* y inside the search bar band ([header, header+SEARCHBAR_H]); same tap the
     * original smoke test used. */
    const int BARX = 120, BARY = 96;

    /* Saved catalogs go to a scratch file, not the device path. */
    char catalogs_path[] = "/tmp/inkshelf_smoke_catalogs_XXXXXX";
    int cfd = mkstemp(catalogs_path);
    if (cfd >= 0) close(cfd);
    unlink(catalogs_path);
    catalogs_set_path(catalogs_path);

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
    CHECK(g_dl_active && g_timer && screen_has("Downloading..."),
          "download returns to the event loop while still pending");
    h(EVT_KEYPRESS, IV_KEY_OK, 0);
    CHECK(g_dl_starts == 1, "repeated OK does not start a duplicate transfer");
    timer_tick();
    CHECK(g_dl_active && g_timer, "timer advances a pending transfer without blocking input");
    int messages = g_messages;
    h(EVT_HIDE, 0, 0);
    h(EVT_BACKGROUND, 0, 0);
    CHECK(!g_dl_active && !g_timer && g_messages == messages,
          "lock cancels immediately and defers the message until unlock");
    h(EVT_SHOW, 0, 0);
    CHECK(g_messages == messages + 1 && strstr(g_message, "locked") &&
          strstr(g_message, "try downloading") && !screen_has("Downloading..."),
          "unlock restores the detail screen and explains how to retry");
    h(EVT_FOREGROUND, 0, 0);
    CHECK(g_messages == messages + 1 && g_dl_starts == 1,
          "duplicate foreground events neither repeat the notice nor restart the transfer");
    h(EVT_KEYPRESS, IV_KEY_OK, 0);
    CHECK(g_dl_starts == 2 && g_dl_active, "retry explicitly starts a fresh transfer");
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* cancellation also frees the screen safely */
    CHECK(!g_dl_active && !g_timer, "Back cancels the transfer before freeing its screen");
    timer_tick();
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* reopen the book */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);
    h(EVT_BACKGROUND, 0, 0);            /* some firmware only sends background */
    CHECK(!g_dl_active && !g_timer, "background alone also cancels the transfer");
    messages = g_messages;
    h(EVT_FOREGROUND, 0, 0);
    CHECK(g_messages == messages + 1, "foreground alone delivers the interruption notice");
    h(EVT_KEYPRESS, IV_KEY_OK, 0);
    g_dl_complete = 1;
    timer_tick();
    CHECK(!g_dl_active && !g_timer && strstr(g_message, "Saved to your library"),
          "completed transfer restores the screen and shows success");

    /* A failed timer step must restore the screen and permit an explicit retry. */
    g_dl_complete = 0;
    g_dl_error = 1;
    messages = g_messages;
    h(EVT_KEYPRESS, IV_KEY_OK, 0);
    CHECK(g_dl_active == 1 && g_timer, "error scenario begins with an active transfer");
    timer_tick();
    CHECK(!g_dl_active && !g_timer && !screen_has("Downloading..."),
          "failed transfer stops its timer, releases the job and restores the screen");
    CHECK(g_messages == messages + 1 && strstr(g_message, "Download failed:") &&
          strstr(g_message, "stub download error"), "download error is shown to the user");
    timer_tick();
    CHECK(g_messages == messages + 1 && !g_timer && !g_dl_active,
          "stopped error timer neither repeats the message nor restarts the transfer");
    g_dl_error = 0;
    int starts = g_dl_starts;
    h(EVT_KEYPRESS, IV_KEY_OK, 0);
    CHECK(g_dl_starts == starts + 1 && g_dl_active == 1 && g_timer,
          "explicit retry after an error starts one fresh transfer");
    g_dl_complete = 1;
    timer_tick();
    CHECK(!g_dl_active && !g_timer && g_messages == messages + 2 &&
          strstr(g_message, "Saved to your library"), "retry after an error completes successfully");
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

    /* --- Saved catalogs: a custom URL only has to be typed once ----------- */
    printf("saved catalogs:\n");
    CHECK(!screen_has("Remove a saved catalog..."),
          "nothing saved: no remove row in the picker");

    /* Picker rows: Gutenberg, Flibusta, Custom URL... */
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);    /* select "Custom URL..." */
    g_custom_url_input = "http://unreachable.example/opds";
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* type it -> browse fails to load */
    CHECK(screen_has("URL: http://unreachable.example/opds") && !screen_has("A Book"),
          "an unreachable address shows the error screen");
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* -> picker */
    CHECK(catalogs_count() == 0 && !screen_has("unreachable.example/opds"),
          "an address that failed to open is not saved");

    g_custom_url_input = " my.example/opds ";
    int r4 = g_nreq;
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* "Custom URL..." again -> loads */
    CHECK(strcmp(g_custom_url_prefill, "http://unreachable.example/opds") == 0,
          "the keyboard reopens with the failed address, ready to be fixed");
    CHECK(requested_since(r4, "http://my.example/opds") &&
          !requested_since(r4, " http://my.example/opds"),
          "custom URL gains http:// before fetching, without the stray spaces");
    int nav_req = g_nreq;
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* drill into the relative Fiction link */
    CHECK(requested_since(nav_req, "http://my.example/fiction.xml"),
          "relative navigation from a bare address keeps the server");
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* -> custom catalog root */
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* -> picker */
    CHECK(screen_has("Sample Catalog") && screen_has("my.example/opds"),
          "the opened catalog is listed in the picker under its feed title");
    CHECK(screen_has("Remove a saved catalog..."), "picker now offers removal");
    catalogs_load();                    /* forget memory: prove it is on disk */
    CHECK(catalogs_count() == 1 &&
          strcmp(catalogs_get(0)->url, "http://my.example/opds") == 0,
          "saved catalog persists on disk");

    /* Simulate a catalog saved by an older version without a scheme. */
    FILE *legacy = fopen(catalogs_path, "w");
    CHECK(legacy != NULL, "can write a legacy saved catalog");
    if (legacy) {
        fputs("my.example/opds\tSample Catalog\n", legacy);
        fclose(legacy);
    }
    catalogs_load();

    /* Picker rows: Gutenberg, Flibusta, [saved], Custom URL..., Remove... —
     * the selection (row 2) now sits on the saved catalog. */
    int r5 = g_nreq;
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* open the saved catalog: no typing */
    CHECK(requested_since(r5, "http://my.example/opds"),
          "picking a legacy saved catalog adds http:// before fetching");
    nav_req = g_nreq;
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* relative Fiction link again */
    CHECK(requested_since(nav_req, "http://my.example/fiction.xml"),
          "relative navigation from a legacy saved address keeps the server");
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* -> saved catalog root */
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* -> picker */

    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);    /* "Custom URL..." */
    g_custom_url_input = "";            /* keyboard cancelled / left empty */
    int r6 = g_nreq;
    h(EVT_KEYPRESS, IV_KEY_OK, 0);
    CHECK(g_custom_url_prefill[0] == '\0',
          "after a successful open the keyboard starts empty again");
    CHECK(g_nreq == r6, "an empty address fetches nothing");

    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);    /* "Remove a saved catalog..." */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> remove mode */
    CHECK(screen_has("Remove saved catalog") && screen_has("Sample Catalog") &&
          !screen_has("Project Gutenberg"),
          "remove mode lists only the saved catalogs");
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* Back leaves remove mode, not the picker */
    CHECK(screen_has("Project Gutenberg") && catalogs_count() == 1,
          "Back from remove mode returns to the picker, nothing removed");
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);    /* "Remove a saved catalog..." */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> remove mode */
    int r7 = g_nreq;
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* remove the saved catalog */
    CHECK(g_nreq == r7, "removing does not open the catalog");
    CHECK(screen_has("Project Gutenberg") && !screen_has("Sample Catalog") &&
          !screen_has("Remove a saved catalog..."),
          "removed catalog is gone and the picker is back to its plain form");
    catalogs_load();
    CHECK(catalogs_count() == 0, "removal persists on disk");
    unlink(catalogs_path);

    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* pop catalog picker -> main menu */

    /* Exercise the WiFi Drop screen: enter, refresh (any key), then back. */
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);    /* main menu: select WiFi Book Drop */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> WiFi Drop screen (starts stub server) */

    /* --- WiFi drop: received books reach the firmware's library ------------ */
    printf("wifi drop announces received books:\n");
    g_fake_received = "/mnt/ext1/Books/Uploaded One.epub";
    h(EVT_KEYPRESS, IV_KEY_NEXT, 0);    /* refresh (any non-Back key) */
    CHECK(g_book_ready_calls == 1 && strcmp(g_book_ready_last, "/mnt/ext1/Books/Uploaded One.epub") == 0,
          "a book received while the screen is open is announced on refresh");
    g_fake_received = "/mnt/ext1/Books/Uploaded Two.fb2";
    h(EVT_KEYPRESS, IV_KEY_BACK, 0);    /* -> back to main menu (stops stub server) */
    CHECK(g_book_ready_calls == 2 && strcmp(g_book_ready_last, "/mnt/ext1/Books/Uploaded Two.fb2") == 0,
          "a book received after the last refresh is announced when leaving");
    CHECK(strcmp(g_book_calls, "PRPR") == 0,
          "each book is announced as BookPreparing then BookReady");

    /* --- 4) Exit button on the home screen -------------------------------- */
    printf("home screen exit:\n");
    CHECK(g_closeapp_calls == 0, "Back from sub-screens never closes the app");
    CHECK(screen_has("Exit"), "home screen draws an Exit button");
    h(EVT_POINTERUP, 670, 36);          /* tap Exit (top-right corner) */
    CHECK(g_closeapp_calls == 1, "tapping Exit on the home screen closes the app");

    /* Firmware can exit the app with a download still pending. */
    h(EVT_KEYPRESS, IV_KEY_UP, 0);      /* home: select OPDS */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> catalog picker */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> root feed */
    h(EVT_KEYPRESS, IV_KEY_DOWN, 0);
    h(EVT_KEYPRESS, IV_KEY_OK, 0);      /* -> book detail */
    h(EVT_KEYPRESS, IV_KEY_OK, 0);
    CHECK(g_dl_active && g_timer, "exit scenario begins with an active download");
    h(EVT_EXIT, 0, 0);
    CHECK(!g_dl_active && !g_timer, "exit releases the download before closing the UI");

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

/* One pending "received" book at a time (g_fake_received, set by the script). */
int httpd_take_received(char *out, size_t outsz)
{
    if (!g_fake_received) return 0;
    snprintf(out, outsz, "%s", g_fake_received);
    g_fake_received = NULL;
    return 1;
}
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

struct download_job { int pct; };
download_job *download_start(const char *url, const char *title, const char *mime,
                             char errbuf[DL_ERR_MAX])
{
    (void)url; (void)title; (void)mime;
    g_dl_starts++;
    g_dl_active++;
    errbuf[0] = '\0';
    return calloc(1, sizeof(download_job));
}
int download_step(download_job *job, char out_path[DL_PATH_MAX], char errbuf[DL_ERR_MAX])
{
    job->pct = 50;
    if (g_dl_error) {
        out_path[0] = '\0';
        snprintf(errbuf, DL_ERR_MAX, "stub download error");
        return -1;
    }
    errbuf[0] = '\0';
    snprintf(out_path, DL_PATH_MAX, "/tmp/book.epub");
    return g_dl_complete ? 0 : 1;
}
int download_percent(const download_job *job) { return job->pct; }
void download_destroy(download_job *job) { if (job) { g_dl_active--; free(job); } }


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
    if (strstr(g_last_url, "unreachable"))
        return CURLE_COULDNT_CONNECT;
    if (strstr(g_last_url, "zzznomatch")) {
        feed = EMPTY_FEED;
        n = sizeof(EMPTY_FEED) - 1;
    }
    if (s->write_cb && s->write_cb((char *)feed, 1, n, s->write_data) != n)
        return CURLE_WRITE_ERROR;
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
