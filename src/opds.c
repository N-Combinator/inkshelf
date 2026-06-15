/*
 * opds.c — OPDS/Atom parser and helpers (see opds.h), built on xml.c.
 */

#include <stdlib.h>
#include <string.h>

#include "opds.h"
#include "xml.h"

/* ---- small string helpers ------------------------------------------ */

static char *trim_dup(const char *s)
{
    if (!s) return NULL;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') n--;
        else break;
    }
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static int contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nl) return 1;
    }
    return 0;
}

/* ---- parse state --------------------------------------------------- */

typedef struct {
    opds_feed *feed;
    opds_entry *entry;          /* current entry, or NULL at feed level */
    opds_link  *link;           /* pending link being filled, or NULL */
    int in_author;
    int cur_is_link;            /* current element is <link> */
    char *text;                 /* accumulated text for current element */
    size_t text_len;
    int oom;                    /* allocation failure latch */
} pstate;

static void text_reset(pstate *s)
{
    free(s->text);
    s->text = NULL;
    s->text_len = 0;
}

static void text_append(pstate *s, const char *t, size_t n)
{
    char *p = realloc(s->text, s->text_len + n + 1);
    if (!p) { s->oom = 1; return; }
    s->text = p;
    memcpy(s->text + s->text_len, t, n);
    s->text_len += n;
    s->text[s->text_len] = '\0';
}

