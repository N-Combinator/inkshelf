/*
 * ui.c — implementation of the InkView drawing helpers and list widget.
 */

#include <stddef.h>

#include "app.h"
#include "ui.h"

/* Layout constants (px). PocketBook panels are high-DPI, so these are sized
 * generously to stay readable on e-ink. */
#define HEADER_H   72
#define FOOTER_H   56
#define PAD_X      24
#define ROW_PAD_Y  10

/* On-screen Back button, drawn at the right of the header on every non-root
 * screen. */
#define BACK_W     128
#define BACK_H     48

/* On-screen primary action button (e.g. Download), drawn centred just above
 * the footer so it is reachable by touch on key-less PocketBook models. */
#define ACTION_H        64
#define ACTION_MAX_W    400
#define ACTION_MARGIN   12

static ui_fonts g_fonts;

void ui_fonts_open(void)
{
    /* These three faces ship on stock PocketBook firmware. */
    g_fonts.title = OpenFont("LiberationSans-Bold", 34, 1);
    g_fonts.item  = OpenFont("LiberationSans", 30, 1);
    g_fonts.sub   = OpenFont("LiberationSans", 22, 1);
}

void ui_fonts_close(void)
{
    if (g_fonts.title) CloseFont(g_fonts.title);
    if (g_fonts.item)  CloseFont(g_fonts.item);
    if (g_fonts.sub)   CloseFont(g_fonts.sub);
    g_fonts.title = g_fonts.item = g_fonts.sub = NULL;
}

const ui_fonts *ui_get_fonts(void)
{
    return &g_fonts;
}

int ui_header_height(void) { return HEADER_H; }
int ui_footer_height(void) { return FOOTER_H; }

/* Geometry of the on-screen Back button (top-right of the header). */
static void back_button_rect(int *x, int *y, int *w, int *h)
{
    *w = BACK_W;
    *h = BACK_H;
    *x = ScreenWidth() - PAD_X - BACK_W;
    *y = (HEADER_H - BACK_H) / 2;
}

void ui_draw_header(const char *title)
{
    int w = ScreenWidth();
    int title_w = w - 2 * PAD_X;

    FillArea(0, 0, w, HEADER_H, WHITE);

    /* Non-root screens get a tappable Back button so navigation never depends
     * on a particular hardware key being present. */
    if (nav_depth() > 1) {
        int bx, by, bw, bh;
        back_button_rect(&bx, &by, &bw, &bh);
        DrawRect(bx, by, bw, bh, BLACK);
        DrawRect(bx + 1, by + 1, bw - 2, bh - 2, BLACK);
        SetFont(g_fonts.sub, BLACK);
        DrawTextRect(bx, by, bw, bh, "\xE2\x80\xB9 Back",
                     ALIGN_CENTER | VALIGN_MIDDLE);
        title_w = bx - PAD_X - 12;
        if (title_w < 0) title_w = 0;
    }

    SetFont(g_fonts.title, BLACK);
    DrawTextRect(PAD_X, 0, title_w, HEADER_H,
                 title ? title : "", ALIGN_LEFT | VALIGN_MIDDLE);
    /* Separator under the header. */
    DrawLine(0, HEADER_H - 1, w, HEADER_H - 1, BLACK);
}

int ui_back_button_hit(int x, int y)
{
    if (nav_depth() <= 1) return 0;
    int bx, by, bw, bh;
    back_button_rect(&bx, &by, &bw, &bh);
    /* Generous touch target: forgive a few px around the drawn box and accept
     * the full header height so a fat-finger tap still registers. */
    return x >= bx - 8 && x <= bx + bw + 8 && y >= 0 && y <= HEADER_H;
}

void ui_draw_footer(const char *hint)
{
    int w = ScreenWidth();
    int h = ScreenHeight();
    int y = h - FOOTER_H;

    FillArea(0, y, w, FOOTER_H, WHITE);
    DrawLine(0, y, w, y, LGRAY);
    SetFont(g_fonts.sub, DGRAY);
    DrawTextRect(PAD_X, y, w - 2 * PAD_X, FOOTER_H,
                 hint ? hint : "", ALIGN_CENTER | VALIGN_MIDDLE);
}

/* Geometry of the centred action button (just above the footer). */
static void action_button_rect(int *x, int *y, int *bw, int *bh)
{
    int w = ScreenWidth();
    int aw = w - 2 * PAD_X;
    if (aw > ACTION_MAX_W) aw = ACTION_MAX_W;
    *bw = aw;
    *bh = ACTION_H;
    *x = (w - aw) / 2;
    *y = ScreenHeight() - FOOTER_H - ACTION_MARGIN - ACTION_H;
}

int ui_action_button_top(void)
{
    int x, y, bw, bh;
    action_button_rect(&x, &y, &bw, &bh);
    return y - ACTION_MARGIN;
}

