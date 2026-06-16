/*
 * httpd.c — embedded HTTP upload server (see httpd.h).
 *
 * Structure:
 *   - A background accept thread handles one connection at a time (an e-reader
 *     has one user; serial handling keeps memory flat and the code simple).
 *   - GET /            -> a minimal browser upload form.
 *   - POST / (multipart/form-data) -> stream the file part to the library.
 *   - The multipart reader runs over an httpd_reader_fn so it is unit-tested
 *     on the host with no socket (tests/test_httpd.c).
 *
 * The body parser locates the closing boundary with a streaming KMP match, so
 * a multi-megabyte book is never buffered whole in RAM; confirmed content is
 * flushed straight to disk as it goes.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "httpd.h"
#include "library.h"

#define HTTPD_RDBUF       65536
#define HTTPD_WRBUF       (1024 * 1024)   /* stdio write buffer for uploads */
#define HTTPD_LINE_MAX    2048
#define HTTPD_MAX_UPLOAD  (512ULL * 1024 * 1024)  /* refuse books over 512 MB */
#define HTTPD_BOUNDARY_MAX 200

/* POST /deploy writes the new app binary here, atomically over the running ELF
 * (.part + rename — ETXTBSY-safe). Kept self-contained so tests can't depend on
 * a real device path. */
#define DEPLOY_APP_DIR    "/mnt/ext1/applications"
#define DEPLOY_APP_NAME   "inkshelf.app"

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz) {
        strncpy(err, msg, errsz - 1);
        err[errsz - 1] = '\0';
    }
}

/* Case-insensitive substring search (strcasestr is a GNU extension we avoid
 * to keep -std=c11 + _POSIX_C_SOURCE clean). Returns a pointer into `hay`. */
static const char *ci_strstr(const char *hay, const char *needle)
{
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0) return p;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* buffered reader over an httpd_reader_fn                             */
/* ------------------------------------------------------------------ */

typedef struct {
    httpd_reader_fn raw;
    void           *ud;
    unsigned char   buf[HTTPD_RDBUF];
    size_t          pos, len;
} breader;

static void br_init(breader *b, httpd_reader_fn raw, void *ud)
{
    b->raw = raw;
    b->ud = ud;
    b->pos = b->len = 0;
}

/* Ensure at least one byte is available; return 1 if so, 0 at EOF, -1 error. */
static int br_fill(breader *b)
{
    if (b->pos < b->len) return 1;
    long r = b->raw(b->ud, (char *)b->buf, sizeof b->buf);
    if (r < 0) return -1;
    if (r == 0) return 0;
    b->pos = 0;
    b->len = (size_t)r;
    return 1;
}

/* Read one CRLF/LF-terminated line, sans terminator, NUL-terminated into out.
 * Returns line length, or -1 at EOF with nothing read. Overlong lines are
 * truncated to fit but fully consumed up to the newline. */