static opds_entry *feed_add_entry(opds_feed *feed)
{
    opds_entry *p = realloc(feed->entries, sizeof(*p) * (feed->entry_count + 1));
    if (!p) return NULL;
    feed->entries = p;
    opds_entry *e = &feed->entries[feed->entry_count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static opds_link *entry_add_link(opds_entry *e)
{
    opds_link *p = realloc(e->links, sizeof(*p) * (e->link_count + 1));
    if (!p) return NULL;
    e->links = p;
    opds_link *l = &e->links[e->link_count++];
    memset(l, 0, sizeof(*l));
    return l;
}

/* ---- SAX callbacks ------------------------------------------------- */

static void on_start(void *ud, const char *name)
{
    pstate *s = ud;
    const char *ln = xml_localname(name);
    text_reset(s);
    s->cur_is_link = 0;

    if (strcmp(ln, "entry") == 0) {
        s->entry = feed_add_entry(s->feed);
        if (!s->entry) s->oom = 1;
    } else if (strcmp(ln, "author") == 0) {
        s->in_author = 1;
    } else if (strcmp(ln, "link") == 0) {
        s->cur_is_link = 1;
        /* Links only matter on entries for the browser; ignore feed-level. */
        if (s->entry) {
            s->link = entry_add_link(s->entry);
            if (!s->link) s->oom = 1;
        } else {
            s->link = NULL;
        }
    }
}

static void on_attr(void *ud, const char *name, const char *value)
{
    pstate *s = ud;
    if (!s->cur_is_link || !s->link) return;
    const char *ln = xml_localname(name);

    char **slot = NULL;
    if (strcmp(ln, "rel") == 0) slot = &s->link->rel;
    else if (strcmp(ln, "type") == 0) slot = &s->link->type;
    else if (strcmp(ln, "href") == 0) slot = &s->link->href;
    else if (strcmp(ln, "title") == 0) slot = &s->link->title;
    if (!slot) return;

    free(*slot);
    *slot = trim_dup(value);
    if (!*slot) s->oom = 1;
}

static void on_text(void *ud, const char *text, size_t len)
{
    pstate *s = ud;
    text_append(s, text, len);
}

/* Store accumulated text into `*slot` if not already set. */
static void set_once(pstate *s, char **slot)
{
    if (*slot || !s->text) return;
    *slot = trim_dup(s->text);
    if (!*slot) s->oom = 1;
}

static void on_end(void *ud, const char *name)
{
    pstate *s = ud;
    const char *ln = xml_localname(name);

    if (strcmp(ln, "title") == 0) {
        if (s->entry) set_once(s, &s->entry->title);
        else set_once(s, &s->feed->title);
    } else if (strcmp(ln, "name") == 0) {
        if (s->in_author && s->entry) set_once(s, &s->entry->author);
    } else if (strcmp(ln, "summary") == 0 || strcmp(ln, "content") == 0) {
        if (s->entry) set_once(s, &s->entry->summary);
    } else if (strcmp(ln, "author") == 0) {
        s->in_author = 0;
    } else if (strcmp(ln, "link") == 0) {
        s->link = NULL;
        s->cur_is_link = 0;
    } else if (strcmp(ln, "entry") == 0) {
        s->entry = NULL;
    }

    text_reset(s);
}

/* ---- public API ---------------------------------------------------- */

opds_feed *opds_parse(const char *xml, unsigned long len)
{
    opds_feed *feed = calloc(1, sizeof(*feed));
    if (!feed) return NULL;

    pstate st = {0};
    st.feed = feed;

    xml_handler h = {
        .ud = &st,
        .on_start = on_start,
        .on_attr = on_attr,
        .on_text = on_text,
        .on_end = on_end,
    };

    int rc = xml_parse(xml, (size_t)len, &h);
    text_reset(&st);

    if (rc != 0 || st.oom) {
        opds_feed_free(feed);
        return NULL;
    }
    return feed;
}

void opds_feed_free(opds_feed *feed)
{
    if (!feed) return;
    for (int i = 0; i < feed->entry_count; i++) {
        opds_entry *e = &feed->entries[i];
        free(e->title);
        free(e->author);
        free(e->summary);
        for (int j = 0; j < e->link_count; j++) {
            free(e->links[j].rel);
            free(e->links[j].type);
            free(e->links[j].href);
            free(e->links[j].title);
        }
        free(e->links);
    }
    free(feed->entries);
    free(feed->title);
    free(feed);
}

int opds_link_is_acquisition(const opds_link *link)
{
    return link && contains_ci(link->rel, "acquisition");
}

int opds_link_is_catalog(const opds_link *link)
{
    if (!link) return 0;
    if (opds_link_is_acquisition(link)) return 0;
    return contains_ci(link->type, "application/atom+xml");
}

int opds_entry_is_book(const opds_entry *entry)
{
    if (!entry) return 0;
    for (int i = 0; i < entry->link_count; i++)
        if (opds_link_is_acquisition(&entry->links[i])) return 1;
    return 0;
}

int opds_entry_is_navigation(const opds_entry *entry)
{
    if (!entry || opds_entry_is_book(entry)) return 0;
    for (int i = 0; i < entry->link_count; i++)
        if (opds_link_is_catalog(&entry->links[i])) return 1;
    return 0;
}

const char *opds_entry_subfeed_href(const opds_entry *entry)
{
    if (!entry) return NULL;
    for (int i = 0; i < entry->link_count; i++)
        if (opds_link_is_catalog(&entry->links[i]))
            return entry->links[i].href;
    return NULL;
}

const opds_link *opds_entry_best_acquisition(const opds_entry *entry)
{
    if (!entry) return NULL;
    const opds_link *epub = NULL, *fb2 = NULL, *any = NULL;
    for (int i = 0; i < entry->link_count; i++) {
        const opds_link *l = &entry->links[i];
        if (!opds_link_is_acquisition(l)) continue;
        if (!any) any = l;
        if (!epub && contains_ci(l->type, "epub")) epub = l;
        if (!fb2 && (contains_ci(l->type, "fb2") ||
                     contains_ci(l->type, "x-fictionbook"))) fb2 = l;
    }
    if (epub) return epub;
    if (fb2) return fb2;
    return any;
}

int opds_resolve_url(const char *base, const char *href, char *out, unsigned long outsz)
{
    if (!href || !out || outsz == 0) return -1;

    /* Absolute URL already. */
    if (strstr(href, "://")) {
        if (strlen(href) + 1 > outsz) return -1;
        strcpy(out, href);
        return 0;
    }

    if (!base) return -1;

    const char *sep = strstr(base, "://");
    if (!sep) {
        if (strlen(href) + 1 > outsz) return -1;
        strcpy(out, href);
        return 0;
    }

    /* Authority spans from after "://" to the next '/', '?' or end. */
    const char *auth = sep + 3;
    const char *path = auth;
    while (*path && *path != '/' && *path != '?' && *path != '#') path++;
    size_t root_len = (size_t)(path - base);   /* scheme://authority */

    if (href[0] == '/') {
        /* Root-relative. */
        if (root_len + strlen(href) + 1 > outsz) return -1;
        memcpy(out, base, root_len);
        strcpy(out + root_len, href);
        return 0;
    }

    /* Path-relative: take the directory of base's path. */
    const char *q = path;
    while (*q && *q != '?' && *q != '#') q++;   /* end of path */
    const char *last_slash = NULL;
    for (const char *r = path; r < q; r++)
        if (*r == '/') last_slash = r;

    size_t dir_len = last_slash ? (size_t)(last_slash + 1 - base) : root_len;
    /* Ensure a separating slash if base had no path at all. */
    int need_slash = (dir_len == root_len);
    size_t total = dir_len + (need_slash ? 1 : 0) + strlen(href);
    if (total + 1 > outsz) return -1;

    memcpy(out, base, dir_len);
    size_t pos = dir_len;
    if (need_slash) out[pos++] = '/';
    strcpy(out + pos, href);
    return 0;
}
