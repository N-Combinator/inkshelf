/*
 * library.c — book library path + naming logic (see library.h).
 *
 * Deliberately free of InkView and libcurl so the whole file builds and runs
 * on the host test harness.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strncasecmp, strcasecmp */
#include <sys/stat.h>
#include <sys/types.h>

#include "library.h"

#define LIBRARY_DEFAULT_DIR "/mnt/ext1/Books"
#define LIBRARY_MAX_BASE    96      /* chars of title kept in the file name */

int library_dir(char *out, size_t outsz)
{
    const char *env = getenv("INKSHELF_LIBDIR");
    const char *dir = (env && env[0]) ? env : LIBRARY_DEFAULT_DIR;
    size_t n = strlen(dir);

    /* Drop any trailing slashes. */
    while (n > 1 && dir[n - 1] == '/') n--;
    if (n + 1 > outsz) return -1;
    memcpy(out, dir, n);
    out[n] = '\0';
    return 0;
}

int library_ensure_dir(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(dir, 0775) == 0)
        return 0;
    /* A racing creator may have won; re-check. */
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    return -1;
}

/* Case-insensitive substring test. */
static int ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return 1;
    }
    return 0;
}

/* Extension from URL path's last '.', validated as a short book extension. */
static const char *ext_from_url(const char *url)
{
    if (!url) return NULL;
    const char *end = url;
    while (*end && *end != '?' && *end != '#') end++;

    const char *dot = NULL;
    for (const char *p = url; p < end; p++) {
        if (*p == '/') dot = NULL;
        else if (*p == '.') dot = p;
    }
    if (!dot) return NULL;

    size_t len = (size_t)(end - dot);
    if (len < 2 || len > 6) return NULL;
    for (const char *p = dot + 1; p < end; p++)
        if (!isalnum((unsigned char)*p)) return NULL;

    static const char *known[] = {
        ".epub", ".fb2", ".pdf", ".mobi", ".azw", ".azw3",
        ".djvu", ".cbz", ".cbr", ".txt", ".zip", NULL
    };
    for (int i = 0; known[i]; i++)
        if (strcasecmp(dot, known[i]) == 0) return known[i];
    return NULL;
}

const char *library_ext_for(const char *mime, const char *url)
{
    if (mime && mime[0]) {
        if (ci_contains(mime, "epub"))                 return ".epub";
        if (ci_contains(mime, "fictionbook") ||
            ci_contains(mime, "fb2"))                  return ".fb2";
        if (ci_contains(mime, "mobipocket") ||
            ci_contains(mime, "mobi"))                 return ".mobi";
        if (ci_contains(mime, "pdf"))                  return ".pdf";
        if (ci_contains(mime, "djvu"))                 return ".djvu";
        if (ci_contains(mime, "x-cbr"))                return ".cbr";
        if (ci_contains(mime, "x-cbz") ||
            ci_contains(mime, "comicbook"))            return ".cbz";
        if (ci_contains(mime, "text/plain"))           return ".txt";
    }
    const char *ue = ext_from_url(url);
    if (ue) return ue;
    return ".bin";
}

/* True for characters safe in FAT/ext4 filenames. */
static int name_char_ok(unsigned char c)
{
    return isalnum(c) || c == ' ' || c == '-' || c == '_' ||
           c == '(' || c == ')' || c == '[' || c == ']' ||
           c == ',' || c == '.' || c == '\'' || c == '&' || c == '+';
}

int library_build_filename(const char *title, const char *mime,
                           const char *url, char *out, size_t outsz)
{
    const char *ext = library_ext_for(mime, url);
    size_t extlen = strlen(ext);

    if (outsz < extlen + 2) return -1;

    size_t cap = outsz - extlen - 1;
    if (cap > LIBRARY_MAX_BASE) cap = LIBRARY_MAX_BASE;

    size_t n = 0;
    int prev_us = 0;
    for (const char *p = title ? title : ""; *p && n < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (name_char_ok(c)) {
            out[n++] = (char)c;
            prev_us = 0;
        } else {
            if (!prev_us) { out[n++] = '_'; prev_us = 1; }
        }
    }
    /* Trim trailing awkward chars (FAT-unfriendly). */
    while (n > 0 && (out[n-1] == ' ' || out[n-1] == '.' || out[n-1] == '_'))
        n--;
    if (n == 0) {
        const char *fallback = "book";
        n = strlen(fallback);
        if (n > cap) n = cap;
        memcpy(out, fallback, n);
    }
    memcpy(out + n, ext, extlen);
    out[n + extlen] = '\0';
    return 0;
}

int library_target_path(const char *title, const char *mime, const char *url,
                        char *out, size_t outsz)
{
    char dir[512];
    char name[256];
    if (library_dir(dir, sizeof(dir)) != 0) return -1;
    if (library_build_filename(title, mime, url, name, sizeof(name)) != 0)
        return -1;

    size_t need = strlen(dir) + 1 + strlen(name) + 1;
    if (need > outsz) return -1;
    size_t dlen = strlen(dir);
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, strlen(name) + 1);
    return 0;
}