static long br_read_line(breader *b, char *out, size_t cap)
{
    size_t n = 0;
    int any = 0;
    for (;;) {
        int f = br_fill(b);
        if (f < 0) return -1;
        if (f == 0) break;
        unsigned char c = b->buf[b->pos++];
        any = 1;
        if (c == '\n') {
            if (n && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return (long)n;
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    if (!any) return -1;
    if (n && out[n - 1] == '\r') n--;
    out[n] = '\0';
    return (long)n;
}

/* ------------------------------------------------------------------ */
/* filename sanitising                                                 */
/* ------------------------------------------------------------------ */

static int name_char_ok(unsigned char c)
{
    return isalnum(c) || c == ' ' || c == '-' || c == '_' ||
           c == '(' || c == ')' || c == '[' || c == ']' ||
           c == ',' || c == '.' || c == '\'' || c == '&' || c == '+';
}

int httpd_safe_name(const char *raw, char *out, size_t outsz)
{
    if (outsz < 8) return -1;

    /* Drop any directory part the client may have sent (../, C:\, etc.). */
    const char *base = raw ? raw : "";
    for (const char *p = base; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;

    size_t n = 0;
    int prev_us = 0;
    for (const char *p = base; *p && n + 1 < outsz && n < HTTPD_NAME_MAX - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (name_char_ok(c)) {
            out[n++] = (char)c;
            prev_us = 0;
        } else if (!prev_us) {
            out[n++] = '_';
            prev_us = 1;
        }
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '_'))
        n--;
    out[n] = '\0';

    /* A name with no alphanumeric character (empty, ".", "...", "__") is not a
     * usable book file — fall back to a safe default. */
    int has_alnum = 0;
    for (size_t i = 0; i < n; i++)
        if (isalnum((unsigned char)out[i])) { has_alnum = 1; break; }
    if (!has_alnum)
        strcpy(out, "book.bin");
    return 0;
}

/* Pull filename="..." out of a Content-Disposition header value. */
static int parse_disposition_filename(const char *line, char *out, size_t outsz)
{
    const char *p = line;
    const char *key = "filename=";
    size_t klen = strlen(key);
    while (*p) {
        if (strncasecmp(p, key, klen) == 0) {
            p += klen;
            size_t n = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && n + 1 < outsz) out[n++] = *p++;
            } else {
                while (*p && *p != ';' && *p != ' ' && n + 1 < outsz) out[n++] = *p++;
            }
            out[n] = '\0';
            return n ? 0 : -1;
        }
        p++;
    }
    return -1;
}

/* Extract the boundary token from a Content-Type header value. */
static int parse_boundary(const char *content_type, char *out, size_t outsz)
{
    if (!content_type) return -1;
    const char *p = ci_strstr(content_type, "boundary=");
    if (!p) return -1;
    p += strlen("boundary=");
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < outsz) out[n++] = *p++;
    } else {
        while (*p && *p != ';' && *p != ' ' && *p != '\r' && n + 1 < outsz)
            out[n++] = *p++;
    }
    out[n] = '\0';
    return n ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* streaming multipart body -> file                                    */
/* ------------------------------------------------------------------ */

/* KMP prefix function for the delimiter. */
static void kmp_prefix(const unsigned char *p, size_t n, int *pi)
{
    pi[0] = 0;
    size_t k = 0;
    for (size_t i = 1; i < n; i++) {
        while (k > 0 && p[i] != p[k]) k = (size_t)pi[k - 1];
        if (p[i] == p[k]) k++;
        pi[i] = (int)k;
    }
}

/*
 * Consume bytes from `b` until the delimiter `delim` (length dl) is matched,
 * writing everything before it to `fp` (NULL = discard). Returns 0 when the
 * delimiter was found, -1 on read/write error or EOF before the delimiter.
 *
 * Uses streaming KMP: at any point `m` bytes of the delimiter are tentatively
 * matched and equal delim[0..m); when the match breaks, the bytes that slide
 * out of the window are known content and are reconstructed from `delim`
 * itself, so no input has to be buffered beyond the read chunk.
 */
