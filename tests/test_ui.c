/*
 * test_ui.c — unit test for ui_nav_classify(), the hardware-key navigation
 * mapping. Links the real ui.c against the stub inkview.h (which defines the
 * real IV_KEY_* codes), so this asserts the classifier matches the codes the
 * device actually delivers in EVT_KEYPRESS — the bug was that it matched the
 * Linux evdev KEY_* values instead, so no hardware key ever did anything.
 */

#include <stdio.h>

#include "ui.h"

static int g_fail;

#define CHECK(cond, msg) do {                                  \
    if (cond) { printf("  ok   %s\n", msg); }                  \
    else      { printf("  FAIL %s\n", msg); g_fail++; }        \
} while (0)

/* ---- minimal InkView stubs so ui.c links (only the draw helpers it calls;
 * the test never paints anything) -------------------------------------- */
static int g_sw = 758, g_sh = 1024;     /* 6" panel unless a test changes it */
int  ScreenWidth(void)  { return g_sw; }
int  ScreenHeight(void) { return g_sh; }
ifont *OpenFont(const char *n, int s, int a) { (void)n; (void)s; (void)a; return (ifont *)1; }
void CloseFont(ifont *f) { (void)f; }
void SetFont(ifont *f, int c) { (void)f; (void)c; }
void DrawTextRect(int x,int y,int w,int h,const char *s,int f) { (void)x;(void)y;(void)w;(void)h;(void)s;(void)f; }
void DrawLine(int a,int b,int c,int d,int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void DrawRect(int a,int b,int c,int d,int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void FillArea(int a,int b,int c,int d,int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void FullUpdate(void) {}
/* ui.c's header chrome asks the nav stack how deep it is. */
int  nav_depth(void) { return 1; }

int main(void)
{
    printf("ui_nav_classify (IV_KEY_* -> nav action):\n");

    /* Up/down: scroll the selection one row. */
    CHECK(ui_nav_classify(IV_KEY_UP)   == UI_NAV_UP,        "IV_KEY_UP -> UP");
    CHECK(ui_nav_classify(IV_KEY_DOWN) == UI_NAV_DOWN,      "IV_KEY_DOWN -> DOWN");

    /* Page-turn keys (and the d-pad left/right) page a screenful. PREV2/NEXT2
     * are the secondary page keys some PocketBook models expose. */
    CHECK(ui_nav_classify(IV_KEY_PREV)  == UI_NAV_PAGE_UP,   "IV_KEY_PREV -> PAGE_UP");
    CHECK(ui_nav_classify(IV_KEY_PREV2) == UI_NAV_PAGE_UP,   "IV_KEY_PREV2 -> PAGE_UP");
    CHECK(ui_nav_classify(IV_KEY_LEFT)  == UI_NAV_PAGE_UP,   "IV_KEY_LEFT -> PAGE_UP");
    CHECK(ui_nav_classify(IV_KEY_NEXT)  == UI_NAV_PAGE_DOWN, "IV_KEY_NEXT -> PAGE_DOWN");
    CHECK(ui_nav_classify(IV_KEY_NEXT2) == UI_NAV_PAGE_DOWN, "IV_KEY_NEXT2 -> PAGE_DOWN");
    CHECK(ui_nav_classify(IV_KEY_RIGHT) == UI_NAV_PAGE_DOWN, "IV_KEY_RIGHT -> PAGE_DOWN");

    CHECK(ui_nav_classify(IV_KEY_OK)   == UI_NAV_SELECT,    "IV_KEY_OK -> SELECT");
    CHECK(ui_nav_classify(IV_KEY_BACK) == UI_NAV_BACK,      "IV_KEY_BACK -> BACK");
    CHECK(ui_nav_classify(IV_KEY_HOME) == UI_NAV_BACK,      "IV_KEY_HOME -> BACK");

    /* Regression guard: the Linux evdev value for "up" (KEY_UP == 103) is NOT
     * what the device sends and must not be mistaken for a navigation key. */
    CHECK(ui_nav_classify(103) == UI_NAV_NONE,             "evdev 103 is NOT a nav key");
    CHECK(ui_nav_classify(0)   == UI_NAV_NONE,             "unknown key -> NONE");

    /* Root-screen header buttons. The stub nav_depth() above returns 1 (root),
     * and on the 758 px stub screen the corner button spans x 606..734,
     * y 12..60 (PAD_X 24, 128x48, centred in the 72 px header). */
    printf("header buttons on the root screen:\n");
    CHECK(ui_exit_button_hit(670, 36),  "tap on the Exit button hits");
    CHECK(ui_exit_button_hit(600, 70),  "near-miss inside the forgiving margin still hits");
    CHECK(!ui_exit_button_hit(100, 36), "tap on the left of the header is not Exit");
    CHECK(!ui_exit_button_hit(670, 300),"tap below the header is not Exit");
    CHECK(!ui_back_button_hit(670, 36), "root screen has no Back button to hit");

    /* Primary action (Download) button geometry. FOOTER_H 56, ACTION_MARGIN 12,
     * PAD_X 24; height is max(64, screen_h / 14). */
    printf("action button on a 758x1024 panel:\n");
    CHECK(ui_action_button_top() == 1024 - 56 - 12 - 73 - 12, "73 px tall, sits above the footer");
    CHECK(ui_action_button_hit(379, 919),  "tap in the middle hits");
    CHECK(ui_action_button_hit(30, 919),   "full content width: tap near the left edge hits");
    CHECK(!ui_action_button_hit(379, 870), "tap above the button misses");

    printf("action button on a 1404x1872 panel (InkPad One):\n");
    g_sw = 1404; g_sh = 1872;
    CHECK(ui_action_button_top() == 1872 - 56 - 12 - 133 - 12, "scales to 133 px tall");
    CHECK(ui_action_button_hit(702, 1737), "tap in the middle hits");
    CHECK(ui_action_button_hit(1370, 1737),"spans the content width: tap near the right edge hits");
    CHECK(!ui_action_button_hit(702, 1650),"tap above the button misses");
    g_sw = 758; g_sh = 1024;

    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    printf("ui: all passed\n");
    return 0;
}
