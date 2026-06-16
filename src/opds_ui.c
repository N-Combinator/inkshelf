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
#define NAV_HINT_H  30        /* scroll-keys memo strip, shown when the list overflows */
#define SB_PAD      24
#define FILTER_CAP  80

static screen_t *make_browse(const char *url, int is_root);
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
    int is_root;            /* this is a catalog's entry feed (root), not a drill-down */
    char filter[FILTER_CAP];   /* local title/author filter ("" = show all) */
    char err[HTTP_ERR_LEN];
} browse_state;

/* The catalog's root search endpoint, captured the first time its entry feed
 * loads (see browse_load). g_root_search_valid == 0 means the catalog as a
 * whole advertises no search, so every screen falls back to local filtering. */
static char g_root_search_href[URLCAP];
static char g_root_search_base[URLCAP];
static int  g_root_search_valid;

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
    /* When the list runs past one screenful, reserve a thin band above the rows
     * for a memo that the hardware keys scroll/page — there is no on-screen
     * scrollbar, so otherwise users may not realise there is more below the
     * fold. Skip it (no wasted space) when everything already fits. */
    if (b->list.count > b->list.per_page)
        ui_list_set_top_inset(&b->list, SEARCHBAR_H + NAV_HINT_H);
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

    /* A catalog's entry feed defines the "whole library" search scope. Capture
     * its search link (if any) so the main screen can search globally and so
     * deeper feeds know whether the catalog supports search at all. Re-loading
     * a root (e.g. paging, or opening a different catalog) refreshes it. */
    if (b->is_root) {
        const char *sh = opds_feed_search_href(b->feed);
        if (sh) {
            snprintf(g_root_search_href, sizeof g_root_search_href, "%s", sh);
            snprintf(g_root_search_base, sizeof g_root_search_base, "%s", b->url);
            g_root_search_valid = 1;
        } else {
            g_root_search_href[0] = '\0';
            g_root_search_base[0] = '\0';
            g_root_search_valid = 0;
        }
    }
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

/* One-line memo drawn just above the list (only when it scrolls) so users
 * discover the hardware scroll/page keys — the list has no scrollbar. */
static void draw_scroll_hint(int y)
{
    int w = ScreenWidth();
    const ui_fonts *f = ui_get_fonts();
    FillArea(0, y, w, NAV_HINT_H, WHITE);
    SetFont(f->sub, DGRAY);
    DrawTextRect(SB_PAD, y, w - 2 * SB_PAD, NAV_HINT_H,
                 "Up/Down keys scroll \xC2\xB7 Prev/Next page",
                 ALIGN_CENTER | VALIGN_MIDDLE);
    DrawLine(SB_PAD, y + NAV_HINT_H - 1, w - SB_PAD, y + NAV_HINT_H - 1, LGRAY);
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
    /* The inset was widened in browse_build_items when the list overflows; that
     * is exactly when the scroll memo belongs above the rows. */
    if (b->list.area_y > ui_header_height() + SEARCHBAR_H)
        draw_scroll_hint(ui_header_height() + SEARCHBAR_H);
    ui_list_draw(&b->list);
    /* Menu runs a server search when this scope has one (whole library at the
     * root, or a category that advertises its own); otherwise Menu filters the
     * loaded list, same as the tap bar. */
    int menu_searches = (b->is_root && g_root_search_valid) ||
                        opds_feed_search_href(b->feed);
    ui_draw_footer(menu_searches
                   ? "OK open \xC2\xB7 tap bar filters \xC2\xB7 Menu searches \xC2\xB7 Back"
                   : "OK open \xC2\xB7 tap bar / Menu filter list \xC2\xB7 Back");
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
            screen_t *c = make_browse(abs, 0);   /* drill-down: a category, not root */
            if (c) nav_push(c);
        }
    } else if (opds_entry_is_book(e)) {
        screen_t *c = make_book_detail(e, b->url);
        if (c) nav_push(c);
    }
}

/* ---- search (async via the on-screen keyboard) -------------------- */

static char g_query[128];

/* Endpoint the pending keyboard search will hit. Copied as plain strings at
 * keyboard-open time (not a screen pointer) so the callback is safe even if the
 * originating screen is gone, and so the chosen scope — whole library vs the
 * current category — is locked in when the user starts typing. */
