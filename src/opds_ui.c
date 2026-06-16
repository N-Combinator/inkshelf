/*
 * opds_ui.c — the OPDS browser screens.
 *
 * Three screens, all built on the nav stack + list widget:
 *   - catalog picker: presets or a custom URL (on-screen keyboard)
 *   - browse: fetch+parse a feed, list entries, drill into subfeeds,
 *     follow "next" paging, run an OpenSearch query, open a book
 *   - book detail: metadata + format; download lands in milestone #5
 *
 * Browse/book screens are heap-allocated per feed and free themselves via
 * the nav stack's on_destroy hook when popped.
 */

#define _POSIX_C_SOURCE 200809L   /* strdup */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inkview.h"

#include "app.h"
#include "download.h"
#include "http.h"
#include "opds.h"
#include "screens.h"
#include "ui.h"

#define URLCAP 1024
#define SEARCHBAR_H 56        /* on-screen local-filter bar, below the header */
#define SB_PAD      24
#define FILTER_CAP  80

static screen_t *make_browse(const char *url);
static screen_t *make_book_detail(const opds_entry *e, const char *base_url);

/* Header + centred body + footer; used for loading/error states. */
static void draw_center_message(const char *title, const char *msg)
{
    int w = ScreenWidth();
    int cy = ui_header_height();
    int ch = ScreenHeight() - ui_header_height() - ui_footer_height();
    const ui_fonts *f = ui_get_fonts();

    ClearScreen();
    ui_draw_header(title);
    SetFont(f->item, DGRAY);
    DrawTextRect(24, cy, w - 48, ch, msg, ALIGN_CENTER | VALIGN_MIDDLE);
    ui_draw_footer("Back to return");
    ui_flush_full();
}

/* ---- browse screen ------------------------------------------------- */

typedef struct {
    char *url;
    opds_feed *feed;
    ui_list list;
    ui_list_item *items;
    char **labels;          /* owned secondary strings, one per display row */
    int *entry_index;       /* display row -> feed entry index (-1 = "More results") */
    int display_count;
    int has_next;
    int ok;
    int loaded;
    char filter[FILTER_CAP];   /* local title/author filter ("" = show all) */
    char err[HTTP_ERR_LEN];
} browse_state;

static void browse_clear_items(browse_state *b)
{
    if (b->labels) {
        for (int i = 0; i < b->display_count; i++) free(b->labels[i]);
        free(b->labels);
    }
    free(b->items);
    free(b->entry_index);
    b->items = NULL;
    b->labels = NULL;
    b->entry_index = NULL;
    b->display_count = 0;
    b->has_next = 0;
}

