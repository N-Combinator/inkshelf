/*
 * net.h — WiFi session keep-alive for PocketBook.
 *
 * PocketBook firmware powers the WiFi radio down on an idle timer to save
 * battery, so a link brought up once at launch silently drops while the reader
 * sits on a screen or reads. net_ensure_online() re-asserts the connection; it
 * is a cheap no-op when the radio is already up, so callers invoke it before a
 * network operation or before retrying one that failed transiently. Keeping the
 * one InkView dependency here means the HTTP layer never includes inkview.h.
 */
#ifndef INKSHELF_NET_H
#define INKSHELF_NET_H

/* Bring the default network interface up if it is not already. Blocks until the
 * radio is connected (the firmware does the work); safe to call repeatedly. */
void net_ensure_online(void);

#endif /* INKSHELF_NET_H */
