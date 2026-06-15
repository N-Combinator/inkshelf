/*
 * app.h — application-level screen interface and navigation stack.
 *
 * inkshelf is organised as a stack of "screens". Exactly one screen is
 * active at a time; it owns the display and receives input events. Screens
 * push child screens (e.g. main menu -> OPDS browser) and pop back with the
 * Back key. The top-level InkView event handler in main.c just forwards
 * events to the current screen through this interface.
 */

#ifndef INKSHELF_APP_H
#define INKSHELF_APP_H

/* A screen implements whichever callbacks it needs; NULL is fine for the
 * rest. Each callback may repaint via the ui_* helpers. `data` is the
 * screen's own private state, passed back to every callback. */
typedef struct screen {
    const char *title;                 /* shown in the header bar */
    void *data;                        /* screen-private state */

    void (*on_enter)(struct screen *self);   /* became active (first push or re-shown after pop) */
    void (*on_show)(struct screen *self);    /* repaint request */
    void (*on_leave)(struct screen *self);   /* hidden (child pushed) or about to be popped */
    void (*on_destroy)(struct screen *self); /* removed from the stack for good — free resources */

    /* Return non-zero if the event was handled. */
    int (*on_key)(struct screen *self, int key);
    int (*on_pointer)(struct screen *self, int x, int y);
} screen_t;

/* Navigation stack. */
void nav_push(screen_t *screen);   /* make `screen` active */
void nav_pop(void);                /* drop the top screen, re-show the one below */
screen_t *nav_current(void);       /* topmost screen, or NULL if empty */
int nav_depth(void);               /* number of screens on the stack */

/* Repaint the current screen (calls its on_show). */
void nav_repaint(void);

#endif /* INKSHELF_APP_H */
