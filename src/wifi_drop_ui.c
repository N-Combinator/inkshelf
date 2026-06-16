/*
 * wifi_drop_ui.c — "WiFi Book Drop" screen (#7) with PIN protection.
 *
 * Entering this screen starts the embedded HTTP upload server (httpd.c), but
 * only once a 4-digit access PIN exists: the first visit prompts for one. The
 * PIN is shown on screen (so the user can type it into the browser / pass it to
 * the deploy script) and every upload/deploy request must present it. The
 * display shows the URL to open plus a live tally of received books. Leaving
 * the screen (Back) stops the server.
 *
 * There is no explicit timer event in the subset of InkView we target, so
 * status is refreshed on every key event other than Back — the page-turn
 * buttons become a natural "refresh" gesture.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "httpd.h"
#include "net.h"
#include "ui.h"

/* The SDK's inkview.h defines the numeric-keypad layout constant; the host test
 * stub does not, so fall back to a harmless value (worst case the full keyboard
 * is shown — the PIN is still enterable). */
#ifndef KBD_NUMERIC
#define KBD_NUMERIC 1
#endif

#define WD_PIN_KEY "pin"

/* ------------------------------------------------------------------ */
/* screen state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int  started;                /* httpd_start() succeeded */
    int  need_pin;               /* no PIN set yet -> show setup prompt */
    char pin[HTTPD_PIN_MAX];
    char start_err[HTTPD_ERR_MAX];
} wifi_drop_state;

static wifi_drop_state g_wd_state;
static screen_t       *g_wd_self;            /* for repaint from kbd handler */
static char            g_pin_input[HTTPD_PIN_MAX];

/* ------------------------------------------------------------------ */
/* PIN helpers                                                         */
/* ------------------------------------------------------------------ */

