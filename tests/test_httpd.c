/*
 * test_httpd.c — host unit tests for the WiFi-drop upload server's parsing
 * core (httpd.c), with no sockets involved.
 *
 * The multipart receiver runs over an httpd_reader_fn, so here we feed it a
 * canned multipart/form-data body from memory — crucially in *small, awkward
 * chunks* so the streaming boundary matcher is exercised across read
 * boundaries (the case most likely to break a hand-rolled parser).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "httpd.h"
#include "config.h"

static int g_fail;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* ---- a memory-backed reader that hands out tiny chunks ------------- */

typedef struct {
    const unsigned char *data;
    size_t len, pos, chunk;
} memrd;

static long mem_reader(void *ud, char *buf, size_t n)
{
    memrd *m = ud;
    size_t avail = m->len - m->pos;
    if (avail == 0) return 0;
    size_t want = n;
    if (m->chunk && want > m->chunk) want = m->chunk;   /* force small reads */
    if (want > avail) want = avail;
    memcpy(buf, m->data + m->pos, want);
    m->pos += want;
    return (long)want;
}

/* Build a multipart/form-data body with the given file payload. */
static size_t build_body(unsigned char *out, size_t cap,
                         const char *boundary, const char *filename,
                         const unsigned char *payload, size_t plen)
{
    size_t n = 0;
    n += (size_t)snprintf((char *)out + n, cap - n,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        boundary, filename);
    memcpy(out + n, payload, plen); n += plen;
    n += (size_t)snprintf((char *)out + n, cap - n, "\r\n--%s--\r\n", boundary);
    return n;
}

/* Like build_body but with a caller-chosen part Content-Type (deploy tests). */
static size_t build_body_ct(unsigned char *out, size_t cap,
                            const char *boundary, const char *filename,
                            const char *ctype,
                            const unsigned char *payload, size_t plen)
{
    size_t n = 0;
    n += (size_t)snprintf((char *)out + n, cap - n,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n",
        boundary, filename, ctype);
    memcpy(out + n, payload, plen); n += plen;
    n += (size_t)snprintf((char *)out + n, cap - n, "\r\n--%s--\r\n", boundary);
    return n;
}

static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    *out_len = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

static void test_safe_name(void)
{
    printf("filename sanitising:\n");
    char out[256];

    httpd_safe_name("book.epub", out, sizeof out);
    CHECK(strcmp(out, "book.epub") == 0, "plain name kept");

    httpd_safe_name("../../etc/passwd", out, sizeof out);
    CHECK(strcmp(out, "passwd") == 0, "path traversal stripped");

    httpd_safe_name("C:\\Users\\me\\My Book.fb2", out, sizeof out);
    CHECK(strcmp(out, "My Book.fb2") == 0, "windows path stripped");

    httpd_safe_name("we:rd/na*me?.pdf", out, sizeof out);
    CHECK(strcmp(out, "na_me_.pdf") == 0, "unsafe chars collapsed to _");

    httpd_safe_name("", out, sizeof out);
    CHECK(strcmp(out, "book.bin") == 0, "empty -> default");

    httpd_safe_name("...", out, sizeof out);
    CHECK(strcmp(out, "book.bin") == 0, "dots-only -> default");
}

/* Drive one upload through the parser and assert the file landed intact. */
static void run_upload(const char *dir, const char *boundary,
                       const char *filename, const unsigned char *payload,
                       size_t plen, size_t chunk, const char *label)
{
    unsigned char body[1 << 20];
    size_t blen = build_body(body, sizeof body, boundary, filename, payload, plen);

    memrd rd = { .data = body, .len = blen, .pos = 0, .chunk = chunk };
    char ctype[128];
    snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", boundary);

    char name[256], err[256];
    unsigned long long bytes = 0;
    int rc = httpd_recv_multipart(mem_reader, &rd, ctype, dir,
                                  name, sizeof name, &bytes, err, sizeof err);

    char m[128];
    snprintf(m, sizeof m, "%s: parse ok", label);
    CHECK(rc == 0, m);
    if (rc != 0) { printf("       err=%s\n", err); return; }

    snprintf(m, sizeof m, "%s: byte count == payload", label);
    CHECK(bytes == plen, m);

    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t got_len = 0;
    unsigned char *got = slurp(path, &got_len);
    snprintf(m, sizeof m, "%s: file readable", label);
    CHECK(got != NULL, m);
    if (got) {
        snprintf(m, sizeof m, "%s: content byte-identical", label);
        CHECK(got_len == plen && memcmp(got, payload, plen) == 0, m);
        free(got);
    }
    unlink(path);
}