/* Case-insensitive substring test; an empty needle matches everything. */
static int str_contains_ci(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* An entry survives the local filter if the query appears in its title or
 * author. Works on every catalog because it runs over already-loaded entries
 * (no dependency on a server-side OpenSearch link). */
static int entry_matches(const opds_entry *e, const char *filter)
{
    if (!filter || !filter[0]) return 1;
    return str_contains_ci(e->title, filter) ||
           str_contains_ci(e->author, filter);
}

static void browse_build_items(browse_state *b)
{
    browse_clear_items(b);
    opds_feed *f = b->feed;

    int filtering = b->filter[0] != '\0';
    /* "More results" paging only applies to the full feed: the next page is
     * fetched server-side and would arrive unfiltered, so hide it while a
     * local filter is active. */
    b->has_next = (!filtering && opds_feed_link_href(f, "next")) ? 1 : 0;

    int alloc = f->entry_count + 1;   /* worst case: every entry + a next row */
    b->items = calloc(alloc, sizeof(*b->items));
    b->labels = calloc(alloc, sizeof(*b->labels));
    b->entry_index = calloc(alloc, sizeof(*b->entry_index));
    if (!b->items || !b->labels || !b->entry_index) { browse_clear_items(b); return; }

    int n = 0;
    for (int i = 0; i < f->entry_count; i++) {
        opds_entry *e = &f->entries[i];
        if (filtering && !entry_matches(e, b->filter)) continue;

        b->items[n].primary = e->title ? e->title : "(untitled)";

        const char *sec = NULL;
        if (opds_entry_is_navigation(e)) {
            sec = "Catalog >";
        } else if (opds_entry_is_book(e)) {
            if (e->author) {
                sec = e->author;
            } else {
                const opds_link *a = opds_entry_best_acquisition(e);
                sec = (a && a->type) ? a->type : "eBook";
            }
        } else {
            sec = e->author;
        }
        b->labels[n] = sec ? strdup(sec) : NULL;
        b->items[n].secondary = b->labels[n];
        b->entry_index[n] = i;
        n++;
    }

    if (b->has_next) {
        b->items[n].primary = "More results...";
        b->items[n].secondary = NULL;
        b->entry_index[n] = -1;
        n++;
    }

    b->display_count = n;
    ui_list_init(&b->list, b->items, n);
    ui_list_set_top_inset(&b->list, SEARCHBAR_H);
}

static void browse_load(browse_state *b, const char *url)
{
    char *nu = strdup(url);
    if (!nu) return;
    free(b->url);
    b->url = nu;

    draw_center_message("OPDS Catalog", "Loading...");

    if (b->feed) { opds_feed_free(b->feed); b->feed = NULL; }
    browse_clear_items(b);
    b->ok = 0;
    b->err[0] = '\0';
    b->loaded = 1;

    char *buf = NULL;
    size_t len = 0;
    if (http_get_mem(b->url, &buf, &len, b->err, sizeof(b->err)) != 0)
        return;

    b->feed = opds_parse(buf, len);
    if (!b->feed) {
        /* Include the start of the payload: if the server returned an HTML
         * block/error page instead of OPDS XML, this makes it obvious. */
        char head[60] = "";
        if (buf) {
            size_t n = len < sizeof(head) - 1 ? len : sizeof(head) - 1;
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)buf[i];
                head[i] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
            }
            head[n] = '\0';
        }
        snprintf(b->err, sizeof(b->err), "Not valid OPDS (%lu bytes): %s",
                 (unsigned long)len, head);
        free(buf);
        return;
    }
    free(buf);
    browse_build_items(b);
    b->ok = 1;
}

static void browse_enter(screen_t *self)
{
    browse_state *b = self->data;
    if (!b->loaded) browse_load(b, b->url);
}

/* Tappable filter bar just under the header. Shows the active query or a
 * prompt; tapping it opens the keyboard (see browse_pointer). */
static void draw_search_bar(const browse_state *b)
{
    int w = ScreenWidth();
    int y = ui_header_height();
    int bx = SB_PAD, by = y + 6;
    int bw = w - 2 * SB_PAD, bh = SEARCHBAR_H - 12;
    const ui_fonts *f = ui_get_fonts();

    FillArea(0, y, w, SEARCHBAR_H, WHITE);
    DrawRect(bx, by, bw, bh, DGRAY);

    char line[FILTER_CAP + 48];
    if (b->filter[0]) {
        snprintf(line, sizeof line, "Filter: %s   (tap to edit)", b->filter);
        SetFont(f->sub, BLACK);
    } else {
        snprintf(line, sizeof line, "Tap to filter by title or author");
        SetFont(f->sub, DGRAY);
    }
    DrawTextRect(bx + 12, by, bw - 24, bh, line, ALIGN_LEFT | VALIGN_MIDDLE);
}

static void browse_show(screen_t *self)
{
    browse_state *b = self->data;
    const char *title = (b->ok && b->feed && b->feed->title)
                        ? b->feed->title : "OPDS Catalog";

    if (!b->ok) {
        /* Show the real error, the URL we tried, and where the full request
         * log lives, so a failure can be diagnosed straight from the screen. */
        char body[HTTP_ERR_LEN + URLCAP + 96];
        snprintf(body, sizeof body, "%s\n\nURL: %s\n\nLog: %s",
                 b->err[0] ? b->err : "No data",
                 b->url ? b->url : "(none)",
                 "/mnt/ext1/inkshelf.log");
        draw_center_message(title, body);
        return;
    }

    ClearScreen();
    ui_draw_header(title);
    draw_search_bar(b);
    ui_list_draw(&b->list);
    ui_draw_footer(opds_feed_search_href(b->feed)
                   ? "OK open \xC2\xB7 tap bar filters \xC2\xB7 Menu searches catalog \xC2\xB7 Back"
                   : "OK open \xC2\xB7 tap bar filters list \xC2\xB7 Back");
    ui_flush_full();
}

