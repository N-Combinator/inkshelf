/*
 * catalogs.h — the user's saved OPDS catalogs.
 *
 * A custom catalog address is typed on the on-screen keyboard, which is slow
 * on e-ink, so every address that opened successfully is remembered and
 * offered in the catalog picker next time. The list lives in its own file, by
 * default
 *   /mnt/ext1/system/config/inkshelf-catalogs.txt
 * one catalog per line as "URL<TAB>title" (the title is optional). It is kept
 * out of inkshelf.conf because a URL can be far longer than CONFIG_VALUE_MAX.
 * The file is plain text and safe to edit by hand over USB.
 *
 * Most recently used first. Writes are atomic (temp + rename).
 */

#ifndef INKSHELF_CATALOGS_H
#define INKSHELF_CATALOGS_H

#define CATALOGS_MAX       16
#define CATALOG_URL_MAX    1024
#define CATALOG_TITLE_MAX  128

typedef struct {
    char url[CATALOG_URL_MAX];
    char title[CATALOG_TITLE_MAX];   /* "" when the feed had no title */
} saved_catalog;

/* (Re)read the list from disk. A missing file is an empty list. */
void catalogs_load(void);

int catalogs_count(void);

/* Entry `idx` (0 = most recent), or NULL if out of range. The pointer is valid
 * until the next catalogs_* call that changes the list. */
const saved_catalog *catalogs_get(int idx);

/*
 * Remember `url` (surrounding whitespace is ignored) under `title` (may be
 * NULL), moving it to the front. An address already in the list is not
 * duplicated; it keeps its old title if `title` is empty. When the list is
 * full the least recently used entry is dropped.
 *
 * Returns 0 on success and -1 if the URL is empty, too long, or contains
 * control characters, or if the file could not be written — in the last case
 * the entry is still listed for the rest of the session.
 */
int catalogs_add(const char *url, const char *title);

/* Forget entry `idx`. Returns 0 on success, -1 on a bad index or I/O failure. */
int catalogs_remove(int idx);

/*
 * Override the file path (used by host tests) and forget the loaded list.
 * Pass NULL to reset to the built-in device default. The string is copied.
 */
void catalogs_set_path(const char *path);

#endif /* INKSHELF_CATALOGS_H */
