/*
 * download.h — book file download to the device library.
 *
 * Writes the file to the device's Books folder via a libcurl transfer, with
 * a progress callback so the UI can show a percentage. The file is written
 * to a temp path first and renamed into place only on success, so a partial
 * download never appears in the library.
 *
 * After a successful save the function attempts a best-effort library rescan
 * via SendEvent; this is not officially documented by PocketBook but is the
 * accepted community approach. If the rescan event is unavailable the file
 * will appear after the user triggers a manual library scan or restarts the
 * reader.
 */

#ifndef INKSHELF_DOWNLOAD_H
#define INKSHELF_DOWNLOAD_H

#define DL_PATH_MAX  512
#define DL_ERR_MAX   256

/*
 * Progress callback — called periodically during the download.
 * `pct` is 0–100; return non-zero to abort.
 */
typedef int (*dl_progress_cb)(int pct, void *ud);

/*
 * Download `url` and save it as a book file.
 *
 * `title`   — used to derive the filename (sanitised, truncated).
 * `mime`    — MIME type of the download link (e.g. "application/epub+zip").
 *             Used to pick the file extension.
 * `cb`/`ud` — progress callback + user data (both may be NULL).
 * `out_path`— on success, filled with the absolute path of the saved file.
 * `errbuf`  — on failure, filled with a human-readable message.
 *
 * Returns 0 on success, -1 on failure.
 */
int download_book(const char *url,
                  const char *title,
                  const char *mime,
                  dl_progress_cb cb, void *ud,
                  char out_path[DL_PATH_MAX],
                  char errbuf[DL_ERR_MAX]);

/* Best-effort library rescan signal. Returns 0 if the rescan event was sent,
 * -1 if it could not be sent (not fatal — file is on disk regardless). */
int download_rescan_library(void);

#endif /* INKSHELF_DOWNLOAD_H */
