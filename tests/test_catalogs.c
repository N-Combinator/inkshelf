/*
 * test_catalogs.c — unit tests for the saved OPDS catalog list (catalogs.c):
 * persistence across a reload, ordering, de-duplication, the size cap, removal
 * and tolerance of a hand-edited file.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "catalogs.h"

static int g_fail;

#define CHECK(cond, msg) do {                                  \
    if (cond) { printf("  ok   %s\n", msg); }                  \
    else      { printf("  FAIL %s\n", msg); g_fail++; }        \
} while (0)

static int is_at(int idx, const char *url, const char *title)
{
    const saved_catalog *c = catalogs_get(idx);
    return c && strcmp(c->url, url) == 0 && strcmp(c->title, title) == 0;
}

int main(void)
{
    char tmpl[] = "/tmp/inkshelf_catalogs_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { perror("mkstemp"); return 1; }
    close(fd);
    unlink(tmpl);                       /* catalogs_add creates it from scratch */
    catalogs_set_path(tmpl);

    printf("saved catalogs:\n");
    CHECK(catalogs_count() == 0, "missing file is an empty list");
    CHECK(catalogs_get(0) == NULL, "get on an empty list -> NULL");

    CHECK(catalogs_add("http://192.168.1.20:8080/opds", "Calibre") == 0, "add first");
    CHECK(catalogs_add("  https://example.org/very/long/path/opds.xml \n", NULL) == 0,
          "add second (untitled, padded with whitespace)");
    CHECK(catalogs_count() == 2, "two entries");
    CHECK(is_at(0, "https://example.org/very/long/path/opds.xml", ""),
          "newest first, whitespace trimmed");
    CHECK(is_at(1, "http://192.168.1.20:8080/opds", "Calibre"), "older entry second");

    catalogs_load();                    /* drop the in-memory copy, re-read disk */
    CHECK(catalogs_count() == 2 &&
          is_at(0, "https://example.org/very/long/path/opds.xml", "") &&
          is_at(1, "http://192.168.1.20:8080/opds", "Calibre"),
          "list survives a reload from disk");

    CHECK(catalogs_add("http://192.168.1.20:8080/opds", "") == 0, "re-add existing");
    CHECK(catalogs_count() == 2, "re-adding does not duplicate");
    CHECK(is_at(0, "http://192.168.1.20:8080/opds", "Calibre"),
          "re-added entry moves to the front and keeps its title");
    CHECK(catalogs_add("http://192.168.1.20:8080/opds", "My\tLibrary\n") == 0 &&
          is_at(0, "http://192.168.1.20:8080/opds", "My Library"),
          "a new title replaces the old one; control characters flattened");

    CHECK(catalogs_add("", "x") == -1, "empty URL rejected");
    CHECK(catalogs_add("   ", "x") == -1, "blank URL rejected");
    CHECK(catalogs_add(NULL, "x") == -1, "NULL URL rejected");
    CHECK(catalogs_add("http://a/\tb", "x") == -1, "URL with a tab rejected");
    char huge[CATALOG_URL_MAX + 16];
    memset(huge, 'a', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';
    CHECK(catalogs_add(huge, "x") == -1, "over-long URL rejected");
    char longest[CATALOG_URL_MAX];
    memset(longest, 'b', sizeof longest - 1);
    longest[sizeof longest - 1] = '\0';
    CHECK(catalogs_add(longest, "Longest") == 0, "URL of the maximum length accepted");
    catalogs_load();
    CHECK(catalogs_count() == 3 && is_at(0, longest, "Longest"),
          "maximum-length URL survives a reload");
    CHECK(catalogs_remove(0) == 0 && catalogs_count() == 2, "remove the newest");

    CHECK(catalogs_remove(5) == -1 && catalogs_remove(-1) == -1, "bad index rejected");
    CHECK(catalogs_remove(1) == 0 && catalogs_count() == 1 &&
          is_at(0, "http://192.168.1.20:8080/opds", "My Library"),
          "remove keeps the other entry");
    catalogs_load();
    CHECK(catalogs_count() == 1, "removal survives a reload");

    /* Fill past the cap: the oldest entry is the one dropped. */
    char url[64];
    for (int i = 0; i < CATALOGS_MAX + 3; i++) {
        snprintf(url, sizeof url, "http://host%d/opds", i);
        catalogs_add(url, NULL);
    }
    snprintf(url, sizeof url, "http://host%d/opds", CATALOGS_MAX + 2);
    CHECK(catalogs_count() == CATALOGS_MAX, "list is capped");
    CHECK(is_at(0, url, ""), "newest entry kept at the front");
    CHECK(is_at(CATALOGS_MAX - 1, "http://host3/opds", ""), "oldest entries dropped");

    /* A file edited by hand: comments, blank lines, CRLF, a duplicate. */
    FILE *f = fopen(tmpl, "w");
    if (!f) { perror("fopen"); return 1; }
    fputs("# my catalogs\r\n\r\nhttp://one/opds\tOne\r\n"
          "http://two/opds\r\nhttp://one/opds\tAgain\r\n", f);
    fclose(f);
    catalogs_load();
    CHECK(catalogs_count() == 2 && is_at(0, "http://one/opds", "One") &&
          is_at(1, "http://two/opds", ""),
          "hand-edited file: comments, blank lines, CRLF and duplicates handled");

    unlink(tmpl);
    catalogs_set_path(NULL);
    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    printf("catalogs: all passed\n");
    return 0;
}