static int pin_valid(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    if (n != 4) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static void wd_draw(screen_t *self);

/* Try to bring the server up (idempotent). */
static void wd_try_start(wifi_drop_state *st)
{
    if (st->started) return;
    /* Reader may have idled long enough for the firmware to drop WiFi; bring
     * the radio back before binding so the upload server gets a live link. */
    net_ensure_online();
    char err[HTTPD_ERR_MAX];
    if (httpd_start(0, err, sizeof err) == 0) {
        st->started = 1;
        st->start_err[0] = '\0';
    } else {
        st->started = 0;
        snprintf(st->start_err, sizeof st->start_err, "%s", err);
    }
}

/* Keyboard callback: validate, persist, push the PIN to the live server. Must
 * NOT re-open the keyboard on a bad entry (the host stub would recurse). */
static void pin_entered(char *s)
{
    if (!pin_valid(s)) {
        Message(ICON_WARNING, "PIN", "PIN must be exactly 4 digits.", 2000);
        if (g_wd_self) wd_draw(g_wd_self);
        return;
    }
    snprintf(g_wd_state.pin, sizeof g_wd_state.pin, "%s", s);
    config_set(WD_PIN_KEY, g_wd_state.pin);
    httpd_set_pin(g_wd_state.pin);
    g_wd_state.need_pin = 0;
    wd_try_start(&g_wd_state);
    if (g_wd_self) wd_draw(g_wd_self);
}

static void prompt_pin(const char *title)
{
    g_pin_input[0] = '\0';
    OpenKeyboard(title, g_pin_input, sizeof g_pin_input - 1, KBD_NUMERIC, pin_entered);
}

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
    int cy = hh + 24;
    int cw = sw - 48;

    ClearScreen();
    ui_draw_header("WiFi Book Drop");

    if (st->need_pin) {
        /* First run: no PIN yet — prompt to create one. */
        SetFont(f->item, BLACK);
        DrawTextRect(24, cy, cw, 160,
                     "Set a 4-digit PIN to enable WiFi Book Drop.\n\n"
                     "You will type this PIN into your browser (and the deploy "
                     "script) to upload books or update the app over WiFi.",
                     ALIGN_LEFT | VALIGN_TOP);
        ui_draw_action_button("Set PIN");
        ui_draw_footer("Tap 'Set PIN' or press OK \xC2\xB7 Back to return");
        ui_flush_full();
        return;
    }

    if (!st->started) {
        char msg[300];
        snprintf(msg, sizeof msg, "Server could not start.\n\n%s", st->start_err);
        SetFont(f->item, BLACK);
        DrawTextRect(24, cy, cw, ScreenHeight() - cy - fh - 24,
                     msg, ALIGN_CENTER | VALIGN_MIDDLE);
        ui_draw_footer("Back to return");
        ui_flush_full();
        return;
    }

    httpd_status_t hs;
    httpd_status(&hs);

    char url[128];
    snprintf(url, sizeof url, "http://%s:%d/", hs.ip, hs.port);

    SetFont(f->sub, DGRAY);
    DrawTextRect(24, cy, cw, 48,
                 "Open this address in your browser:", ALIGN_LEFT | VALIGN_MIDDLE);

    SetFont(f->title, BLACK);
    DrawTextRect(24, cy + 56, cw, 64, url, ALIGN_LEFT | VALIGN_MIDDLE);

    /* PIN line — the user needs this to upload. */
    char pin_line[48];
    snprintf(pin_line, sizeof pin_line, "PIN: %s", st->pin);
    SetFont(f->item, BLACK);
    DrawTextRect(24, cy + 120, cw, 48, pin_line, ALIGN_LEFT | VALIGN_MIDDLE);

    int dy = cy + 176;
    DrawLine(24, dy, sw - 24, dy, DGRAY);
    dy += 16;

    char count_line[80];
    snprintf(count_line, sizeof count_line, "Books received: %u", hs.uploads);
    SetFont(f->item, BLACK);
    DrawTextRect(24, dy, cw, 48, count_line, ALIGN_LEFT | VALIGN_MIDDLE);
    dy += 56;

    if (hs.last_file[0]) {
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

    if (hs.last_error[0] && dy < ui_action_button_top() - 48) {
        char emsg[HTTPD_ERR_MAX + 16];
        snprintf(emsg, sizeof emsg, "Error: %s", hs.last_error);
        SetFont(f->sub, DGRAY);
        DrawTextRect(24, dy, cw, 48, emsg, ALIGN_LEFT | VALIGN_MIDDLE);
    }

    ui_draw_action_button("Change PIN");
    ui_draw_footer("Any key to refresh \xC2\xB7 Back to stop");
    ui_flush_full();
}

/* ------------------------------------------------------------------ */
/* screen callbacks                                                    */
/* ------------------------------------------------------------------ */

static void wd_enter(screen_t *self)
{
    wifi_drop_state *st = self->data;
    g_wd_self = self;

    /* Load any saved PIN. If valid, push it to the server and start it; else
     * fall into the setup prompt and keep the server down until one is set. */
    char saved[HTTPD_PIN_MAX] = {0};
    if (config_get(WD_PIN_KEY, saved, sizeof saved) == 0 && pin_valid(saved)) {
        snprintf(st->pin, sizeof st->pin, "%s", saved);
        st->need_pin = 0;
        httpd_set_pin(st->pin);
        wd_try_start(st);
    } else {
        st->need_pin = 1;
        st->started = 0;
    }
}

static void wd_show(screen_t *self)
{
    g_wd_self = self;
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
    wifi_drop_state *st = self->data;
    ui_nav_action nav = ui_nav_classify(key);

    if (nav == UI_NAV_BACK) {
        nav_pop();
        return 1;
    }
    if (nav == UI_NAV_SELECT) {
        /* OK both creates the first PIN and changes an existing one. */
        prompt_pin(st->need_pin ? "Set a 4-digit PIN" : "Change PIN");
        return 1;
    }
    wd_draw(self);                               /* any other key: refresh */
    return 1;
}

static int wd_pointer(screen_t *self, int x, int y)
{
    wifi_drop_state *st = self->data;

    if (ui_back_button_hit(x, y)) {
        nav_pop();
        return 1;
    }
    /* Both states draw a single action button (Set/Change PIN). */
    if ((st->need_pin || st->started) && ui_action_button_hit(x, y)) {
        prompt_pin(st->need_pin ? "Set a 4-digit PIN" : "Change PIN");
        return 1;
    }
    wd_draw(self);                               /* tap elsewhere: refresh */
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
