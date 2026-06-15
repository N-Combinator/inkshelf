/*
 * opds.h — OPDS (Atom) feed model and parser.
 *
 * Parses an OPDS catalog feed into a small in-memory model: a feed with a
 * title and a list of entries, each entry carrying its title/author/summary
 * and its links. OPDS distinguishes two entry kinds:
 *   - navigation: points to another (sub)feed   -> drill down
 *   - acquisition: a book with downloadable files -> show download options
 *
 * Link/URL helpers classify links and resolve relative hrefs against the
 * feed URL so the browser UI (milestone #4) and downloader (#5) can act on
 * them directly.
 */

#ifndef INKSHELF_OPDS_H
#define INKSHELF_OPDS_H

typedef struct {
    char *rel;     /* link relation, e.g. ".../acquisition" (may be NULL) */
    char *type;    /* MIME type, e.g. "application/epub+zip" (may be NULL) */
    char *href;    /* target URL, possibly relative (never NULL once parsed) */
    char *title;   /* optional human label (may be NULL) */
} opds_link;

typedef struct {
    char *title;
    char *author;
    char *summary;
    opds_link *links;
    int link_count;
} opds_entry;

typedef struct {
    char *title;
    opds_entry *entries;
    int entry_count;
    opds_link *links;        /* feed-level links (search, next, start, ...) */
    int link_count;
} opds_feed;

/* Parse an OPDS/Atom document. Returns a heap feed (free with
 * opds_feed_free) or NULL on allocation/parse failure. */
opds_feed *opds_parse(const char *xml, unsigned long len);
void opds_feed_free(opds_feed *feed);

/* ---- link classification ------------------------------------------- */

/* A link whose rel marks it as a downloadable acquisition link. */
int opds_link_is_acquisition(const opds_link *link);

/* A link pointing at another OPDS/Atom feed (navigation / subsection). */
int opds_link_is_catalog(const opds_link *link);

/* Entry kind helpers (an entry is "navigation" if it links to a subfeed and
 * has no acquisition link; otherwise, if it has an acquisition link, it is a
 * book). */
int opds_entry_is_navigation(const opds_entry *entry);
int opds_entry_is_book(const opds_entry *entry);

/* The subfeed URL for a navigation entry, or NULL. */
const char *opds_entry_subfeed_href(const opds_entry *entry);

/* Best download link for a book entry, preferring epub then fb2 then any
 * acquisition link. Returns NULL if the entry has no acquisition link. */
const opds_link *opds_entry_best_acquisition(const opds_entry *entry);

/* ---- feed-level links & search ------------------------------------- */

/* href of the first feed-level link whose rel contains `rel_substr`
 * (case-insensitive), or NULL. Use "next"/"previous"/"start" for paging. */
const char *opds_feed_link_href(const opds_feed *feed, const char *rel_substr);

/* href of the feed's search link (rel contains "search"), or NULL. This may
 * be either a templated OPDS URL (contains "{searchTerms}") or an OpenSearch
 * description document — the caller decides how to resolve it (see type). */
const char *opds_feed_search_href(const opds_feed *feed);
const char *opds_feed_search_type(const opds_feed *feed);

/* Extract the best query template from an OpenSearch description document
 * (the XML a rel="search" link may point at). Prefers a <Url> whose type is
 * an Atom/OPDS feed and whose template contains "{searchTerms}". Returns a
 * heap string (caller frees) or NULL if none found. */
char *opds_opensearch_template(const char *xml, unsigned long len);

/* Substitute the query into an OpenSearch "{searchTerms}" template,
 * URL-encoding the query. Other {...} macros are dropped. Returns a heap
 * string (caller frees) or NULL on OOM. */
char *opds_apply_search_template(const char *tmpl, const char *query);

/* ---- URL resolution ------------------------------------------------ */

/* Resolve `href` (absolute, root-relative or relative) against `base` into
 * `out` (size `outsz`). Returns 0 on success, -1 if it did not fit. */
int opds_resolve_url(const char *base, const char *href, char *out, unsigned long outsz);

#endif /* INKSHELF_OPDS_H */