static int consume_to_delim(breader *b, const unsigned char *delim, size_t dl,
                            const int *pi, FILE *fp,
                            unsigned long long *written,
                            unsigned long long max_upload,
                            char *err, size_t errsz)
{
    size_t m = 0;
    for (;;) {
        int f = br_fill(b);
        if (f < 0) { set_err(err, errsz, "read error"); return -1; }
        if (f == 0) { set_err(err, errsz, "upload truncated (no closing boundary)"); return -1; }

        while (b->pos < b->len) {
            /* Fast path: nothing matched -> bulk-skip to the next delim[0]. */
            if (m == 0) {
                size_t start = b->pos;
                while (b->pos < b->len && b->buf[b->pos] != delim[0]) b->pos++;
                size_t run = b->pos - start;
                if (run) {
                    if (fp && fwrite(b->buf + start, 1, run, fp) != run) {
                        set_err(err, errsz, "write failed (disk full?)");
                        return -1;
                    }
                    *written += run;
                    if (*written > max_upload) {
                        set_err(err, errsz, "upload exceeds size limit");
                        return -1;
                    }
                }
                if (b->pos >= b->len) break;
            }

            unsigned char c = b->buf[b->pos++];
            while (m > 0 && c != delim[m]) {
                size_t m2 = (size_t)pi[m - 1];
                size_t emit = m - m2;            /* these bytes were content */
                if (emit) {
                    if (fp && fwrite(delim, 1, emit, fp) != emit) {
                        set_err(err, errsz, "write failed (disk full?)");
                        return -1;
                    }
                    *written += emit;
                }
                m = m2;
            }
            if (c == delim[m]) {
                m++;
                if (m == dl) return 0;            /* delimiter complete */
            } else {
                if (fp && fputc(c, fp) == EOF) {
                    set_err(err, errsz, "write failed (disk full?)");
                    return -1;
                }
                *written += 1;
                if (*written > max_upload) {
                    set_err(err, errsz, "upload exceeds size limit");
                    return -1;
                }
            }
        }
    }
}

/* Resolve a non-clobbering target path: "<dir>/<name>", "<dir>/<base> (1).ext"... */
static int unique_target(const char *dir, const char *name,
                         char *out, size_t outsz)
{
    if ((size_t)snprintf(out, outsz, "%s/%s", dir, name) >= outsz) return -1;
    struct stat st;
    if (stat(out, &st) != 0) return 0;            /* free */

    /* Split name into base + extension. */
    const char *dot = strrchr(name, '.');
    char base[HTTPD_NAME_MAX], ext[32];
    if (dot && dot != name) {
        size_t bl = (size_t)(dot - name);
        if (bl >= sizeof base) bl = sizeof base - 1;
        memcpy(base, name, bl);
        base[bl] = '\0';
        snprintf(ext, sizeof ext, "%s", dot);
    } else {
        snprintf(base, sizeof base, "%s", name);
        ext[0] = '\0';
    }
    for (int i = 1; i < 1000; i++) {
        if ((size_t)snprintf(out, outsz, "%s/%s (%d)%s", dir, base, i, ext) >= outsz)
            return -1;
        if (stat(out, &st) != 0) return 0;
    }
    return -1;
}

/* Core multipart receiver, operating on an already-initialised breader so the
 * socket handler can keep reading from the same buffered stream after parsing
 * the request headers. The public reader_fn entry point below wraps this. */
/*
 * force_name    NULL  -> library mode: derive a sanitised, non-clobbering name
 *                        from the upload's filename (book drop).
 *               !NULL -> deploy mode: write straight to "<dest_dir>/<force_name>",
 *                        overwriting via .part + rename (ETXTBSY-safe).
 * require_octet  in deploy mode, reject parts whose Content-Type isn't
 *                application/octet-stream.
 * max_upload     hard byte cap for the streamed part.
 */
