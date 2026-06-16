/*
 * ui.h — InkView drawing helpers and a reusable scrollable list widget.
 *
 * Keeps all the e-ink layout/painting concerns in one place so screens can
 * be written in terms of "draw a header", "draw this list", "draw a footer
 * hint" rather than juggling raw InkView coordinates. The list widget backs
 * the main menu and, later, the OPDS results browser.
 */

#ifndef INKSHELF_UI_H
#define INKSHELF_UI_H

#include "inkview.h"

/* Shared fonts, opened once at startup. */
typedef struct {
    ifont *title;   /* header bar */
    ifont *item;    /* list item primary line */
    ifont *sub;     /* list item secondary line / hints */
} ui_fonts;

void ui_fonts_open(void);
void ui_fonts_close(void);
const ui_fonts *ui_get_fonts(void);

/* Chrome. Header draws the title bar; footer draws a centred hint line.
 * The header automatically shows an on-screen "Back" button whenever the nav
 * stack is deeper than the root screen, so Back is reachable by touch on any
 * PocketBook regardless of its hardware key layout (some models are
 * touch-only). Screens route taps through ui_back_button_hit(). */
int  ui_header_height(void);
int  ui_footer_height(void);
void ui_draw_header(const char *title);
void ui_draw_footer(const char *hint);

/* Returns non-zero if (x, y) falls on the on-screen Back button. Always 0 on
 * the root screen (no button is drawn there). Screens should call this at the
 * top of their on_pointer handler and nav_pop() when it returns true. */
int  ui_back_button_hit(int x, int y);

/* Centred primary action button (e.g. Download), drawn just above the footer
 * so an action is reachable by touch on key-less PocketBook models. Draw it
 * with ui_draw_action_button(), route taps through ui_action_button_hit(),
 * and keep body content above ui_action_button_top() so they never overlap. */
void ui_draw_action_button(const char *label);
int  ui_action_button_hit(int x, int y);
int  ui_action_button_top(void);

/* Push the whole framebuffer to the panel (full e-ink refresh). */
void ui_flush_full(void);

/* ---- Scrollable list widget ---------------------------------------- */

typedef struct {
    const char *primary;     /* required: main line */
    const char *secondary;   /* optional: smaller second line, may be NULL */
} ui_list_item;

typedef struct {
    const ui_list_item *items;
    int count;
    int selected;       /* index of the highlighted row */
    int top;            /* index of the first visible row */
    int row_h;          /* pixel height of one row (incl. secondary line) */
    int area_y;         /* y of the list area (below header) */
    int area_h;         /* height of the list area (above footer) */
    int per_page;       /* rows that fit in area_h */
} ui_list;

/* Bind `items`/`count` to the list and compute geometry for the current
 * screen size. Selection starts at 0. */
void ui_list_init(ui_list *list, const ui_list_item *items, int count);

/* Draw the list rows within the content area (does not touch header/footer). */
void ui_list_draw(const ui_list *list);

/* Move the selection by `delta` rows (clamped), scrolling as needed.
 * Returns non-zero if the selection actually changed. */
int ui_list_move(ui_list *list, int delta);

/* Page the selection by one screenful in `dir` (+1 down, -1 up).
 * Returns non-zero if the selection changed. */
int ui_list_page(ui_list *list, int dir);

/* Map a touch point to a row index, or -1 if outside the list / empty row. */
int ui_list_hit(const ui_list *list, int x, int y);

/* ---- input classification ------------------------------------------ */

/* PocketBook models differ wildly in their hardware keys (some are
 * touch-only, some have page-turn bars, some have a d-pad). Map the raw
 * InkView key code to a single semantic navigation action so screens don't
 * each re-encode the key matrix. */
typedef enum {
    UI_NAV_NONE = 0,
    UI_NAV_UP,
    UI_NAV_DOWN,
    UI_NAV_PAGE_UP,
    UI_NAV_PAGE_DOWN,
    UI_NAV_SELECT,
    UI_NAV_BACK
} ui_nav_action;

ui_nav_action ui_nav_classify(int key);

#endif /* INKSHELF_UI_H */
