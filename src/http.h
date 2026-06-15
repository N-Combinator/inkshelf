/*
 * http.h — tiny HTTP client wrapper around libcurl.
 *
 * Used to fetch OPDS feeds into memory (#3) and, in a later milestone, to
 * download book files to disk (#5). libcurl ships in the PocketBook SDK
 * toolchain. Kept deliberately small: blocking GETs with sane defaults
 * (redirect following, timeout, identifying User-Agent).
 */

#ifndef INKSHELF_HTTP_H
#define INKSHELF_HTTP_H

#include <stddef.h>

#define HTTP_ERR_LEN 160

/*
 * GET `url` into a freshly allocated, NUL-terminated buffer.
 * On success returns 0, sets *out_buf (caller frees) and *out_len.
 * On failure returns -1 and writes a message into err (if non-NULL).
 */
int http_get_mem(const char *url, char **out_buf, size_t *out_len,
                 char *err, size_t errsz);

#endif /* INKSHELF_HTTP_H */
