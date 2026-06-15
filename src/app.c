/*
 * app.c — navigation stack implementation.
 */

#include <stddef.h>

#include "app.h"

#define NAV_MAX 8

static screen_t *g_stack[NAV_MAX];
static int g_depth;

void nav_push(screen_t *screen)
{
    if (screen == NULL || g_depth >= NAV_MAX) return;

    screen_t *cur = nav_current();
    if (cur && cur->on_leave) cur->on_leave(cur);

    g_stack[g_depth++] = screen;

    if (screen->on_enter) screen->on_enter(screen);
    nav_repaint();
}

void nav_pop(void)
{
    if (g_depth == 0) return;

    screen_t *top = g_stack[--g_depth];
    if (top->on_leave) top->on_leave(top);
    if (top->on_destroy) top->on_destroy(top);

    screen_t *cur = nav_current();
    if (cur) {
        if (cur->on_enter) cur->on_enter(cur);
        nav_repaint();
    }
}

screen_t *nav_current(void)
{
    return g_depth > 0 ? g_stack[g_depth - 1] : NULL;
}

int nav_depth(void)
{
    return g_depth;
}

void nav_repaint(void)
{
    screen_t *cur = nav_current();
    if (cur && cur->on_show) cur->on_show(cur);
}
