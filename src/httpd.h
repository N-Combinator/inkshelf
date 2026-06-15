/*
 * httpd.h — embedded HTTP upload server for "WiFi Book Drop" (#6).
 *
 * Runs a tiny HTTP/1.1 server in a background thread so any phone or laptop on
 * the same WiFi can open http://<reader-ip>:8080/ in a browser and drop a book
 * straight onto the device — no cable, no desktop app. Uploaded files land in
 * the library directory (see library.h) using the same atomic temp+rename
 * write the OPDS downloader uses, so a dropped connection never leaves a
 * corrupt book behind.
 *
 * Deliberately dependency-free: plain POSIX sockets + one pthread, no extra
 * libraries beyond what the PocketBook toolchain already ships. The UI screen
 * (#7) just calls httpd_start()/httpd_status()/httpd_stop() and renders the
 * status struct; all of the protocol lives here.
 *
 * The multipart parser is factored to run over a read callback rather than a
 * raw socket, so it is exercised by host unit tests (tests/test_httpd.c) with
 * no network involved.
 */

#ifndef INKSHELF_HTTPD_H
#define INKSHELF_HTTPD_H

#include <stddef.h>

#define HTTPD_DEFAULT_PORT 8080
#define HTTPD_NAME_MAX     256
#define HTTPD_ERR_MAX      160
#define HTTPD_IP_MAX       64

/* A snapshot of the server's live state, copied out under lock by
 * httpd_status(). The WiFi-drop UI screen polls this to show the URL to type
 * and a running tally of what has arrived. */
typedef struct {
    int                running;                 /* thread up and listening */
    int                port;                    /* bound TCP port */
    char               ip[HTTPD_IP_MAX];        /* best-guess LAN IPv4 */
    unsigned           uploads;                 /* completed uploads so far */
    char               last_file[HTTPD_NAME_MAX]; /* name of newest upload */
    unsigned long long last_bytes;              /* size of newest upload */
    char               last_error[HTTPD_ERR_MAX]; /* newest error, "" if none */
} httpd_status_t;

/*
 * Start the upload server. `port` of 0 selects HTTPD_DEFAULT_PORT. Spawns the
 * accept thread and returns immediately. Returns 0 on success, -1 on failure
 * (err filled with a human-readable reason, e.g. address in use). Calling
 * start while already running is a no-op success.
 */
int httpd_start(int port, char *err, size_t errsz);

/* Stop the server: unblock and join the accept thread, close the socket.
 * Safe to call when not running. */
void httpd_stop(void);

/* Copy the current status out (thread-safe). */
void httpd_status(httpd_status_t *out);

/*
 * Best-effort LAN IPv4 address of the device (e.g. "192.168.1.5") into `out`.
 * Skips loopback; prefers a private-range address. Returns 0 on success, or
 * -1 if none was found (out set to "0.0.0.0").
 */
int httpd_local_ipv4(char *out, size_t outsz);

/* ---- exposed for host unit tests (tests/test_httpd.c) ------------------ */

/*
 * Read callback abstraction. Fill up to `n` bytes into `buf`; return the
 * number read, 0 at end of input, or <0 on error. Lets the multipart reader
 * run over a socket on-device and over a memory buffer in tests.
 */
typedef long (*httpd_reader_fn)(void *ud, char *buf, size_t n);

/*
 * Receive a multipart/form-data body from `reader` and save the first file
 * part into `dest_dir`. `content_type` is the full Content-Type header value
 * (the boundary is extracted from it). Streams the part body straight to
 * "<dest_dir>/<name>.part" and renames it into place only on success; a name
 * collision is resolved by appending " (1)", " (2)", ... .
 *
 * On success returns 0, fills `out_name` with the saved file's base name and
 * `out_bytes` with its size. On failure returns -1 with a message in `err`.
 */
int httpd_recv_multipart(httpd_reader_fn reader, void *ud,
                         const char *content_type, const char *dest_dir,
                         char *out_name, size_t out_name_sz,
                         unsigned long long *out_bytes,
                         char *err, size_t errsz);

/*
 * Sanitise a client-supplied upload filename into a safe base name (no path
 * component, FAT/ext4-safe characters, length-limited, extension preserved).
 * Always yields a non-empty name (falls back to "book.bin"). Returns 0 on
 * success, -1 if `out` was too small.
 */
int httpd_safe_name(const char *raw, char *out, size_t outsz);

#endif /* INKSHELF_HTTPD_H */