static void browse_open_selected(screen_t *self)
{
    browse_state *b = self->data;
    if (!b->ok) return;

    int row = b->list.selected;
    if (row < 0 || row >= b->display_count) return;
    int idx = b->entry_index[row];   /* feed entry, or -1 for "More results" */
    char abs[URLCAP];

    /* "More results" synthetic row -> load next page in place. */
    if (idx < 0) {
        const char *nh = opds_feed_link_href(b->feed, "next");
        if (nh && opds_resolve_url(b->url, nh, abs, sizeof(abs)) == 0) {
            browse_load(b, abs);
            b->list.selected = 0;
            browse_show(self);
        }
        return;
    }

    if (idx >= b->feed->entry_count) return;
    opds_entry *e = &b->feed->entries[idx];

    if (opds_entry_is_navigation(e)) {
        const char *h = opds_entry_subfeed_href(e);
        if (h && opds_resolve_url(b->url, h, abs, sizeof(abs)) == 0) {
            screen_t *c = make_browse(abs);
            if (c) nav_push(c);
        }
    } else if (opds_entry_is_book(e)) {
        screen_t *c = make_book_detail(e, b->url);
        if (c) nav_push(c);
    }
}

/* ---- search (async via the on-screen keyboard) -------------------- */

static browse_state *g_search_origin;
static char g_query[128];

static void do_search(browse_state *b, const char *query)
{
    const char *shref = opds_feed_search_href(b->feed);
    if (!shref) return;

    char sabs[URLCAP];
    if (opds_resolve_url(b->url, shref, sabs, sizeof(sabs)) != 0) return;

    char tmpl[URLCAP];
    if (strstr(sabs, "{searchTerms}")) {
        /* rel="search" already a templated OPDS URL. */
        snprintf(tmpl, sizeof(tmpl), "%s", sabs);
    } else {
        /* rel="search" points at an OpenSearch description doc; fetch it. */
        draw_center_message("Search", "Loading...");
        char *buf = NULL;
        size_t len = 0;
        char err[HTTP_ERR_LEN];
        if (http_get_mem(sabs, &buf, &len, err, sizeof(err)) != 0) {
            Message(ICON_WARNING, "Search", err, 2500);
            return;
        }
        char *t = opds_opensearch_template(buf, len);
        free(buf);
        if (!t) {
            Message(ICON_INFORMATION, "Search", "Catalog search not supported.", 2500);
            return;
        }
        int rc = opds_resolve_url(sabs, t, tmpl, sizeof(tmpl));
        free(t);
        if (rc != 0) return;
    }

    char *qurl = opds_apply_search_template(tmpl, query);
    if (!qurl) return;
    char absq[URLCAP];
    int rc = opds_resolve_url(b->url, qurl, absq, sizeof(absq));
    free(qurl);
    if (rc != 0) return;

    screen_t *c = make_browse(absq);
    if (c) nav_push(c);
}

static void search_kbd_cb(char *text)
{
    if (!text || !text[0] || !g_search_origin) return;
    do_search(g_search_origin, text);
}

/* ---- local filter (client-side, works on every catalog) ----------- */

static browse_state *g_filter_origin;
static screen_t     *g_filter_screen;
static char          g_filter_query[FILTER_CAP];

/* Keyboard result for the on-screen filter bar: an empty string clears the
 * filter and shows everything again. Rebuilds the visible rows in place. */
static void filter_kbd_cb(char *text)
{
    if (!g_filter_origin || !g_filter_screen) return;
    browse_state *b = g_filter_origin;
    snprintf(b->filter, sizeof b->filter, "%s", text ? text : "");
    browse_build_items(b);
    b->list.selected = 0;
    b->list.top = 0;
    browse_show(g_filter_screen);
}

static void filter_open(browse_state *b, screen_t *self)
{
    g_filter_origin = b;
    g_filter_screen = self;
    /* Pre-fill with the current query so tapping the bar edits, not resets. */
    snprintf(g_filter_query, sizeof g_filter_query, "%s", b->filter);
    OpenKeyboard("Filter by title or author", g_filter_query,
                 (int)sizeof(g_filter_query) - 1, 0, filter_kbd_cb);
}

