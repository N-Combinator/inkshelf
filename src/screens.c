/*
 * screens.c — the app's home screen.
 *
 * A custom-drawn landing page rather than a plain list: an "about" block at the
 * top (name, tagline, author, repo, build version) and two large entry buttons
 * filling the lower part of the screen. The buttons are navigated with the
 * hardware up/down keys (selection drawn inverted) or by tapping directly.
 *
 * INKSHELF_VERSION is injected by CMake (git describe); the host test gate
 * compiles without it, so a "dev" fallback keeps the build self-contained.
 */

#include <stdio.h>

#include "app.h"
#include "screens.h"
#include "ui.h"

#ifndef INKSHELF_VERSION
#define INKSHELF_VERSION "dev"
#endif

#define MARGIN     32       /* left/right page margin (px) */
#define BTN_GAP    24       /* gap between the two buttons (px) */
#define BTN_PAD_X  28       /* text inset inside a button (px) */

/* ---- entry buttons ------------------------------------------------- */

static const struct {
    const char *title;
    const char *desc;
} BUTTONS[] = {
    { "OPDS Catalog",   "Browse & download books" },
    { "WiFi Book Drop", "Receive books over WiFi" },
};
#define BTN_COUNT ((int)(sizeof(BUTTONS) / sizeof(BUTTONS[0])))

typedef struct {
    int selected;           /* highlighted button (0..BTN_COUNT-1) */
} menu_state;

static menu_state g_menu_state;

/* A bold display face for the "inkshelf" wordmark, larger than the shared
 * title font. Opened once and kept for the app's lifetime. */
static ifont *hero_font(void)
{
    static ifont *f;
    if (!f) f = OpenFont("LiberationSans-Bold", 56, 1);
    return f;
}

/* The buttons occupy the lower ~60% of the screen; split that band into
 * BTN_COUNT equal rows with a gap between them. */
static void button_rect(int i, int *x, int *y, int *w, int *h)
{
    int sw = ScreenWidth();
    int sh = ScreenHeight();
    int top = (int)(sh * 0.42);                 /* about block gets the top 42% */
    int bottom = sh - ui_footer_height() - 16;
    int avail = bottom - top;
    int bh = (avail - BTN_GAP * (BTN_COUNT - 1)) / BTN_COUNT;
    if (bh < 1) bh = 1;

    *x = MARGIN;
    *w = sw - 2 * MARGIN;
    *h = bh;
    *y = top + i * (bh + BTN_GAP);
}

static int button_at(int px, int py)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        int x, y, w, h;
        button_rect(i, &x, &y, &w, &h);
        if (px >= x && px <= x + w && py >= y && py <= y + h) return i;
    }
    return -1;
}

static void menu_activate(int idx)
{
    switch (idx) {
    case 0:
        nav_push(screen_opds_catalog());
        break;
    case 1:
        nav_push(screen_wifi_drop());
        break;
    default:
        break;
    }
}

/* ---- drawing ------------------------------------------------------- */

static void draw_about(void)
{
    int sw = ScreenWidth();
    int x = MARGIN;
    int cw = sw - 2 * MARGIN;
    int y = 48;
    const ui_fonts *f = ui_get_fonts();

    SetFont(hero_font(), BLACK);
    DrawTextRect(x, y, cw, 64, "inkshelf", ALIGN_LEFT | VALIGN_TOP);
    y += 74;

    SetFont(f->item, BLACK);
    DrawTextRect(x, y, cw, 38, "Open-source book manager for PocketBook",
                 ALIGN_LEFT | VALIGN_TOP);
    y += 44;

    SetFont(f->sub, DGRAY);
    DrawTextRect(x, y, cw, 28, "cprite / N-Combinator", ALIGN_LEFT | VALIGN_TOP);
    y += 30;
    DrawTextRect(x, y, cw, 28, "github.com/N-Combinator/inkshelf",
                 ALIGN_LEFT | VALIGN_TOP);
    y += 30;

    char ver[96];
    snprintf(ver, sizeof ver, "Version %s", INKSHELF_VERSION);
    DrawTextRect(x, y, cw, 28, ver, ALIGN_LEFT | VALIGN_TOP);
    y += 36;

    DrawLine(MARGIN, y, sw - MARGIN, y, LGRAY);
}

static void draw_button(int i, int selected)
{
    int x, y, w, h;
    button_rect(i, &x, &y, &w, &h);
    const ui_fonts *f = ui_get_fonts();

    if (selected) {
        FillArea(x, y, w, h, BLACK);
    } else {
        /* Double-stroked border so the tappable target reads on e-ink. */
        DrawRect(x, y, w, h, BLACK);
        DrawRect(x + 1, y + 1, w - 2, h - 2, BLACK);
    }

    int fg = selected ? WHITE : BLACK;
    int sub_fg = selected ? LGRAY : DGRAY;
    int tx = x + BTN_PAD_X;
    int tw = w - 2 * BTN_PAD_X;
    int block_h = 44 + 32;                      /* title line + desc line */
    int ty = y + (h - block_h) / 2;

    SetFont(f->title, fg);
    DrawTextRect(tx, ty, tw, 44, BUTTONS[i].title, ALIGN_LEFT | VALIGN_MIDDLE);
    SetFont(f->sub, sub_fg);
    DrawTextRect(tx, ty + 44, tw, 32, BUTTONS[i].desc, ALIGN_LEFT | VALIGN_MIDDLE);
}

static void menu_show(screen_t *self)
{
    menu_state *st = self->data;

    ClearScreen();
    draw_about();
    for (int i = 0; i < BTN_COUNT; i++)
        draw_button(i, i == st->selected);
    ui_draw_footer("Up/Down to move \xC2\xB7 OK or tap to open");
    ui_flush_full();
}

/* ---- input --------------------------------------------------------- */

static int menu_move(menu_state *st, int delta)
{
    int n = st->selected + delta;
    if (n < 0) n = 0;
    if (n > BTN_COUNT - 1) n = BTN_COUNT - 1;
    if (n == st->selected) return 0;
    st->selected = n;
    return 1;
}

static void menu_enter(screen_t *self)
{
    menu_state *st = self->data;
    if (st->selected < 0 || st->selected >= BTN_COUNT) st->selected = 0;
}

static int menu_key(screen_t *self, int key)
{
    menu_state *st = self->data;

    switch (ui_nav_classify(key)) {
    case UI_NAV_UP:
    case UI_NAV_PAGE_UP:
        if (menu_move(st, -1)) menu_show(self);
        return 1;
    case UI_NAV_DOWN:
    case UI_NAV_PAGE_DOWN:
        if (menu_move(st, +1)) menu_show(self);
        return 1;
    case UI_NAV_SELECT:
        menu_activate(st->selected);
        return 1;
    case UI_NAV_BACK:
        /* Root screen: Back exits the app. */
        CloseApp();
        return 1;
    default:
        return 0;
    }
}

static int menu_pointer(screen_t *self, int x, int y)
{
    menu_state *st = self->data;
    int idx = button_at(x, y);
    if (idx < 0) return 0;

    if (idx != st->selected) {
        st->selected = idx;
        menu_show(self);
    }
    menu_activate(idx);
    return 1;
}

static screen_t g_menu_screen = {
    .title = "inkshelf",
    .data = &g_menu_state,
    .on_enter = menu_enter,
    .on_show = menu_show,
    .on_key = menu_key,
    .on_pointer = menu_pointer,
};

screen_t *screen_main_menu(void)
{
    return &g_menu_screen;
}