void ui_draw_action_button(const char *label)
{
    int x, y, bw, bh;
    action_button_rect(&x, &y, &bw, &bh);
    /* Double-stroked border so the tappable target reads clearly on e-ink. */
    DrawRect(x, y, bw, bh, BLACK);
    DrawRect(x + 1, y + 1, bw - 2, bh - 2, BLACK);
    SetFont(g_fonts.item, BLACK);
    DrawTextRect(x, y, bw, bh, label ? label : "", ALIGN_CENTER | VALIGN_MIDDLE);
}

int ui_action_button_hit(int x, int y)
{
    int bx, by, bw, bh;
    action_button_rect(&bx, &by, &bw, &bh);
    /* Forgive a few px around the drawn box for fat-finger taps. */
    return x >= bx - 8 && x <= bx + bw + 8 &&
           y >= by - 8 && y <= by + bh + 8;
}

void ui_flush_full(void)
{
    FullUpdate();
}

/* ---- list widget --------------------------------------------------- */

void ui_list_init(ui_list *list, const ui_list_item *items, int count)
{
    const ifont *f = g_fonts.item;
    (void)f;

    list->items = items;
    list->count = count;
    list->selected = 0;
    list->top = 0;

    /* Two text lines + padding per row. Use fixed metrics rather than
     * measuring each string so every row is the same height. */
    list->row_h = 30 /*item*/ + 24 /*sub*/ + 2 * ROW_PAD_Y;

    list->area_y = HEADER_H;
    list->area_h = ScreenHeight() - HEADER_H - FOOTER_H;
    list->per_page = list->area_h / list->row_h;
    if (list->per_page < 1) list->per_page = 1;
}

/* Keep `selected` within [top, top+per_page) by adjusting `top`. */
static void ui_list_reveal(ui_list *list)
{
    if (list->selected < list->top) {
        list->top = list->selected;
    } else if (list->selected >= list->top + list->per_page) {
        list->top = list->selected - list->per_page + 1;
    }
    if (list->top < 0) list->top = 0;
}

void ui_list_draw(const ui_list *list)
{
    int w = ScreenWidth();

    FillArea(0, list->area_y, w, list->area_h, WHITE);

    if (list->count == 0) {
        SetFont(g_fonts.sub, DGRAY);
        DrawTextRect(PAD_X, list->area_y, w - 2 * PAD_X, list->area_h,
                     "(empty)", ALIGN_CENTER | VALIGN_MIDDLE);
        return;
    }

    for (int i = 0; i < list->per_page; i++) {
        int idx = list->top + i;
        if (idx >= list->count) break;

        const ui_list_item *it = &list->items[idx];
        int y = list->area_y + i * list->row_h;
        int selected = (idx == list->selected);

        if (selected) {
            /* Inverted highlight bar for the active row. */
            FillArea(0, y, w, list->row_h, BLACK);
        }

        int fg = selected ? WHITE : BLACK;
        int sub_fg = selected ? LGRAY : DGRAY;

        SetFont(g_fonts.item, fg);
        DrawTextRect(PAD_X, y + ROW_PAD_Y, w - 2 * PAD_X, 32,
                     it->primary ? it->primary : "",
                     ALIGN_LEFT | VALIGN_MIDDLE);

        if (it->secondary) {
            SetFont(g_fonts.sub, sub_fg);
            DrawTextRect(PAD_X, y + ROW_PAD_Y + 32, w - 2 * PAD_X, 24,
                         it->secondary, ALIGN_LEFT | VALIGN_MIDDLE);
        }

        /* Row separator (skip under the highlighted row). */
        if (!selected) {
            DrawLine(PAD_X, y + list->row_h - 1, w - PAD_X,
                     y + list->row_h - 1, LGRAY);
        }
    }
}

int ui_list_move(ui_list *list, int delta)
{
    if (list->count == 0) return 0;

    int prev = list->selected;
    int next = prev + delta;

    if (next < 0) next = 0;
    if (next > list->count - 1) next = list->count - 1;

    if (next == prev) return 0;

    list->selected = next;
    ui_list_reveal(list);
    return 1;
}

int ui_list_page(ui_list *list, int dir)
{
    return ui_list_move(list, dir * list->per_page);
}

int ui_list_hit(const ui_list *list, int x, int y)
{
    (void)x;
    if (list->count == 0) return -1;
    if (y < list->area_y || y >= list->area_y + list->area_h) return -1;

    int row = (y - list->area_y) / list->row_h;
    int idx = list->top + row;
    if (idx < 0 || idx >= list->count) return -1;
    if (idx >= list->top + list->per_page) return -1;
    return idx;
}

ui_nav_action ui_nav_classify(int key)
{
    switch (key) {
    case KEY_UP:
        return UI_NAV_UP;
    case KEY_DOWN:
        return UI_NAV_DOWN;
    case KEY_LEFT:
    case KEY_PREVIOUS:
        return UI_NAV_PAGE_UP;
    case KEY_RIGHT:
    case KEY_NEXT:
        return UI_NAV_PAGE_DOWN;
    case KEY_OK:
        return UI_NAV_SELECT;
    case KEY_BACK:
    case KEY_HOME:
    case KEY_MENU:
        return UI_NAV_BACK;
    default:
        return UI_NAV_NONE;
    }
}