static int recv_multipart_br(breader *bp,
                             const char *content_type, const char *dest_dir,
                             const char *force_name, int require_octet,
                             unsigned long long max_upload,
                             char *out_name, size_t out_name_sz,
                             unsigned long long *out_bytes,
                             char *err, size_t errsz)
{
    char boundary[HTTPD_BOUNDARY_MAX];
    if (parse_boundary(content_type, boundary, sizeof boundary) != 0) {
        set_err(err, errsz, "missing multipart boundary");
        return -1;
    }

    /* The opening delimiter is "--<boundary>"; subsequent parts and the close
     * are preceded by CRLF: "\r\n--<boundary>". */
    char opener[HTTPD_BOUNDARY_MAX + 4];
    snprintf(opener, sizeof opener, "--%s", boundary);

    unsigned char delim[HTTPD_BOUNDARY_MAX + 8];
    int pi[HTTPD_BOUNDARY_MAX + 8];
    int dl = snprintf((char *)delim, sizeof delim, "\r\n--%s", boundary);
    if (dl <= 0) { set_err(err, errsz, "bad boundary"); return -1; }
    kmp_prefix(delim, (size_t)dl, pi);

    /* Scan lines until the opening boundary, then read part headers; loop over
     * parts until we find one carrying a filename. */
    char line[HTTPD_LINE_MAX];
    int seen_opener = 0;
    char filename[HTTPD_NAME_MAX] = {0};
    char part_ctype[128] = {0};

    for (;;) {
        long n = br_read_line(bp, line, sizeof line);
        if (n < 0) { set_err(err, errsz, "malformed upload (no parts)"); return -1; }

        if (!seen_opener) {
            if (strcmp(line, opener) == 0) seen_opener = 1;
            continue;
        }

        /* In part headers now. Collect until the blank line. */
        if (strncasecmp(line, "Content-Disposition:", 20) == 0)
            parse_disposition_filename(line, filename, sizeof filename);
        if (strncasecmp(line, "Content-Type:", 13) == 0) {
            const char *v = line + 13;
            while (*v == ' ') v++;
            snprintf(part_ctype, sizeof part_ctype, "%s", v);
        }

        if (n == 0) {
            /* End of this part's headers; body follows. */
            if (filename[0]) break;               /* this is our file part */

            /* A non-file field: skip its body to the next boundary, then keep
             * looking for a file part. */
            unsigned long long skip = 0;
            if (consume_to_delim(bp, delim, (size_t)dl, pi, NULL, &skip,
                                 max_upload, err, errsz) != 0)
                return -1;
            /* After the delimiter: "--" ends the body, "\r\n" precedes a part. */
            int c1 = br_fill(bp) > 0 ? bp->buf[bp->pos] : -1;
            if (c1 == '-') { set_err(err, errsz, "no file in upload"); return -1; }
            seen_opener = 1;                       /* next lines are part headers */
            filename[0] = '\0';
            part_ctype[0] = '\0';
            continue;
        }
    }

    /* Deploy mode rejects a part that doesn't declare itself a raw binary. This
     * is a sanity gate, not real auth — the Content-Type is client-supplied. */
    if (require_octet && ci_strstr(part_ctype, "application/octet-stream") == NULL) {
        set_err(err, errsz, "deploy requires application/octet-stream");
        return -1;
    }

    /* Stream the file part's body straight to "<target>.part". In deploy mode
     * the target is fixed (overwrite the app); in library mode it is a
     * sanitised, non-clobbering name derived from the upload. */
    char target[1024];
    if (force_name) {
        if ((size_t)snprintf(target, sizeof target, "%s/%s", dest_dir, force_name)
                >= sizeof target) {
            set_err(err, errsz, "cannot build target path");
            return -1;
        }
    } else {
        char safe[HTTPD_NAME_MAX];
        httpd_safe_name(filename, safe, sizeof safe);
        if (unique_target(dest_dir, safe, target, sizeof target) != 0) {
            set_err(err, errsz, "cannot build target path");
            return -1;
        }
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.part", target);

    FILE *fp = fopen(tmp, "wb");
    if (!fp) { set_err(err, errsz, "cannot open output file"); return -1; }

    /* Stock stdio flushes every BUFSIZ (~4-8 KB), so a multi-MB book turns into
     * thousands of tiny writes to the reader's flash and uploads crawl. Give the
     * stream a large fully-buffered (_IOFBF) buffer so the 64 KB read chunks
     * coalesce into a few big flash writes. Best-effort: if the allocation fails
     * we just keep stdio's default buffer. The buffer must outlive the stream,
     * so it is freed only after fclose(). */
    char *wrbuf = malloc(HTTPD_WRBUF);
    if (wrbuf) setvbuf(fp, wrbuf, _IOFBF, HTTPD_WRBUF);

    unsigned long long written = 0;
    if (consume_to_delim(bp, delim, (size_t)dl, pi, fp, &written,
                         max_upload, err, errsz) != 0) {
        fclose(fp);
        free(wrbuf);
        remove(tmp);
        return -1;
    }
    if (fclose(fp) != 0) {
        free(wrbuf);
        remove(tmp);
        set_err(err, errsz, "write failed on close");
        return -1;
    }
    free(wrbuf);
    if (written == 0) {
        remove(tmp);
        set_err(err, errsz, "empty file");
        return -1;
    }
    if (rename(tmp, target) != 0) {
        remove(tmp);
        set_err(err, errsz, "rename into library failed");
        return -1;
    }

    /* Return the base name (after the last '/'). */
    const char *base = target;
    for (const char *p = target; *p; p++)
        if (*p == '/') base = p + 1;
    if (out_name && out_name_sz) {
        strncpy(out_name, base, out_name_sz - 1);
        out_name[out_name_sz - 1] = '\0';
    }
    if (out_bytes) *out_bytes = written;
    return 0;
}

int httpd_recv_multipart(httpd_reader_fn reader, void *ud,
                         const char *content_type, const char *dest_dir,
                         char *out_name, size_t out_name_sz,
                         unsigned long long *out_bytes,
                         char *err, size_t errsz)
{
    breader b;
    br_init(&b, reader, ud);
    return recv_multipart_br(&b, content_type, dest_dir,
                             NULL, 0, HTTPD_MAX_UPLOAD,
                             out_name, out_name_sz, out_bytes, err, errsz);
}

int httpd_recv_deploy(httpd_reader_fn reader, void *ud,
                      const char *content_type, const char *dest_dir,
                      const char *app_name,
                      unsigned long long *out_bytes,
                      char *err, size_t errsz)
{
    breader b;
    br_init(&b, reader, ud);
    char name[HTTPD_NAME_MAX];
    return recv_multipart_br(&b, content_type, dest_dir,
                             app_name, 1, HTTPD_DEPLOY_MAX,
                             name, sizeof name, out_bytes, err, errsz);
}

/* ------------------------------------------------------------------ */
/* LAN IPv4 discovery                                                  */
/* ------------------------------------------------------------------ */

static int is_private_v4(unsigned long a)   /* a in host byte order */
{
    return (a >> 24) == 10 ||
           ((a >> 20) == 0xAC1) ||           /* 172.16.0.0/12 */
           ((a >> 16) == 0xC0A8);            /* 192.168.0.0/16 */
}

int httpd_local_ipv4(char *out, size_t outsz)
{
    if (out && outsz) snprintf(out, outsz, "0.0.0.0");

    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return -1;

    char best[INET_ADDRSTRLEN] = {0};
    int  best_private = 0;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
        unsigned long a = ntohl(sin->sin_addr.s_addr);
        if (a == 0 || (a >> 24) == 127) continue;     /* skip 0.0.0.0 / loopback */

        char tmp[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sin->sin_addr, tmp, sizeof tmp)) continue;

        int priv = is_private_v4(a);
        if (!best[0] || (priv && !best_private)) {
            snprintf(best, sizeof best, "%s", tmp);
            best_private = priv;
        }
        if (best_private) break;                       /* private is good enough */
    }
    freeifaddrs(ifa);

    if (!best[0]) return -1;
    if (out && outsz) snprintf(out, outsz, "%s", best);
    return 0;
}

