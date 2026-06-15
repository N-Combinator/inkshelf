/*
 * http.c — libcurl-backed HTTP GET (see http.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "http.h"

#define HTTP_UA       "inkshelf/0.1 (PocketBook)"
#define HTTP_TIMEOUT  30L
#define HTTP_MAX_BODY (32 * 1024 * 1024)   /* 32 MB cap for in-memory feeds */

typedef struct {
    char *data;
    size_t len;
    int overflow;
} membuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    membuf *m = ud;
    size_t add = size * nmemb;

    if (m->len + add > HTTP_MAX_BODY) {
        m->overflow = 1;
        return 0;   /* abort transfer */
    }
    char *p = realloc(m->data, m->len + add + 1);
    if (!p) return 0;
    m->data = p;
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = '\0';
    return add;
}

static void set_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz) {
        strncpy(err, msg, errsz - 1);
        err[errsz - 1] = '\0';
    }
}

int http_get_mem(const char *url, char **out_buf, size_t *out_len,
                 char *err, size_t errsz)
{
    if (!url || !out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;

    CURL *curl = curl_easy_init();
    if (!curl) {
        set_err(err, errsz, "curl init failed");
        return -1;
    }

    membuf m = {0};
    char curlerr[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_UA);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlerr);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* allow gzip */

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (rc != CURLE_OK) {
        if (m.overflow)
            set_err(err, errsz, "response too large");
        else
            set_err(err, errsz, curlerr[0] ? curlerr : curl_easy_strerror(rc));
        free(m.data);
        curl_easy_cleanup(curl);
        return -1;
    }
    if (status >= 400) {
        char msg[HTTP_ERR_LEN];
        snprintf(msg, sizeof(msg), "HTTP %ld", status);
        set_err(err, errsz, msg);
        free(m.data);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_cleanup(curl);

    if (!m.data) {
        /* Empty but successful response. */
        m.data = calloc(1, 1);
        if (!m.data) { set_err(err, errsz, "out of memory"); return -1; }
    }
    *out_buf = m.data;
    *out_len = m.len;
    return 0;
}
