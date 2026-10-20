#ifndef INKSHELF_LIBRARY_SCAN_H
#define INKSHELF_LIBRARY_SCAN_H

#include <time.h>

/* Call only after a book is closed and renamed to its final path. No InkView
 * calls: safe from the HTTP worker. The scanner survives closing inkshelf. */
int library_scan_request(void);

/* Handle the private supervisor mode before starting InkView. Returns -1 for
 * normal app arguments, otherwise an exit status for main(). */
int library_scan_handle_args(int argc, char **argv);

/* Supervisor core, with explicit paths so host tests use a fake scanner. */
int library_scan_run(const char *scanner, const char *lock_path,
                     const char *log_path, const struct timespec *requested,
                     int debounce_ms, int timeout_ms);

#endif