static void test_multipart(void)
{
    printf("multipart upload:\n");

    char tmpl[] = "/tmp/inkshelf_httpd_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { printf("  FAIL could not make temp dir\n"); g_fail++; return; }

    /* Small text payload, read whole. */
    const unsigned char txt[] = "Hello e-reader, this is a tiny book.";
    run_upload(dir, "----WebKitFormBoundaryABC123", "hello.txt",
               txt, sizeof txt - 1, 0, "small/one-read");

    /* Same payload but forced through 1-byte reads — stresses the streaming
     * boundary matcher across every possible split. */
    run_upload(dir, "----WebKitFormBoundaryABC123", "hello.txt",
               txt, sizeof txt - 1, 1, "small/1-byte-chunks");

    /* Binary payload containing bytes that partially match the delimiter
     * ("\r\n--") so the KMP fallback path is exercised, in 7-byte chunks. */
    unsigned char bin[5000];
    for (size_t i = 0; i < sizeof bin; i++) bin[i] = (unsigned char)(i * 31 + 7);
    /* sprinkle near-miss boundary prefixes */
    memcpy(bin + 100, "\r\n--not", 7);
    memcpy(bin + 2000, "\r\n-", 3);
    memcpy(bin + 4096, "\r\n--XYZ", 7);
    run_upload(dir, "XYZBOUNDARY", "blob.bin", bin, sizeof bin, 7,
               "binary/near-miss-boundaries");

    /* A larger payload (200 KB) in 4 KB chunks. */
    static unsigned char big[200 * 1024];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 7 + i / 13);
    run_upload(dir, "BIGBOUND", "novel.epub", big, sizeof big, 4096,
               "large/200KB");

    /* Collision: uploading the same name twice must not clobber. */
    run_upload(dir, "B", "dup.txt", txt, sizeof txt - 1, 0, "first dup");
    {
        unsigned char body[4096];
        size_t blen = build_body(body, sizeof body, "B", "dup.txt", txt, sizeof txt - 1);
        memrd rd = { .data = body, .len = blen, .pos = 0, .chunk = 0 };
        char name[256], err[256];
        unsigned long long bytes = 0;
        /* keep the first file in place this time */
        char p1[512]; snprintf(p1, sizeof p1, "%s/dup.txt", dir);
        FILE *f = fopen(p1, "wb"); if (f) { fwrite("x", 1, 1, f); fclose(f); }
        int rc = httpd_recv_multipart(mem_reader, &rd,
                    "multipart/form-data; boundary=B", dir,
                    name, sizeof name, &bytes, err, sizeof err);
        CHECK(rc == 0 && strcmp(name, "dup (1).txt") == 0,
              "name collision -> 'dup (1).txt'");
        char p2[512]; snprintf(p2, sizeof p2, "%s/%s", dir, name);
        unlink(p1); unlink(p2);
    }

    /* Missing boundary -> clean error, no crash. */
    {
        memrd rd = { .data = (const unsigned char *)"", .len = 0, .pos = 0, .chunk = 0 };
        char name[256], err[256];
        unsigned long long bytes = 0;
        int rc = httpd_recv_multipart(mem_reader, &rd, "text/plain", dir,
                                      name, sizeof name, &bytes, err, sizeof err);
        CHECK(rc == -1, "non-multipart content type rejected");
    }

    rmdir(dir);
}