static char g_search_href[URLCAP];
static char g_search_base[URLCAP];

/* Resolve `shref` against `base`, follow an OpenSearch description document if
 * the link isn't already a "{searchTerms}" template, substitute `query`, and
 * push the results as a new browse screen. Returns 0 on success, -1 if the
 * catalog's search could not be used. */
static int run_server_search(const char *shref, const char *base, const char *query)
{
    if (!shref || !shref[0] || !base || !base[0]) return -1;

    char sabs[URLCAP];
    if (opds_resolve_url(base, shref, sabs, sizeof(sabs)) != 0) return -1;

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
            return -1;
        }
        char *t = opds_opensearch_template(buf, len);
        free(buf);
        if (!t) {
            Message(ICON_INFORMATION, "Search", "Catalog search not supported.", 2500);
            return -1;
        }
        int rc = opds_resolve_url(sabs, t, tmpl, sizeof(tmpl));
        free(t);
        if (rc != 0) return -1;
    }

    char *qurl = opds_apply_search_template(tmpl, query);
    if (!qurl) return -1;
    char absq[URLCAP];
    int rc = opds_resolve_url(base, qurl, absq, sizeof(absq));
    free(qurl);
    if (rc != 0) return -1;

    screen_t *c = make_browse(absq, 0);   /* results are their own feed, not root */
    if (c) nav_push(c);
    return 0;
}

static void search_kbd_cb(char *text)
{
    if (!text || !text[0]) return;
    run_server_search(g_search_href, g_search_base, text);
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

    /* IV_KEY_MENU is the code InkView actually sends for the Menu key (evdev
     * KEY_MENU is a different number and never matched on-device, so Menu —
     * the only search trigger — did nothing, at the root feed and everywhere). */
    if (key == IV_KEY_MENU) {
        if (!b->ok) return 1;

        const char *cur = opds_feed_search_href(b->feed);

        if (b->is_root && g_root_search_valid) {
            /* Main screen: search the whole library via the catalog's root
             * search endpoint. */
            snprintf(g_search_href, sizeof g_search_href, "%s", g_root_search_href);
            snprintf(g_search_base, sizeof g_search_base, "%s", g_root_search_base);
            g_query[0] = '\0';
            OpenKeyboard("Search whole library", g_query,
                         (int)sizeof(g_query) - 1, 0, search_kbd_cb);
        } else if (cur) {
            /* Inside a category that advertises its own search: scope the query
             * to this feed (whatever the catalog assigns to its search link). */
            snprintf(g_search_href, sizeof g_search_href, "%s", cur);
            snprintf(g_search_base, sizeof g_search_base, "%s", b->url);
            g_query[0] = '\0';
            OpenKeyboard("Search this category", g_query,
                         (int)sizeof(g_query) - 1, 0, search_kbd_cb);
        } else {
            /* No server-side search for this scope — fall back to filtering the
             * already-loaded entries, telling the user that's what's happening. */
            Message(ICON_INFORMATION, "Search",
                    g_root_search_valid
                        ? "This section has no search. Filtering the loaded list instead."
                        : "This catalog has no search. Filtering the loaded list instead.",
                    2500);
            filter_open(b, self);
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
        if (g_filter_origin == b) { g_filter_origin = NULL; g_filter_screen = NULL; }
        free(b->url);
        if (b->feed) opds_feed_free(b->feed);
        browse_clear_items(b);
        free(b);
    }
    free(self);
}

static screen_t *make_browse(const char *url, int is_root)
{
    screen_t *s = calloc(1, sizeof(*s));
    browse_state *b = calloc(1, sizeof(*b));
    if (!s || !b) { free(s); free(b); return NULL; }

    b->url = strdup(url);
    if (!b->url) { free(s); free(b); return NULL; }
    b->is_root = is_root;

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
    screen_t *c = make_browse(text, 1);   /* user's chosen entry feed = root */
    if (c) nav_push(c);
}

static void catalog_open(int idx)
{
    if (idx >= 0 && idx < CATALOG_PRESETS) {
        screen_t *c = make_browse(PRESET_URLS[idx], 1);   /* preset entry = root */
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