/* ------------------------------------------------------------------ */
/* server thread + connection handling                                 */
/* ------------------------------------------------------------------ */

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    int             listen_fd;
    int             running;
    int             started;
    httpd_status_t  st;
    char            pin[HTTPD_PIN_MAX];     /* "" = gate disabled */
} S = { .listen_fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

int httpd_pin_authorized(const char *configured, const char *provided)
{
    if (!configured || !configured[0]) return 1;      /* no PIN set => open */
    if (!provided) return 0;
    return strcmp(configured, provided) == 0;
}

void httpd_set_pin(const char *pin)
{
    pthread_mutex_lock(&S.lock);
    snprintf(S.pin, sizeof S.pin, "%s", pin ? pin : "");
    pthread_mutex_unlock(&S.lock);
}

/* Authorize a request against the live PIN (snapshot under lock). */
static int pin_ok(const char *provided)
{
    char cur[HTTPD_PIN_MAX];
    pthread_mutex_lock(&S.lock);
    snprintf(cur, sizeof cur, "%s", S.pin);
    pthread_mutex_unlock(&S.lock);
    return httpd_pin_authorized(cur, provided);
}

static long sock_read(void *ud, char *buf, size_t n)
{
    int fd = *(int *)ud;
    ssize_t r = recv(fd, buf, n, 0);
    if (r < 0) return -1;
    return (long)r;
}

static void send_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(fd, data + off, len - off, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void send_response(int fd, const char *status, const char *ctype,
                          const char *body)
{
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     status, ctype, strlen(body));
    if (n > 0) send_all(fd, head, (size_t)n);
    send_all(fd, body, strlen(body));
}

/* The upload page POSTs via fetch() so it can attach the X-Inkshelf-PIN header
 * (a plain <form> cannot set custom headers). Vanilla JS, no dependencies. */
static const char UPLOAD_FORM[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>inkshelf \xE2\x80\x94 WiFi Book Drop</title>"
    "<style>body{font-family:sans-serif;max-width:34em;margin:3em auto;padding:0 1em}"
    "h1{font-size:1.4em}input{display:block;margin:.6em 0}"
    "#pin{font-size:1.2em;letter-spacing:.3em;width:6em}"
    "button{font-size:1em;padding:.5em 1.2em}"
    "#out{margin-top:1em;white-space:pre-wrap}</style></head><body>"
    "<h1>Drop a book onto your PocketBook</h1>"
    "<p>Enter the PIN shown on the reader, choose a book file and upload it. "
    "It will appear in your library.</p>"
    "<form id=f>"
    "<label>PIN <input id=pin inputmode=numeric maxlength=4 autocomplete=off></label>"
    "<input type=file id=file required>"
    "<button type=submit>Upload</button></form>"
    "<div id=out></div>"
    "<script>"
    "var f=document.getElementById('f');"
    "f.onsubmit=function(e){e.preventDefault();"
    "var fl=document.getElementById('file').files[0];"
    "if(!fl){return;}"
    "var fd=new FormData();fd.append('file',fl);"
    "var o=document.getElementById('out');o.textContent='Uploading\\u2026';"
    "fetch('/drop',{method:'POST',headers:{'X-Inkshelf-PIN':"
    "document.getElementById('pin').value},body:fd})"
    ".then(function(r){return r.text().then(function(t){"
    "o.textContent=(r.ok?'\\u2713 ':'\\u2717 ')+t;});})"
    ".catch(function(err){o.textContent='\\u2717 '+err;});};"
    "</script></body></html>";

static void record_upload(const char *name, unsigned long long bytes)
{
    pthread_mutex_lock(&S.lock);
    S.st.uploads++;
    snprintf(S.st.last_file, sizeof S.st.last_file, "%s", name);
    S.st.last_bytes = bytes;
    S.st.last_error[0] = '\0';
    pthread_mutex_unlock(&S.lock);
}

static void record_error(const char *msg)
{
    pthread_mutex_lock(&S.lock);
    snprintf(S.st.last_error, sizeof S.st.last_error, "%s", msg);
    pthread_mutex_unlock(&S.lock);
}

/* Relaunch inkshelf after a /deploy. We are running *inside* inkshelf.app, so we
 * cannot kill+exec inline — we would die before the new binary starts and the
 * client would never get its 200. Instead detach a shell (reparented to init,
 * named "sh" so killall misses it) that waits for this response to flush, kills
 * the old app, and launches the freshly written one. Best-effort UX only: the
 * new binary is already swapped in regardless, so if the relaunch doesn't take
 * the user just taps the icon. */
static void deploy_schedule_restart(void)
{
    int rc = system("(sleep 2; killall " DEPLOY_APP_NAME " 2>/dev/null; sleep 1; "
                     DEPLOY_APP_DIR "/" DEPLOY_APP_NAME " >/dev/null 2>&1 &) "
                     ">/dev/null 2>&1 &");
    (void)rc;
}

/* Free the listening port the instant we answer a /deploy, rather than waiting
 * for the relaunch shell to kill this process (~2s later) and letting the kernel
 * reclaim the socket. Without this, the freshly relaunched inkshelf races the
 * old one for port 8080 and the WiFi-drop screen reports "port already in use".
 *
 * We run inside serve_thread here, so we must NOT join it (self-join deadlocks):
 * we just drop the socket and clear `running`, and the accept loop falls out on
 * its own. Detach self so the exiting thread is reaped without a later join. */
static void deploy_release_port(void)
{
    pthread_mutex_lock(&S.lock);
    int lfd = S.listen_fd;
    S.running = 0;
    S.st.running = 0;
    S.listen_fd = -1;
    S.started = 0;
    pthread_mutex_unlock(&S.lock);

    if (lfd >= 0) {
        shutdown(lfd, SHUT_RDWR);
        close(lfd);
    }
    pthread_detach(pthread_self());
}

static void handle_conn(int cfd)
{
    breader b;
    br_init(&b, sock_read, &cfd);

    char line[HTTPD_LINE_MAX];
    if (br_read_line(&b, line, sizeof line) < 0) return;

    char method[8] = {0};
    char path[256] = {0};
    sscanf(line, "%7s %255s", method, path);

    /* Read headers, capturing the ones we need. */
    char content_type[512] = {0};
    char req_pin[HTTPD_PIN_MAX] = {0};
    for (;;) {
        long n = br_read_line(&b, line, sizeof line);
        if (n <= 0) break;                          /* blank line or EOF */
        if (strncasecmp(line, "Content-Type:", 13) == 0) {
            const char *v = line + 13;
            while (*v == ' ') v++;
            snprintf(content_type, sizeof content_type, "%s", v);
        } else if (strncasecmp(line, "X-Inkshelf-PIN:", 15) == 0) {
            const char *v = line + 15;
            while (*v == ' ') v++;
            snprintf(req_pin, sizeof req_pin, "%s", v);
        }
    }

    if (strcasecmp(method, "GET") == 0) {
        send_response(cfd, "200 OK", "text/html; charset=utf-8", UPLOAD_FORM);
        return;
    }
    if (strcasecmp(method, "POST") != 0) {
        send_response(cfd, "405 Method Not Allowed", "text/plain", "Method not allowed\n");
        return;
    }

    /* Every mutating request must carry the right PIN (no-op when none is set). */
    if (!pin_ok(req_pin)) {
        send_response(cfd, "403 Forbidden", "text/plain", "Wrong or missing PIN\n");
        return;
    }

    /* POST /deploy: overwrite the running app binary and restart. Separate from
     * the book-drop path — fixed target, octet-stream + 10 MB guards. */
    if (strncmp(path, "/deploy", 7) == 0) {
        if (ci_strstr(content_type, "multipart/form-data") == NULL) {
            send_response(cfd, "400 Bad Request", "text/plain",
                          "Expected multipart/form-data\n");
            return;
        }
        char dname[HTTPD_NAME_MAX], derr[HTTPD_ERR_MAX];
        unsigned long long dbytes = 0;
        if (recv_multipart_br(&b, content_type, DEPLOY_APP_DIR,
                              DEPLOY_APP_NAME, 1, HTTPD_DEPLOY_MAX,
                              dname, sizeof dname, &dbytes, derr, sizeof derr) == 0) {
            send_response(cfd, "200 OK", "text/plain",
                          "Deployed. inkshelf will restart.\n");
            /* Release :8080 now so the relaunched binary can bind it cleanly,
             * then schedule the kill+relaunch. */
            deploy_release_port();
            deploy_schedule_restart();
        } else {
            record_error(derr);
            char body[HTTPD_ERR_MAX + 32];
            snprintf(body, sizeof body, "Deploy failed: %s\n", derr);
            send_response(cfd, "500 Internal Server Error", "text/plain", body);
        }
        return;
    }

    if (ci_strstr(content_type, "multipart/form-data") == NULL) {
        send_response(cfd, "400 Bad Request", "text/plain",
                      "Expected multipart/form-data\n");
        return;
    }

    char dir[600];
    if (library_dir(dir, sizeof dir) != 0 || library_ensure_dir(dir) != 0) {
        record_error("library directory unavailable");
        send_response(cfd, "500 Internal Server Error", "text/plain",
                      "Library directory unavailable\n");
        return;
    }

    char name[HTTPD_NAME_MAX], err[HTTPD_ERR_MAX];
    unsigned long long bytes = 0;

    /* `b` already holds any body bytes read past the header blank line; hand
     * the very same breader to the multipart parser so it continues from
     * exactly where header parsing stopped (no bytes dropped). The page POSTs
     * here via fetch() and shows the plain-text reply inline. */
    if (recv_multipart_br(&b, content_type, dir,
                          NULL, 0, HTTPD_MAX_UPLOAD,
                          name, sizeof name, &bytes, err, sizeof err) == 0) {
        record_upload(name, bytes);
        char msg[400];
        snprintf(msg, sizeof msg, "Saved %s (%llu bytes) to your library.\n",
                 name, bytes);
        send_response(cfd, "200 OK", "text/plain; charset=utf-8", msg);
    } else {
        record_error(err);
        char msg[HTTPD_ERR_MAX + 16];
        snprintf(msg, sizeof msg, "%s\n", err);
        send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", msg);
    }
}

static void *serve_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&S.lock);
        int run = S.running;
        int lfd = S.listen_fd;
        pthread_mutex_unlock(&S.lock);
        if (!run) break;

        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;                                  /* socket closed -> stop */
        }
        handle_conn(cfd);
        close(cfd);
    }
    return NULL;
}

