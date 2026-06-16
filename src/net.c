/*
 * net.c — see net.h. Thin wrapper over the InkView NetConnect() call so the
 * keep-alive policy lives in one place and the rest of the app never pulls in
 * inkview.h just to keep WiFi up.
 */
#include "inkview.h"

#include "net.h"

void net_ensure_online(void)
{
    /* NetConnect(NULL) connects the default interface and returns once the
     * radio is up; the firmware treats it as a no-op when already online, so it
     * is safe to call before every network operation. */
    NetConnect(NULL);
}
