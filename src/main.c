/*
 * inkshelf — native PocketBook .app for grabbing books over the air.
 *
 * Runs on stock PocketBook firmware via the official InkView SDK: no
 * KOReader, no jailbreak. Install by copying inkshelf.app onto the SD card.
 *
 * This file is the application entry point and top-level event dispatcher.
 * For the scaffold milestone it just brings the screen up, paints a splash
 * and exits cleanly on the Back/Home key. The OPDS browser and WiFi book
 * drop are layered on top of this loop in later milestones.
 */

#include <stddef.h>

#include "inkview.h"

#define INKSHELF_VERSION "0.1.0-dev"

static ifont *g_title_font;
static ifont *g_body_font;

static void open_fonts(void)
{
    /* DejaVuSans/LiberationSans ship on every PocketBook; size in px, AA on. */
    g_title_font = OpenFont("LiberationSans-Bold", 48, 1);
    g_body_font  = OpenFont("LiberationSans", 28, 1);
}

static void close_fonts(void)
{
    if (g_title_font) CloseFont(g_title_font);
    if (g_body_font)  CloseFont(g_body_font);
    g_title_font = g_body_font = NULL;
}

/* Full-screen splash for the scaffold milestone. */
static void draw_splash(void)
{
    int w = ScreenWidth();
    int h = ScreenHeight();

    ClearScreen();

    SetFont(g_title_font, BLACK);
    DrawTextRect(0, h / 2 - 80, w, 60, "inkshelf",
                 ALIGN_CENTER | VALIGN_MIDDLE);

    SetFont(g_body_font, DGRAY);
    DrawTextRect(0, h / 2, w, 40,
                 "OPDS catalog + WiFi book drop",
                 ALIGN_CENTER | VALIGN_MIDDLE);
    DrawTextRect(0, h / 2 + 50, w, 36, "v" INKSHELF_VERSION,
                 ALIGN_CENTER | VALIGN_MIDDLE);

    SetFont(g_body_font, DGRAY);
    DrawTextRect(0, h - 60, w, 40, "Back / Home to exit",
                 ALIGN_CENTER | VALIGN_MIDDLE);

    FullUpdate();
}

static int inkshelf_handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        open_fonts();
        break;

    case EVT_SHOW:
        draw_splash();
        break;

    case EVT_KEYPRESS:
        if (par1 == KEY_BACK || par1 == KEY_HOME) {
            CloseApp();
            return 1;
        }
        break;

    case EVT_EXIT:
        close_fonts();
        break;

    default:
        break;
    }

    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    InkViewMain(inkshelf_handler);
    return 0;
}