static int browse_key(screen_t *self, int key)
{
    browse_state *b = self->data;

    if (key == KEY_MENU) {
        if (b->ok && opds_feed_search_href(b->feed)) {
            g_search_origin = b;
            g_query[0] = '\0';
            OpenKeyboard("Search catalog", g_query,
                         (int)sizeof(g_query) - 1, 0, search_kbd_cb);
        } else {
            Message(ICON_INFORMATION, "inkshelf", "Search not available here.", 2000);
        }
        return 1;
    }

    switch (ui_nav_classify(key)) {
    case UI_NAV_UP:
        if (ui_list_move(&b->list, -1)) browse_show(self);
        return 1;
    case UI_NAV_DOWN:
        if (ui_list_move(&b->list, +1)) browse_show(self);
        return 1;
    case UI_NAV_PAGE_UP:
        if (ui_list_page(&b->list, -1)) browse_show(self);
        return 1;
    case UI_NAV_PAGE_DOWN:
        if (ui_list_page(&b->list, +1)) browse_show(self);
        return 1;
    case UI_NAV_SELECT:
        browse_open_selected(self);
        return 1;
    case UI_NAV_BACK:
        nav_pop();
        return 1;
    default:
        return 0;
    }
}

static int browse_pointer(screen_t *self, int x, int y)
{
    if (ui_back_button_hit(x, y)) {
        nav_pop();
        return 1;
    }
    browse_state *b = self->data;
    if (!b->ok) return 0;

    /* Tap on the filter bar (between header and first row) -> edit the query. */
    int sb_y = ui_header_height();
    if (y >= sb_y && y < sb_y + SEARCHBAR_H) {
        filter_open(b, self);
        return 1;
    }

    int idx = ui_list_hit(&b->list, x, y);
    if (idx < 0) return 0;
    if (idx != b->list.selected) {
        b->list.selected = idx;
        browse_show(self);
    }
    browse_open_selected(self);
    return 1;
}

static void browse_destroy(screen_t *self)
{
    browse_state *b = self->data;
    if (b) {
        if (g_search_origin == b) g_search_origin = NULL;
        if (g_filter_origin == b) { g_filter_origin = NULL; g_filter_screen = NULL; }
        free(b->url);
        if (b->feed) opds_feed_free(b->feed);
        browse_clear_items(b);
        free(b);
    }
    free(self);
}

static screen_t *make_browse(const char *url)
{
    screen_t *s = calloc(1, sizeof(*s));
    browse_state *b = calloc(1, sizeof(*b));
    if (!s || !b) { free(s); free(b); return NULL; }

    b->url = strdup(url);
    if (!b->url) { free(s); free(b); return NULL; }

    s->title = "OPDS Catalog";
    s->data = b;
    s->on_enter = browse_enter;
    s->on_show = browse_show;
    s->on_key = browse_key;
    s->on_pointer = browse_pointer;
    s->on_destroy = browse_destroy;
    return s;
}

/* ---- book detail screen -------------------------------------------- */

typedef struct {
    char *title;
    char *author;
    char *summary;
    char *dl_url;
    char *dl_type;
} book_state;

static void book_show(screen_t *self)
{
    book_state *bk = self->data;
    int w = ScreenWidth();
    int x = 24;
    int cw = w - 48;
    int yy = ui_header_height() + 12;
    const ui_fonts *f = ui_get_fonts();

    ClearScreen();
    ui_draw_header(self->title);

    if (bk->author) {
        SetFont(f->item, DGRAY);
        DrawTextRect(x, yy, cw, 40, bk->author, ALIGN_LEFT | VALIGN_TOP);
        yy += 46;
    }
    if (bk->dl_type) {
        char line[200];
        snprintf(line, sizeof(line), "Format: %s", bk->dl_type);
        SetFont(f->sub, DGRAY);
        DrawTextRect(x, yy, cw, 30, line, ALIGN_LEFT | VALIGN_TOP);
        yy += 36;
    }
    if (bk->summary) {
        SetFont(f->sub, BLACK);
        /* Stop the summary above the Download button (when shown) so the two
         * never overlap; otherwise run down to just above the footer. */
        int bottom = bk->dl_url ? ui_action_button_top()
                                : ScreenHeight() - ui_footer_height();
        int sh = bottom - yy - 12;
        if (sh > 0)
            DrawTextRect(x, yy, cw, sh, bk->summary, ALIGN_LEFT | VALIGN_TOP);
    }

    if (bk->dl_url)
        ui_draw_action_button("\xE2\xAC\x87 Download");

    ui_draw_footer(bk->dl_url ? "Tap Download or press OK  \xE2\x80\xB9 Back"
                              : "No downloadable file  \xE2\x80\xB9 Back");
    ui_flush_full();
}

