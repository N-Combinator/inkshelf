#ifndef INKSHELF_LIBRARY_SCAN_H
#define INKSHELF_LIBRARY_SCAN_H

#include <time.h>

/* 1 if firmware `fw` (GetSoftwareVersion(), e.g. "U740.6.8.4473") needs the
 * scanner to list new books, 0 if BookReady is enough there (6.11 and up). */
int library_scan_needed(const char *fw);

/* Enable or disable scan requests for this firmware. Call once on the UI
 * thread at startup; until then library_scan_request() does nothing. */
void library_scan_configure(const char *fw);

/* Call only after a book is closed and renamed to its final path. No InkView
 * calls: safe from the HTTP worker. The scanner survives closing inkshelf.
 * Does nothing on firmware where library_scan_needed() is 0. */
int library_scan_request(void);

/* Handle the private supervisor mode before starting InkView. Returns -1 for
 * normal app arguments, otherwise an exit status for main(). */
int library_scan_handle_args(int argc, char **argv);

/* Supervisor core, with explicit paths so host tests use a fake scanner. */
int library_scan_run(const char *scanner, const char *lock_path,
                     const char *log_path, const struct timespec *requested,
                     int debounce_ms, int timeout_ms);

#endif
