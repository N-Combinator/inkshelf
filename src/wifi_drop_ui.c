/*
 * wifi_drop_ui.c — "WiFi Book Drop" screen (#7).
 *
 * Entering this screen starts the embedded HTTP upload server (httpd.c).
 * The display shows the URL the user must type into any browser on the same
 * WiFi network, plus a live running tally of received books. Leaving the
 * screen (Back) stops the server.
 *
 * There is no explicit timer event in the subset of InkView we target, so
 * status is refreshed on every key event other than Back — the page-turn
 * buttons become a natural "refresh" gesture.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "httpd.h"
#include "ui.h"

/* ------------------------------------------------------------------ */
/* screen state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int  started;                /* httpd_start() succeeded */
    char start_err[HTTPD_ERR_MAX];
} wifi_drop_state;

static wifi_drop_state g_wd_state;

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void wd_draw(screen_t *self)
{
    wifi_drop_state *st = self->data;
    const ui_fonts *f = ui_get_fonts();

    int sw = ScreenWidth();
    int hh = ui_header_height();
    int fh = ui_footer_height();
    int cy = hh + 24;                       /* top of content area */
    int cw = sw - 48;

    ClearScreen();
    ui_draw_header("WiFi Book Drop");

    if (!st->started) {
        /* Server failed to start — show the error prominently. */
        char msg[300];
        snprintf(msg, sizeof msg,
                 "Server could not start.\n\n%s", st->start_err);
        SetFont(f->item, BLACK);
        DrawTextRect(24, cy, cw,
                     ScreenHeight() - cy - fh - 24,
                     msg, ALIGN_CENTER | VALIGN_MIDDLE);
        ui_draw_footer("Back to return");
        ui_flush_full();
        return;
    }

    httpd_status_t hs;
    httpd_status(&hs);

    /* Build the URL string. */
    char url[128];
    snprintf(url, sizeof url, "http://%s:%d/", hs.ip, hs.port);

    /* Instruction line */
    SetFont(f->sub, DGRAY);
    DrawTextRect(24, cy, cw, 48,
                 "Open this address in your browser:", ALIGN_LEFT | VALIGN_MIDDLE);

    /* URL — bigger, prominent */
    SetFont(f->title, BLACK);
    DrawTextRect(24, cy + 56, cw, 64, url, ALIGN_LEFT | VALIGN_MIDDLE);

    /* Divider */
    int dy = cy + 138;
    DrawLine(24, dy, sw - 24, dy, DGRAY);
    dy += 16;

    /* Upload tally */
    char count_line[80];
    snprintf(count_line, sizeof count_line,
             "Books received: %u", hs.uploads);
    SetFont(f->item, BLACK);
    DrawTextRect(24, dy, cw, 48, count_line, ALIGN_LEFT | VALIGN_MIDDLE);
    dy += 56;

    if (hs.last_file[0]) {
        /* Most recent upload */
        char detail[HTTPD_NAME_MAX + 40];
        char sz_buf[24];
        unsigned long long b = hs.last_bytes;
        if (b >= 1024 * 1024)
            snprintf(sz_buf, sizeof sz_buf, "%.1f MB", (double)b / (1024.0 * 1024.0));
        else if (b >= 1024)
            snprintf(sz_buf, sizeof sz_buf, "%.1f KB", (double)b / 1024.0);
        else
            snprintf(sz_buf, sizeof sz_buf, "%llu B", b);

        snprintf(detail, sizeof detail, "Last: %s  (%s)", hs.last_file, sz_buf);
        SetFont(f->sub, DGRAY);
        DrawTextRect(24, dy, cw, 48, detail, ALIGN_LEFT | VALIGN_MIDDLE);
        dy += 48;
    }

    if (hs.last_error[0]) {
        /* Non-fatal upload error */
        char emsg[HTTPD_ERR_MAX + 16];
        snprintf(emsg, sizeof emsg, "Error: %s", hs.last_error);
        SetFont(f->sub, DGRAY);
        DrawTextRect(24, dy, cw, 48, emsg, ALIGN_LEFT | VALIGN_MIDDLE);
    }

    ui_draw_footer("Press any key to refresh · Back to stop");
    ui_flush_full();
}

/* ------------------------------------------------------------------ */
/* screen callbacks                                                    */
/* ------------------------------------------------------------------ */

static void wd_enter(screen_t *self)
{
    wifi_drop_state *st = self->data;

    if (!st->started) {
        char err[HTTPD_ERR_MAX];
        if (httpd_start(0, err, sizeof err) == 0) {
            st->started = 1;
            st->start_err[0] = '\0';
        } else {
            st->started = 0;
            snprintf(st->start_err, sizeof st->start_err, "%s", err);
        }
    }
}

static void wd_show(screen_t *self)
{
    wd_draw(self);
}

static void wd_destroy(screen_t *self)
{
    wifi_drop_state *st = self->data;
    if (st->started) {
        httpd_stop();
        st->started = 0;
    }
}

static int wd_key(screen_t *self, int key)
{
    if (ui_nav_classify(key) == UI_NAV_BACK) {
        nav_pop();
        return 1;
    }
    /* Any other key: refresh status. */
    wd_draw(self);
    return 1;
}

static int wd_pointer(screen_t *self, int x, int y)
{
    (void)x; (void)y;
    /* Tap anywhere to refresh. */
    wd_draw(self);
    return 1;
}

/* ------------------------------------------------------------------ */
/* constructor                                                         */
/* ------------------------------------------------------------------ */

static screen_t g_wd_screen = {
    .title      = "WiFi Book Drop",
    .data       = &g_wd_state,
    .on_enter   = wd_enter,
    .on_show    = wd_show,
    .on_destroy = wd_destroy,
    .on_key     = wd_key,
    .on_pointer = wd_pointer,
};

screen_t *screen_wifi_drop(void)
{
    return &g_wd_screen;
}
