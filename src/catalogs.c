/*
 * catalogs.c — saved OPDS catalogs (see catalogs.h).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "catalogs.h"

#ifndef INKSHELF_CATALOGS_PATH
#define INKSHELF_CATALOGS_PATH "/mnt/ext1/system/config/inkshelf-catalogs.txt"
#endif

#define CAT_PATH_MAX 512
#define CAT_LINE_MAX (CATALOG_URL_MAX + CATALOG_TITLE_MAX + 8)

static char g_path[CAT_PATH_MAX] = INKSHELF_CATALOGS_PATH;
static saved_catalog g_list[CATALOGS_MAX];
static int g_count;
static int g_loaded;

void catalogs_set_path(const char *path)
{
    snprintf(g_path, sizeof g_path, "%s",
             (path && path[0]) ? path : INKSHELF_CATALOGS_PATH);
    g_count = 0;
    g_loaded = 0;
}

/* Copy `src` into `dst` without leading/trailing whitespace. Returns -1 if the
 * result is empty, does not fit, or contains a control character. */
static int clean_url(const char *src, char *dst, size_t dstsz)
{
    if (!src) return -1;
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' ||
                     src[n - 1] == '\r' || src[n - 1] == '\n'))
        n--;
    if (n == 0 || n >= dstsz) return -1;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)src[i] < 0x20 || src[i] == 0x7f) return -1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}

/* Copy a feed title, flattening control characters (a tab or newline would
 * break the one-line-per-catalog file) and trimming the ends. */
static void clean_title(const char *src, char *dst, size_t dstsz)
{
    size_t n = 0;
    if (src) {
        while (*src && (unsigned char)*src <= 0x20) src++;
        for (; *src && n + 1 < dstsz; src++)
            dst[n++] = ((unsigned char)*src < 0x20 || *src == 0x7f) ? ' ' : *src;
        if (*src) {
            /* Truncated: don't leave half of a multi-byte UTF-8 character. */
            while (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0x80) n--;
            if (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0xC0) n--;
        }
    }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

static int find_url(const char *url)
{
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_list[i].url, url) == 0) return i;
    return -1;
}

void catalogs_load(void)
{
    g_count = 0;
    g_loaded = 1;

    FILE *f = fopen(g_path, "r");
    if (!f) return;

    char line[CAT_LINE_MAX];
    while (g_count < CATALOGS_MAX && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] != '\n' && !feof(f)) {
            /* over-long line: skip the rest of it rather than read the tail
             * as a catalog of its own */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        if (line[0] == '#') continue;

        char *title = strchr(line, '\t');
        if (title) *title++ = '\0';

        saved_catalog *e = &g_list[g_count];
        if (clean_url(line, e->url, sizeof e->url) != 0) continue;
        if (find_url(e->url) >= 0) continue;
        clean_title(title, e->title, sizeof e->title);
        g_count++;
    }
    fclose(f);
}

static void ensure_loaded(void)
{
    if (!g_loaded) catalogs_load();
}

/* mkdir -p of the directory holding g_path (best-effort). */
static void ensure_parent_dir(void)
{
    char dir[CAT_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", g_path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(dir, 0755); *p = '/'; }
    }
    mkdir(dir, 0755);
}

static int save(void)
{
    char tmp[CAT_PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", g_path) >= sizeof tmp)
        return -1;

    ensure_parent_dir();
    FILE *out = fopen(tmp, "w");
    if (!out) return -1;

    fputs("# inkshelf saved OPDS catalogs: URL<TAB>title, most recent first\n", out);
    for (int i = 0; i < g_count; i++) {
        if (g_list[i].title[0])
            fprintf(out, "%s\t%s\n", g_list[i].url, g_list[i].title);
        else
            fprintf(out, "%s\n", g_list[i].url);
    }

    if (fclose(out) != 0) { remove(tmp); return -1; }
    if (rename(tmp, g_path) != 0) { remove(tmp); return -1; }
    return 0;
}

int catalogs_count(void)
{
    ensure_loaded();
    return g_count;
}

const saved_catalog *catalogs_get(int idx)
{
    ensure_loaded();
    return (idx >= 0 && idx < g_count) ? &g_list[idx] : NULL;
}

int catalogs_add(const char *url, const char *title)
{
    ensure_loaded();

    saved_catalog e;
    if (clean_url(url, e.url, sizeof e.url) != 0) return -1;
    clean_title(title, e.title, sizeof e.title);

    int at = find_url(e.url);
    if (at >= 0) {
        if (!e.title[0])
            snprintf(e.title, sizeof e.title, "%s", g_list[at].title);
    } else if (g_count < CATALOGS_MAX) {
        at = g_count++;
    } else {
        at = CATALOGS_MAX - 1;          /* full: the oldest entry makes room */
    }

    memmove(&g_list[1], &g_list[0], (size_t)at * sizeof g_list[0]);
    g_list[0] = e;
    return save();
}

int catalogs_remove(int idx)
{
    ensure_loaded();
    if (idx < 0 || idx >= g_count) return -1;
    memmove(&g_list[idx], &g_list[idx + 1],
            (size_t)(g_count - idx - 1) * sizeof g_list[0]);
    g_count--;
    return save();
}
