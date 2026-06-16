/*
 * inkshelf — native PocketBook .app for grabbing books over the air.
 *
 * Runs on stock PocketBook firmware via the official InkView SDK: no
 * KOReader, no jailbreak. Install by copying inkshelf.app onto the SD card.
 *
 * This file is the InkView entry point. It owns the fonts and the event
 * loop, and forwards every event to the active screen on the navigation
 * stack (see app.h / screens.h). All UI lives in the screens.
 */

#include <stddef.h>

#include "inkview.h"

#include "app.h"
#include "net.h"
#include "screens.h"
#include "ui.h"

static int inkshelf_handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        ui_fonts_open();
        /* Bring WiFi up at launch. The firmware still powers the radio down on
         * its idle timer, so this is only the initial connect — each network
         * path (OPDS, download, WiFi-drop) re-asserts the link via
         * net_ensure_online() before use to recover from a mid-session drop. */
        net_ensure_online();
        nav_push(screen_main_menu());   /* paints the first screen */
        return 1;

    case EVT_SHOW:
        nav_repaint();
        return 1;

    case EVT_KEYPRESS: {
        screen_t *cur = nav_current();
        if (cur && cur->on_key) return cur->on_key(cur, par1);
        return 0;
    }

    case EVT_POINTERUP: {
        screen_t *cur = nav_current();
        if (cur && cur->on_pointer) return cur->on_pointer(cur, par1, par2);
        return 0;
    }

    case EVT_EXIT:
        ui_fonts_close();
        return 1;

    default:
        return 0;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    InkViewMain(inkshelf_handler);
    return 0;
}
