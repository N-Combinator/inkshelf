/* Book downloads are advanced by the UI timer, never by a blocking wait.
 * All calls (including destruction and firmware notifications) stay on the
 * InkView thread. Destroying an unfinished job cancels it and removes .part.
 */
#ifndef INKSHELF_DOWNLOAD_H
#define INKSHELF_DOWNLOAD_H

#define DL_PATH_MAX 512
#define DL_ERR_MAX 256
#define DL_IDLE_TIMEOUT_SEC 60

typedef struct download_job download_job;

/* Creates a job without waiting for WiFi or starting a blocking transfer.
 * NULL on failure, with a message in errbuf. Inputs need not outlive this call.
 */
download_job *download_start(const char *url, const char *title, const char *mime,
                             char errbuf[DL_ERR_MAX]);

/* Advance available network work. Returns 1 while pending, 0 after saving the
 * book, -1 on failure. Call only until it returns 0/-1, then destroy the job.
 * out_path is populated only on success; errbuf only on failure.
 */
int download_step(download_job *job, char out_path[DL_PATH_MAX],
                  char errbuf[DL_ERR_MAX]);
int download_percent(const download_job *job);
void download_destroy(download_job *job);

#endif
