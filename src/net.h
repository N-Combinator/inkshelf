/*
 * net.h — WiFi session keep-alive for PocketBook.
 *
 * PocketBook firmware powers the WiFi radio down on an idle timer to save
 * battery, so a link brought up once at launch silently drops while the reader
 * sits on a screen or reads. Worse, NetConnect() returns before the interface
 * is actually routable again, so a request fired straight after it just fails
 * (observed: connect errors retried 2x, all rc=7). net_wait_online() closes
 * that gap by polling the connection state until the radio is really up.
 * Keeping the one InkView dependency here means the HTTP layer never includes
 * inkview.h.
 */
#ifndef INKSHELF_NET_H
#define INKSHELF_NET_H

/* Wait budget for the firmware to bring WiFi back after an idle drop. */
#define NET_WAIT_TIMEOUT_MS 4000   /* give up after this long */
#define NET_WAIT_POLL_MS     500   /* re-check connection state this often */

/* Kick the default interface up without waiting on the result. Cheap no-op when
 * already online; used at launch, where there is nothing to block for yet. */
void net_ensure_online(void);

/* Ensure WiFi is up AND actually connected before a network operation. Returns
 * 1 as soon as the link is connected (immediately if it already is); otherwise
 * re-asserts the radio and polls every poll_ms until connected or timeout_ms
 * elapses. Returns 0 if still offline after the timeout — callers should report
 * a connection error rather than fire a request that will only fail. */
int net_wait_online(int timeout_ms, int poll_ms);

#endif /* INKSHELF_NET_H */
