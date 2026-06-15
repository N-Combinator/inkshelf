/*
 * test_opds.c — host unit tests for the XML parser and OPDS model.
 *
 * Pure C, no InkView / libcurl: compile and run on the build host with
 *   tests/run_host_tests.sh
 * exercising the parsing logic that backs the OPDS browser.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opds.h"
#include "xml.h"

static int g_fail;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); g_fail++; } \
} while (0)

static int STREQ(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

/* ---- a navigation feed (sections that drill into subfeeds) --------- */
static const char NAV_FEED[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:opds=\"http://opds-spec.org/2010/catalog\">\n"
"  <title>Example &amp; Co Catalog</title>\n"
"  <entry>\n"
"    <title>Popular</title>\n"
"    <link rel=\"subsection\" href=\"/opds/popular\" type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\"/>\n"
"  </entry>\n"
"  <entry>\n"
"    <title>By Author</title>\n"
"    <!-- a comment -->\n"
"    <link rel=\"subsection\" href=\"authors.xml\" type=\"application/atom+xml\"/>\n"
"  </entry>\n"
"</feed>\n";

/* ---- an acquisition feed (actual books) ---------------------------- */
static const char ACQ_FEED[] =
"<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:dc=\"http://purl.org/dc/terms/\">\n"
"  <title>Search results</title>\n"
"  <entry>\n"
"    <title>The &lt;Great&gt; Book</title>\n"
"    <author><name>Jane Doe</name></author>\n"
"    <summary>A &quot;summary&quot; with entities.</summary>\n"
"    <link rel=\"http://opds-spec.org/image/thumbnail\" href=\"/cover.png\" type=\"image/png\"/>\n"
"    <link rel=\"http://opds-spec.org/acquisition\" href=\"/dl/1.fb2\" type=\"application/x-fictionbook+xml\"/>\n"
"    <link rel=\"http://opds-spec.org/acquisition/open-access\" href=\"/dl/1.epub\" type=\"application/epub+zip\"/>\n"
"  </entry>\n"
"  <entry>\n"
"    <title><![CDATA[Raw <markup> & stuff]]></title>\n"
"    <author><name>John Smith</name></author>\n"
"    <link rel=\"http://opds-spec.org/acquisition\" href=\"http://cdn.example.com/2.epub\" type=\"application/epub+zip\"/>\n"
"  </entry>\n"
"</feed>\n";

static void test_nav_feed(void)
{
    printf("nav feed:\n");
    opds_feed *f = opds_parse(NAV_FEED, sizeof(NAV_FEED) - 1);
    CHECK(f != NULL, "parses");
    if (!f) return;

    CHECK(STREQ(f->title, "Example & Co Catalog"), "feed title entity-decoded");
    CHECK(f->entry_count == 2, "two entries");

    CHECK(STREQ(f->entries[0].title, "Popular"), "entry 0 title");
    CHECK(opds_entry_is_navigation(&f->entries[0]), "entry 0 is navigation");
    CHECK(!opds_entry_is_book(&f->entries[0]), "entry 0 not a book");
    CHECK(STREQ(opds_entry_subfeed_href(&f->entries[0]), "/opds/popular"),
          "entry 0 subfeed href");

    CHECK(opds_entry_is_navigation(&f->entries[1]), "entry 1 is navigation");
    CHECK(STREQ(opds_entry_subfeed_href(&f->entries[1]), "authors.xml"),
          "entry 1 relative subfeed href");

    opds_feed_free(f);
}

static void test_acq_feed(void)
{
    printf("acquisition feed:\n");
    opds_feed *f = opds_parse(ACQ_FEED, sizeof(ACQ_FEED) - 1);
    CHECK(f != NULL, "parses");
    if (!f) return;

    CHECK(f->entry_count == 2, "two entries");

    opds_entry *e0 = &f->entries[0];
    CHECK(STREQ(e0->title, "The <Great> Book"), "title entity-decoded");
    CHECK(STREQ(e0->author, "Jane Doe"), "author from author>name");
    CHECK(STREQ(e0->summary, "A \"summary\" with entities."), "summary decoded");
    CHECK(opds_entry_is_book(e0), "entry 0 is a book");
    CHECK(!opds_entry_is_navigation(e0), "book is not navigation");
    CHECK(e0->link_count == 3, "three links incl. thumbnail");

    const opds_link *best = opds_entry_best_acquisition(e0);
    CHECK(best != NULL, "has acquisition link");
    CHECK(best && STREQ(best->href, "/dl/1.epub"), "prefers epub over fb2");

    opds_entry *e1 = &f->entries[1];
    CHECK(STREQ(e1->title, "Raw <markup> & stuff"), "CDATA title preserved");
    const opds_link *best1 = opds_entry_best_acquisition(e1);
    CHECK(best1 && STREQ(best1->href, "http://cdn.example.com/2.epub"),
          "absolute acquisition href");

    opds_feed_free(f);
}

static void test_url_resolution(void)
{
    printf("url resolution:\n");
    char out[512];
    const char *base = "https://lib.example.com/opds/cat/root.xml?q=1";

    CHECK(opds_resolve_url(base, "http://other.com/x", out, sizeof(out)) == 0 &&
          STREQ(out, "http://other.com/x"), "absolute passthrough");
    CHECK(opds_resolve_url(base, "/dl/book.epub", out, sizeof(out)) == 0 &&
          STREQ(out, "https://lib.example.com/dl/book.epub"), "root-relative");
    CHECK(opds_resolve_url(base, "authors.xml", out, sizeof(out)) == 0 &&
          STREQ(out, "https://lib.example.com/opds/cat/authors.xml"), "path-relative");
    CHECK(opds_resolve_url("https://lib.example.com", "feed.xml", out, sizeof(out)) == 0 &&
          STREQ(out, "https://lib.example.com/feed.xml"), "no-path base");
    CHECK(opds_resolve_url(base, "x", out, 4) == -1, "overflow detected");
}

static void test_xml_edge_cases(void)
{
    printf("xml edge cases:\n");
    /* Numeric entities and self-closing root with attributes. */
    static const char doc[] =
        "<feed><title>caf&#233; &#x26; tea</title>"
        "<entry><title>A</title><link href=\"a\" rel=\"http://opds-spec.org/acquisition\"/></entry>"
        "</feed>";
    opds_feed *f = opds_parse(doc, sizeof(doc) - 1);
    CHECK(f != NULL, "parses");
    if (!f) return;
    /* café = c a f + U+00E9 (2 bytes UTF-8) then " & tea" */
    CHECK(STREQ(f->title, "caf\xC3\xA9 & tea"), "numeric (dec+hex) entities");
    CHECK(f->entry_count == 1 && opds_entry_is_book(&f->entries[0]),
          "self-closing acquisition link");
    opds_feed_free(f);
}

static const char SEARCH_FEED[] =
"<feed xmlns=\"http://www.w3.org/2005/Atom\">\n"
"  <title>Root</title>\n"
"  <link rel=\"search\" type=\"application/atom+xml\" href=\"/opds/search?q={searchTerms}\"/>\n"
"  <link rel=\"next\" type=\"application/atom+xml\" href=\"/opds/page/2\"/>\n"
"  <entry><title>Section</title>"
"    <link rel=\"subsection\" href=\"/s\" type=\"application/atom+xml\"/></entry>\n"
"</feed>\n";

static void test_feed_links_and_search(void)
{
    printf("feed links + search:\n");
    opds_feed *f = opds_parse(SEARCH_FEED, sizeof(SEARCH_FEED) - 1);
    CHECK(f != NULL, "parses");
    if (!f) return;

    CHECK(f->link_count == 2, "two feed-level links captured");
    CHECK(STREQ(opds_feed_search_href(f), "/opds/search?q={searchTerms}"),
          "search href found");
    CHECK(STREQ(opds_feed_link_href(f, "next"), "/opds/page/2"), "next href found");
    CHECK(opds_feed_link_href(f, "previous") == NULL, "no previous link");
    /* Entry link still works (regression on the links_add refactor). */
    CHECK(f->entry_count == 1 && opds_entry_is_navigation(&f->entries[0]),
          "entry-level link still parsed");

    /* OpenSearch description doc -> prefer the atom+xml template. */
    static const char OSD[] =
        "<OpenSearchDescription xmlns=\"http://a9.com/-/spec/opensearch/1.1/\">"
        "<Url type=\"text/html\" template=\"https://x/h?q={searchTerms}\"/>"
        "<Url type=\"application/atom+xml\" template=\"https://x/opds?q={searchTerms}\"/>"
        "</OpenSearchDescription>";
    char *tmpl = opds_opensearch_template(OSD, sizeof(OSD) - 1);
    CHECK(STREQ(tmpl, "https://x/opds?q={searchTerms}"), "opensearch desc -> atom template");
    free(tmpl);
    CHECK(opds_opensearch_template("<x/>", 4) == NULL, "no template -> NULL");

    char *u = opds_apply_search_template("/opds/search?q={searchTerms}", "tolkien lord");
    CHECK(STREQ(u, "/opds/search?q=tolkien%20lord"), "template subst + url-encode");
    free(u);

    u = opds_apply_search_template("/s?q={searchTerms}&i={startIndex?}", "a&b");
    CHECK(STREQ(u, "/s?q=a%26b&i="), "ampersand encoded, extra macro dropped");
    free(u);

    opds_feed_free(f);
}

static void test_malformed(void)
{
    printf("robustness:\n");
    opds_feed *f = opds_parse("not xml at all", 14);
    /* No tags -> empty feed, but must not crash and must free cleanly. */
    CHECK(f != NULL && f->entry_count == 0, "plain text -> empty feed");
    opds_feed_free(f);

    f = opds_parse("", 0);
    CHECK(f != NULL && f->entry_count == 0, "empty input -> empty feed");
    opds_feed_free(f);
}

int main(void)
{
    test_nav_feed();
    test_acq_feed();
    test_url_resolution();
    test_feed_links_and_search();
    test_xml_edge_cases();
    test_malformed();

    printf("\n%s\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
