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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inkview.h"

#include "app.h"
#include "http.h"
#include "opds.h"
#include "screens.h"
#include "ui.h"

#define URLCAP 1024

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
    int display_count;
    int has_next;
    int ok;
    int loaded;
    char err[HTTP_ERR_LEN];
} browse_state;

static void browse_clear_items(browse_state *b)
{
    if (b->labels) {
        for (int i = 0; i < b->display_count; i++) free(b->labels[i]);
        free(b->labels);
    }
    free(b->items);
    b->items = NULL;
    b->labels = NULL;
    b->display_count = 0;
    b->has_next = 0;
}

static void browse_build_items(browse_state *b)
{
    browse_clear_items(b);
    opds_feed *f = b->feed;

    b->has_next = opds_feed_link_href(f, "next") ? 1 : 0;
    int n = f->entry_count + b->has_next;
    int alloc = n > 0 ? n : 1;

    b->items = calloc(alloc, sizeof(*b->items));
    b->labels = calloc(alloc, sizeof(*b->labels));
    if (!b->items || !b->labels) { browse_clear_items(b); return; }

    for (int i = 0; i < f->entry_count; i++) {
        opds_entry *e = &f->entries[i];
        b->items[i].primary = e->title ? e->title : "(untitled)";

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
        b->labels[i] = sec ? strdup(sec) : NULL;
        b->items[i].secondary = b->labels[i];
    }

    if (b->has_next) {
        b->items[f->entry_count].primary = "More results...";
        b->items[f->entry_count].secondary = NULL;
    }

    b->display_count = n;
    ui_list_init(&b->list, b->items, n);
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
    free(buf);
    if (!b->feed) {
        snprintf(b->err, sizeof(b->err), "Could not parse feed");
        return;
    }
    browse_build_items(b);
    b->ok = 1;
}

static void browse_enter(screen_t *self)
{
    browse_state *b = self->data;
    if (!b->loaded) browse_load(b, b->url);
}

static void browse_show(screen_t *self)
{
    browse_state *b = self->data;
    const char *title = (b->ok && b->feed && b->feed->title)
                        ? b->feed->title : "OPDS Catalog";

    if (!b->ok) {
        draw_center_message(title, b->err[0] ? b->err : "No data");
        return;
    }

    ClearScreen();
    ui_draw_header(title);
    ui_list_draw(&b->list);
    ui_draw_footer(opds_feed_search_href(b->feed)
                   ? "OK: open  Menu: search  Back"
                   : "OK: open  Back");
    ui_flush_full();
}

static void browse_open_selected(screen_t *self)
{
    browse_state *b = self->data;
    if (!b->ok) return;

    int idx = b->list.selected;
    char abs[URLCAP];

    /* "More results" synthetic row -> load next page in place. */
    if (b->has_next && idx == b->feed->entry_count) {
        const char *nh = opds_feed_link_href(b->feed, "next");
        if (nh && opds_resolve_url(b->url, nh, abs, sizeof(abs)) == 0) {
            browse_load(b, abs);
            b->list.selected = 0;
            browse_show(self);
        }
        return;
    }

    if (idx < 0 || idx >= b->feed->entry_count) return;
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
    browse_state *b = self->data;
    if (!b->ok) return 0;
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
        int sh = ScreenHeight() - ui_footer_height() - yy - 12;
        if (sh > 0)
            DrawTextRect(x, yy, cw, sh, bk->summary, ALIGN_LEFT | VALIGN_TOP);
    }

    ui_draw_footer(bk->dl_url ? "OK: download  Back" : "No file  Back");
    ui_flush_full();
}

static int book_key(screen_t *self, int key)
{
    book_state *bk = self->data;
    switch (ui_nav_classify(key)) {
    case UI_NAV_SELECT:
        if (bk->dl_url)
            Message(ICON_INFORMATION, "inkshelf",
                    "Download arrives in milestone #5.", 2500);
        else
            Message(ICON_INFORMATION, "inkshelf",
                    "No downloadable file for this entry.", 2500);
        return 1;
    case UI_NAV_BACK:
        nav_pop();
        return 1;
    default:
        return 0;
    }
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
    s->on_destroy = book_destroy;
    return s;
}

/* ---- catalog picker (root of the OPDS flow) ------------------------ */

static const char *PRESET_URLS[] = {
    "https://standardebooks.org/feeds/opds",
    "https://www.gutenberg.org/ebooks.opds/",
};
static const ui_list_item CATALOG_ITEMS[] = {
    { "Standard Ebooks",  "standardebooks.org - curated public domain" },
    { "Project Gutenberg", "gutenberg.org - 70k+ free books" },
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
