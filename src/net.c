/*
 * net.c — see net.h. Thin wrapper over the InkView network calls so the
 * keep-alive policy lives in one place and the rest of the app never pulls in
 * inkview.h just to keep WiFi up.
 */
#define _POSIX_C_SOURCE 200809L   /* nanosleep */

#include <time.h>

#include "inkview.h"

#include "net.h"

/* QueryNetwork() returns a bitmask of NET_* flags; any NET_CONNECTED bit means
 * the link is up and routable — the same test KOReader uses on PocketBook. */
static int net_is_online(void)
{
    return (QueryNetwork() & NET_CONNECTED) != 0;
}

void net_ensure_online(void)
{
    /* No-op when already online; otherwise asks the firmware to connect the
     * default interface. Does not block on the result — see net_wait_online. */
    NetConnect(NULL);
}

int net_wait_online(int timeout_ms, int poll_ms)
{
    if (net_is_online()) return 1;

    /* Radio is down (firmware idle power-save). Ask it to come back, then poll:
     * NetConnect() returns before the interface is actually routable, so a
     * request fired immediately would just fail again. */
    NetConnect(NULL);

    if (poll_ms <= 0) poll_ms = NET_WAIT_POLL_MS;
    for (int waited = 0; waited < timeout_ms; waited += poll_ms) {
        if (net_is_online()) return 1;
        struct timespec ts = { poll_ms / 1000,
                               (long)(poll_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return net_is_online();
}