static void test_deploy(void)
{
    printf("deploy endpoint:\n");

    char tmpl[] = "/tmp/inkshelf_deploy_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { printf("  FAIL could not make temp dir\n"); g_fail++; return; }

    const char *app = "inkshelf.app";
    char dest[512];
    snprintf(dest, sizeof dest, "%s/%s", dir, app);
    const char *ctype = "multipart/form-data; boundary=DB";

    /* Seed an "old" binary so we can prove deploy OVERWRITES the fixed target
     * (book-drop mode would instead create "inkshelf (1).app"). */
    FILE *f = fopen(dest, "wb");
    if (f) { fwrite("OLD-BINARY", 1, 10, f); fclose(f); }

    /* 1. Happy path — octet-stream part replaces the running app in place.
     *    Payload carries embedded NULs to confirm raw-binary handling. */
    const unsigned char newbin[] = "\x7f" "ELF\x00 new inkshelf bytes \x00\x01\x02\xff";
    size_t newlen = sizeof newbin;            /* include the embedded/trailing NULs */
    unsigned char body[4096];
    size_t blen = build_body_ct(body, sizeof body, "DB", "uploaded-name.app",
                                "application/octet-stream", newbin, newlen);
    memrd rd = { .data = body, .len = blen, .pos = 0, .chunk = 5 };
    char err[256];
    unsigned long long bytes = 0;
    int rc = httpd_recv_deploy(mem_reader, &rd, ctype, dir, app, &bytes, err, sizeof err);
    CHECK(rc == 0, "deploy: octet-stream upload accepted");
    if (rc != 0) printf("       err=%s\n", err);
    CHECK(bytes == newlen, "deploy: byte count == payload");

    size_t got_len = 0;
    unsigned char *got = slurp(dest, &got_len);
    CHECK(got && got_len == newlen && memcmp(got, newbin, newlen) == 0,
          "deploy: fixed target overwritten with new binary");
    free(got);
    struct stat stx;
    char coll[600];
    snprintf(coll, sizeof coll, "%s/inkshelf (1).app", dir);
    CHECK(stat(coll, &stx) != 0, "deploy: no collision-renamed copy left behind");

    /* 2. Wrong part mime is rejected and must not touch the installed binary. */
    const unsigned char other[] = "should-not-land";
    blen = build_body_ct(body, sizeof body, "DB", "x.app",
                         "text/plain", other, sizeof other - 1);
    memrd rd2 = { .data = body, .len = blen, .pos = 0, .chunk = 0 };
    rc = httpd_recv_deploy(mem_reader, &rd2, ctype, dir, app, &bytes, err, sizeof err);
    CHECK(rc == -1, "deploy: non-octet-stream part rejected");
    got = slurp(dest, &got_len);
    CHECK(got && got_len == newlen && memcmp(got, newbin, newlen) == 0,
          "deploy: target unchanged after rejected upload");
    free(got);

    /* 3. Size cap fires just over HTTPD_DEPLOY_MAX, target left intact. */
    size_t over = HTTPD_DEPLOY_MAX + 4096;
    unsigned char *big = malloc(over);
    unsigned char *bbody = malloc(over + 512);
    if (big && bbody) {
        memset(big, 0x5a, over);
        size_t bl = build_body_ct(bbody, over + 512, "DB", "big.app",
                                  "application/octet-stream", big, over);
        memrd rd3 = { .data = bbody, .len = bl, .pos = 0, .chunk = 65536 };
        rc = httpd_recv_deploy(mem_reader, &rd3, ctype, dir, app, &bytes, err, sizeof err);
        CHECK(rc == -1, "deploy: over-10MB upload rejected by size cap");
        got = slurp(dest, &got_len);
        CHECK(got && got_len == newlen, "deploy: target unchanged after oversize upload");
        free(got);
    } else {
        printf("  FAIL deploy: could not allocate oversize test buffers\n"); g_fail++;
    }
    free(big); free(bbody);

    unlink(dest);
    rmdir(dir);
}

static void test_local_ip(void)
{
    printf("local ipv4:\n");
    char ip[64];
    int rc = httpd_local_ipv4(ip, sizeof ip);
    /* On a CI host there may be no usable address; either way it must be safe
     * and NUL-terminated, and 0 implies a non-empty dotted quad. */
    CHECK(ip[0] != '\0', "ip string set");
    CHECK(rc == 0 || strcmp(ip, "0.0.0.0") == 0, "rc/value consistent");
    printf("       detected: %s (rc=%d)\n", ip, rc);
}

/* The PIN authorization predicate: open when no PIN is configured, otherwise
 * an exact match is required. This is the gate behind POST /drop and /deploy. */
static void test_pin_auth(void)
{
    printf("pin auth:\n");
    CHECK(httpd_pin_authorized(NULL, NULL) == 1, "no PIN set -> open (NULL/NULL)");
    CHECK(httpd_pin_authorized("", "whatever") == 1, "empty PIN -> open");
    CHECK(httpd_pin_authorized("1234", "1234") == 1, "exact match accepted");
    CHECK(httpd_pin_authorized("1234", "1235") == 0, "wrong PIN rejected");
    CHECK(httpd_pin_authorized("1234", "") == 0, "empty provided rejected when PIN set");
    CHECK(httpd_pin_authorized("1234", NULL) == 0, "missing header rejected when PIN set");
    CHECK(httpd_pin_authorized("1234", "12345") == 0, "prefix is not a match");
}

/* config.c round-trip against a temp file (never touches the device path). */
static void test_config(void)
{
    printf("config store:\n");
    char tmpl[] = "/tmp/inkshelf_conf_XXXXXX";
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0, "temp config path created");
    if (fd >= 0) close(fd);
    unlink(tmpl);                       /* config_set creates it from scratch */
    config_set_path(tmpl);

    char val[CONFIG_VALUE_MAX];
    CHECK(config_get("pin", val, sizeof val) == -1, "missing key -> -1");
    CHECK(val[0] == '\0', "missing key clears out buffer");

    CHECK(config_set("pin", "4271") == 0, "set creates file + key");
    CHECK(config_get("pin", val, sizeof val) == 0 && strcmp(val, "4271") == 0,
          "get returns the value just set");

    CHECK(config_set("other", "x") == 0, "second key set");
    CHECK(config_set("pin", "9999") == 0, "overwrite existing key");
    CHECK(config_get("pin", val, sizeof val) == 0 && strcmp(val, "9999") == 0,
          "overwrite wins");
    CHECK(config_get("other", val, sizeof val) == 0 && strcmp(val, "x") == 0,
          "unrelated key preserved across overwrite");

    unlink(tmpl);
    config_set_path(NULL);             /* restore device default */
}

int main(void)
{
    test_safe_name();
    test_multipart();
    test_deploy();
    test_local_ip();
    test_pin_auth();
    test_config();
    printf("\n%s\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