/* Progress callback: repaints the progress bar on screen. */
typedef struct { const char *title; } prog_ctx;

static int book_progress_cb(int pct, void *ud)
{
    prog_ctx *ctx = ud;
    int w = ScreenWidth();
    int cy = ui_header_height();
    int ch = ScreenHeight() - ui_header_height() - ui_footer_height();
    const ui_fonts *f = ui_get_fonts();

    ClearScreen();
    ui_draw_header(ctx->title);

    char msg[64];
    snprintf(msg, sizeof(msg), "Downloading... %d%%", pct);
    SetFont(f->item, BLACK);
    DrawTextRect(24, cy, w - 48, ch / 2, msg, ALIGN_CENTER | VALIGN_MIDDLE);

    /* Progress bar. */
    int bw = w - 96;
    int bx = 48;
    int by = cy + ch / 2 + 20;
    int bh = 24;
    DrawRect(bx, by, bw, bh, DGRAY);
    int fill = bw * pct / 100;
    if (fill > 0) FillArea(bx, by, fill, bh, BLACK);

    ui_draw_footer("Please wait...");
    PartialUpdate(0, 0, w, ScreenHeight());
    return 0;
}

static void book_do_download(screen_t *self)
{
    book_state *bk = self->data;
    if (!bk->dl_url) {
        Message(ICON_INFORMATION, "inkshelf",
                "No downloadable file for this entry.", 2500);
        return;
    }

    prog_ctx ctx = { .title = bk->title };
    char out_path[DL_PATH_MAX];
    char errbuf[DL_ERR_MAX];

    int rc = download_book(bk->dl_url,
                           bk->title,
                           bk->dl_type,
                           book_progress_cb, &ctx,
                           out_path, errbuf);

    if (rc != 0) {
        char msg[DL_ERR_MAX + 32];
        snprintf(msg, sizeof(msg), "Download failed:\n%s", errbuf);
        Message(ICON_WARNING, "inkshelf", msg, 4000);
    } else {
        char msg[DL_PATH_MAX + 64];
        snprintf(msg, sizeof(msg),
                 "Saved to:\n%s\n\nBook will appear in Library after rescan.",
                 out_path);
        Message(ICON_INFORMATION, "inkshelf", msg, 5000);
    }
    book_show(self);   /* restore the detail screen */
}

static int book_key(screen_t *self, int key)
{
    switch (ui_nav_classify(key)) {
    case UI_NAV_SELECT:
        book_do_download(self);
        return 1;
    case UI_NAV_BACK:
        nav_pop();
        return 1;
    default:
        return 0;
    }
}

static int book_pointer(screen_t *self, int x, int y)
{
    if (ui_back_button_hit(x, y)) {
        nav_pop();
        return 1;
    }
    book_state *bk = self->data;
    if (bk->dl_url && ui_action_button_hit(x, y)) {
        book_do_download(self);
        return 1;
    }
    return 0;
}

static void book_destroy(screen_t *self)
{
    book_state *bk = self->data;
    if (bk) {
        free(bk->title);
        free(bk->author);
        free(bk->summary);
        free(bk->dl_url);
        free(bk->dl_type);
        free(bk);
    }
    free(self);
}

static screen_t *make_book_detail(const opds_entry *e, const char *base_url)
{
    screen_t *s = calloc(1, sizeof(*s));
    book_state *bk = calloc(1, sizeof(*bk));
    if (!s || !bk) { free(s); free(bk); return NULL; }

    bk->title = strdup(e->title ? e->title : "(untitled)");
    bk->author = e->author ? strdup(e->author) : NULL;
    bk->summary = e->summary ? strdup(e->summary) : NULL;

    const opds_link *a = opds_entry_best_acquisition(e);
    if (a) {
        char abs[URLCAP];
        if (a->href && opds_resolve_url(base_url, a->href, abs, sizeof(abs)) == 0)
            bk->dl_url = strdup(abs);
        if (a->type) bk->dl_type = strdup(a->type);
    }

    s->title = bk->title;   /* valid for the screen's lifetime */
    s->data = bk;
    s->on_show = book_show;
    s->on_key = book_key;
    s->on_pointer = book_pointer;
    s->on_destroy = book_destroy;
    return s;
}