int httpd_start(int port, char *err, size_t errsz)
{
    pthread_mutex_lock(&S.lock);
    if (S.running) { pthread_mutex_unlock(&S.lock); return 0; }
    pthread_mutex_unlock(&S.lock);

    if (port <= 0) port = HTTPD_DEFAULT_PORT;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_err(err, errsz, "socket() failed"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        set_err(err, errsz, "port already in use");
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0) {
        set_err(err, errsz, "listen() failed");
        close(fd);
        return -1;
    }

    char ip[HTTPD_IP_MAX];
    httpd_local_ipv4(ip, sizeof ip);

    pthread_mutex_lock(&S.lock);
    S.listen_fd = fd;
    S.running = 1;
    memset(&S.st, 0, sizeof S.st);
    S.st.running = 1;
    S.st.port = port;
    snprintf(S.st.ip, sizeof S.st.ip, "%s", ip);
    pthread_mutex_unlock(&S.lock);

    if (pthread_create(&S.thread, NULL, serve_thread, NULL) != 0) {
        set_err(err, errsz, "cannot start server thread");
        pthread_mutex_lock(&S.lock);
        S.running = 0;
        S.st.running = 0;
        S.listen_fd = -1;
        pthread_mutex_unlock(&S.lock);
        close(fd);
        return -1;
    }
    S.started = 1;
    return 0;
}

void httpd_stop(void)
{
    pthread_mutex_lock(&S.lock);
    int was = S.running;
    int fd = S.listen_fd;
    S.running = 0;
    S.st.running = 0;
    S.listen_fd = -1;
    pthread_mutex_unlock(&S.lock);

    if (!was) return;

    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);                    /* unblock accept() */
        close(fd);
    }
    if (S.started) {
        pthread_join(S.thread, NULL);
        S.started = 0;
    }
}

void httpd_status(httpd_status_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&S.lock);
    *out = S.st;
    pthread_mutex_unlock(&S.lock);
}
