/*
 * http.h — tiny HTTP client wrapper around libcurl.
 *
 * Used to fetch OPDS feeds into memory. libcurl ships in the PocketBook SDK
 * toolchain. Kept deliberately
 * small: blocking GETs with sane defaults (redirect following, timeout,
 * identifying User-Agent).
 */

#ifndef INKSHELF_HTTP_H
#define INKSHELF_HTTP_H

#include <stddef.h>

#include <curl/curl.h>

#define HTTP_ERR_LEN 160

/*
 * GET `url` into a freshly allocated, NUL-terminated buffer.
 * On success returns 0, sets *out_buf (caller frees) and *out_len.
 * On failure returns -1 and writes a message into err (if non-NULL).
 */
int http_get_mem(const char *url, char **out_buf, size_t *out_len,
                 char *err, size_t errsz);

/*
 * Apply inkshelf's TLS settings (see http.c for why peer verification is off)
 * to a libcurl handle. Call it on every handle that may fetch an https:// URL.
 */
void http_apply_tls(CURL *curl);

#endif /* INKSHELF_HTTP_H */
