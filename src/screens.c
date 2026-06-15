/*
 * screens.c — main menu plus a "coming soon" placeholder.
 *
 * The placeholder stands in for the OPDS browser and WiFi-drop screens until
 * those milestones land; it already exercises the full nav-stack round trip
 * (push on select, pop on Back), so wiring the real screens in later is a
 * drop-in replacement of the constructor.
 */

#include <stddef.h>

#include "app.h"
#include "ui.h"

/* ---- placeholder "coming soon" screen ------------------------------ */

typedef struct {
    const char *message;
} placeholder_state;

static void placeholder_show(screen_t *self)
{
    placeholder_state *st = self->data;
    int w = ScreenWidth();
    int content_y = ui_header_height();
    int content_h = ScreenHeight() - ui_header_height() - ui_footer_height();
    const ui_fonts *f = ui_get_fonts();

    ClearScreen();
    ui_draw_header(self->title);

    SetFont(f->item, DGRAY);
    DrawTextRect(24, content_y, w - 48, content_h,
                 st->message, ALIGN_CENTER | VALIGN_MIDDLE);

    ui_draw_footer("Back to return");
    ui_flush_full();
}

static int placeholder_key(screen_t *self, int key)
{
    (void)self;
    if (ui_nav_classify(key) == UI_NAV_BACK) {
        nav_pop();
        return 1;
    }
    return 0;
}

static placeholder_state g_opds_ph = {
    "OPDS catalog browser\n\nComing in the next milestone."
};
static placeholder_state g_wifi_ph = {
    "WiFi book drop\n\nComing in a later milestone."
};

static screen_t g_opds_screen = {
    .title = "OPDS Catalog",
    .data = &g_opds_ph,
    .on_show = placeholder_show,
    .on_key = placeholder_key,
};
static screen_t g_wifi_screen = {
    .title = "WiFi Book Drop",
    .data = &g_wifi_ph,
    .on_show = placeholder_show,
    .on_key = placeholder_key,
};

/* ---- main menu ----------------------------------------------------- */

static const ui_list_item MENU_ITEMS[] = {
    { "OPDS Catalog",   "Browse and download from an OPDS feed" },
    { "WiFi Book Drop", "Upload books from your PC or phone" },
};
#define MENU_COUNT ((int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0])))

typedef struct {
    ui_list list;
} menu_state;

static menu_state g_menu_state;

static void menu_activate(int idx)
{
    switch (idx) {
    case 0:
        nav_push(&g_opds_screen);
        break;
    case 1:
        nav_push(&g_wifi_screen);
        break;
    default:
        break;
    }
}

static void menu_enter(screen_t *self)
{
    menu_state *st = self->data;
    /* Re-init geometry each time we (re)enter in case the screen rotated. */
    int prev = st->list.items ? st->list.selected : 0;
    ui_list_init(&st->list, MENU_ITEMS, MENU_COUNT);
    if (prev < MENU_COUNT) st->list.selected = prev;
}

static void menu_show(screen_t *self)
{
    menu_state *st = self->data;

    ClearScreen();
    ui_draw_header(self->title);
    ui_list_draw(&st->list);
    ui_draw_footer("Up/Down to move · OK or tap to open");
    ui_flush_full();
}

static int menu_key(screen_t *self, int key)
{
    menu_state *st = self->data;

    switch (ui_nav_classify(key)) {
    case UI_NAV_UP:
        if (ui_list_move(&st->list, -1)) menu_show(self);
        return 1;
    case UI_NAV_DOWN:
        if (ui_list_move(&st->list, +1)) menu_show(self);
        return 1;
    case UI_NAV_PAGE_UP:
        if (ui_list_page(&st->list, -1)) menu_show(self);
        return 1;
    case UI_NAV_PAGE_DOWN:
        if (ui_list_page(&st->list, +1)) menu_show(self);
        return 1;
    case UI_NAV_SELECT:
        menu_activate(st->list.selected);
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
    int idx = ui_list_hit(&st->list, x, y);
    if (idx < 0) return 0;

    if (idx != st->list.selected) {
        st->list.selected = idx;
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
