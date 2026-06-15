/*
 * library.h — on-device book library: where downloads go and what they are
 * named.
 *
 * The OPDS browser hands us a book's title, its acquisition URL and the MIME
 * type the server advertises. Turning that into a real file on the PocketBook
 * means two decisions kept here so they can be unit-tested on the host
 * without a device:
 *
 *   1. WHERE — the library directory. PocketBook exposes the SD card at
 *      /mnt/ext1; the reader scans /mnt/ext1/Books for content, so that is
 *      our default target. Overridable via the INKSHELF_LIBDIR environment
 *      variable (the host test harness points it at a temp dir).
 *
 *   2. WHAT NAME — a filesystem-safe name derived from the book title, with
 *      an extension chosen from the MIME type (falling back to the URL, then
 *      to .bin) so the reader recognises the format.
 *
 * None of this touches the network; the actual transfer is in download.c.
 */

#ifndef INKSHELF_LIBRARY_H
#define INKSHELF_LIBRARY_H

#include <stddef.h>

/* Write the library directory into `out` (no trailing slash). Returns 0 on
 * success, -1 if it did not fit. Honours $INKSHELF_LIBDIR, else the
 * PocketBook default /mnt/ext1/Books. */
int library_dir(char *out, size_t outsz);

/* Create the library directory if it does not exist. Parent components are
 * assumed to exist (they do on the device). Returns 0 if the directory
 * exists afterwards, -1 otherwise. */
int library_ensure_dir(const char *dir);

/* Pick a file extension (including the leading '.') for a downloaded book,
 * given the advertised MIME `mime` (may be NULL) and source `url` (may be
 * NULL). Always returns a non-empty extension, falling back to ".bin". */
const char *library_ext_for(const char *mime, const char *url);

/* Build a filesystem-safe file name (no directory part) for a book from its
 * `title`, MIME `mime` and `url`, into `out`. The base name is sanitised
 * (path separators and control/reserved characters replaced) and length-
 * limited; the extension comes from library_ext_for(). Returns 0 on success,
 * -1 if `out` is too small for even a minimal name. */
int library_build_filename(const char *title, const char *mime,
                           const char *url, char *out, size_t outsz);

/* Convenience: full "<library_dir>/<filename>" path for a book. Returns 0 on
 * success, -1 if it did not fit. */
int library_target_path(const char *title, const char *mime, const char *url,
                        char *out, size_t outsz);

#endif /* INKSHELF_LIBRARY_H */