/* ---- catalog picker (root of the OPDS flow) ------------------------ */

/* Presets must line up index-for-index with the first CATALOG_PRESETS entries
 * of CATALOG_ITEMS; "Custom URL..." stays last (see catalog_open). */
static const char *PRESET_URLS[] = {
    "https://www.gutenberg.org/ebooks.opds/",
    /* Flibusta's OPDS lives on flibusta.is (the .su host has no /opds and 404s).
     * .is can be blocked in some regions; users behind a block can still add a
     * working mirror via "Custom URL...". */
    "https://flibusta.is/opds",
    /* Standard Ebooks was dropped: its OPDS feed now returns HTTP 401 (requires
     * a patron account), so it can't be a public preset. Gutenberg covers the
     * same English public-domain ground. */
};
static const ui_list_item CATALOG_ITEMS[] = {
    { "Project Gutenberg", "gutenberg.org - 70k+ free books" },
    { "Flibusta",          "flibusta.is - large Russian-language library" },
    { "Custom URL...",     "Enter an OPDS catalog address" },
};
#define CATALOG_COUNT ((int)(sizeof(CATALOG_ITEMS) / sizeof(CATALOG_ITEMS[0])))
#define CATALOG_PRESETS ((int)(sizeof(PRESET_URLS) / sizeof(PRESET_URLS[0])))

typedef struct { ui_list list; } catalog_state;
static catalog_state g_catalog;
static char g_custom_url[URLCAP];

static void custom_kbd_cb(char *text)
{
    if (!text || !text[0]) return;
    screen_t *c = make_browse(text);
    if (c) nav_push(c);
}

static void catalog_open(int idx)
{
    if (idx >= 0 && idx < CATALOG_PRESETS) {
        screen_t *c = make_browse(PRESET_URLS[idx]);
        if (c) nav_push(c);
    } else if (idx == CATALOG_COUNT - 1) {
        g_custom_url[0] = '\0';
        OpenKeyboard("OPDS catalog URL", g_custom_url,
                     (int)sizeof(g_custom_url) - 1, 0, custom_kbd_cb);
    }
}

static void catalog_enter(screen_t *self)
{
    catalog_state *st = self->data;
    int prev = st->list.items ? st->list.selected : 0;
    ui_list_init(&st->list, CATALOG_ITEMS, CATALOG_COUNT);
    if (prev < CATALOG_COUNT) st->list.selected = prev;
}

static void catalog_show(screen_t *self)
{
    catalog_state *st = self->data;
    ClearScreen();
    ui_draw_header(self->title);
    ui_list_draw(&st->list);
    ui_draw_footer("OK or tap to open  Back");
    ui_flush_full();
}

static int catalog_key(screen_t *self, int key)
{
    catalog_state *st = self->data;
    switch (ui_nav_classify(key)) {
    case UI_NAV_UP:
        if (ui_list_move(&st->list, -1)) catalog_show(self);
        return 1;
    case UI_NAV_DOWN:
        if (ui_list_move(&st->list, +1)) catalog_show(self);
        return 1;
    case UI_NAV_PAGE_UP:
        if (ui_list_page(&st->list, -1)) catalog_show(self);
        return 1;
    case UI_NAV_PAGE_DOWN:
        if (ui_list_page(&st->list, +1)) catalog_show(self);
        return 1;
    case UI_NAV_SELECT:
        catalog_open(st->list.selected);
        return 1;
    case UI_NAV_BACK:
        nav_pop();
        return 1;
    default:
        return 0;
    }
}

static int catalog_pointer(screen_t *self, int x, int y)
{
    if (ui_back_button_hit(x, y)) {
        nav_pop();
        return 1;
    }
    catalog_state *st = self->data;
    int idx = ui_list_hit(&st->list, x, y);
    if (idx < 0) return 0;
    if (idx != st->list.selected) {
        st->list.selected = idx;
        catalog_show(self);
    }
    catalog_open(idx);
    return 1;
}

static screen_t g_catalog_screen = {
    .title = "OPDS Catalog",
    .data = &g_catalog,
    .on_enter = catalog_enter,
    .on_show = catalog_show,
    .on_key = catalog_key,
    .on_pointer = catalog_pointer,
};

screen_t *screen_opds_catalog(void)
{
    return &g_catalog_screen;
}
