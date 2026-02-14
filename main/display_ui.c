#include "display_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "display_ui";

static lv_obj_t *s_lbl_title = NULL;
static lv_obj_t *s_lbl_line1 = NULL;
static lv_obj_t *s_lbl_line2 = NULL;
static lv_obj_t *s_lbl_line3 = NULL;
static lv_obj_t *s_lbl_line4 = NULL;

static display_status_t s_cur = {0};

static void lock_lvgl(void)   { lvgl_port_lock(0); }
static void unlock_lvgl(void) { lvgl_port_unlock(); }

void ui_set_screen_bg(uint32_t hex_rgb)
{
    lock_lvgl();

    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_hex(hex_rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    unlock_lvgl();
}

static void fmt_time_hhmmss(time_t t, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (t <= 0) {
        snprintf(out, out_sz, "--:--:--");
        return;
    }
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(out, out_sz, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static void fmt_duration_s(int64_t sec, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (sec < 0) sec = 0;
    int64_t h = sec / 3600;
    int64_t m = (sec % 3600) / 60;
    int64_t s = sec % 60;
    if (h > 0) snprintf(out, out_sz, "%lldh%lldm%llds", (long long)h, (long long)m, (long long)s);
    else if (m > 0) snprintf(out, out_sz, "%lldm%llds", (long long)m, (long long)s);
    else snprintf(out, out_sz, "%llds", (long long)s);
}

static void redraw_locked(void) {
    if (!s_lbl_title) return;

    // Title
    if (s_cur.setup_mode) {
        lv_label_set_text(s_lbl_title, "PowerPinger  (SETUP)");
    } else {
        lv_label_set_text(s_lbl_title, "PowerPinger");
    }

    // Line 1: WiFi / Ping
    char line1[128];
    snprintf(line1, sizeof(line1),
             "WiFi: %s   Ping: %s",
             s_cur.wifi_ok ? "OK" : "DOWN",
             s_cur.ping_recent_ok ? "OK" : "NO");
    lv_label_set_text(s_lbl_line1, line1);

    // Line 2: Power
    char line2[128];
    snprintf(line2, sizeof(line2),
             "Power: %s",
             s_cur.power_on ? "ON" : "OFF");
    lv_label_set_text(s_lbl_line2, line2);

    // Line 3: last change time + since
    char tbuf[32];
    char dbuf[32];
    char pbuf[32];

    time_t now = 0;
    time(&now);

    fmt_time_hhmmss((time_t)s_cur.last_change_epoch_s, tbuf, sizeof(tbuf));
    if (s_cur.last_change_epoch_s > 0) {
        fmt_duration_s((int64_t)now - s_cur.last_change_epoch_s, dbuf, sizeof(dbuf));
    } else {
        snprintf(dbuf, sizeof(dbuf), "--");
    }

    char line3[128];
    snprintf(line3, sizeof(line3),
             "Changed: %s   (%s)",
             tbuf, dbuf);
    lv_label_set_text(s_lbl_line3, line3);

    // fmt_duration_s((int64_t)now - s_cur.last_ping_try_epoch_s, pbuf, sizeof(pbuf));
    fmt_time_hhmmss((time_t)s_cur.last_ping_try_epoch_s, pbuf, sizeof(pbuf));

    char line4[128];
    snprintf(line4, sizeof(line4),
             "Time: %s",
             pbuf);
    lv_label_set_text(s_lbl_line4, line4);
}

void display_ui_init(void) {
    lock_lvgl();

    lv_obj_t *scr = lv_scr_act();

    // Немного стилей по умолчанию
    lv_obj_set_style_pad_all(scr, 12, 0);

    s_lbl_title = lv_label_create(scr);
    lv_label_set_text(s_lbl_title, "");
    lv_obj_set_style_text_font(s_lbl_title, LV_FONT_DEFAULT, 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_lbl_line1 = lv_label_create(scr);
    lv_label_set_text(s_lbl_line1, "");
    lv_obj_align(s_lbl_line1, LV_ALIGN_TOP_LEFT, 0, 24);

    s_lbl_line2 = lv_label_create(scr);
    lv_label_set_text(s_lbl_line2, "");
    lv_obj_align(s_lbl_line2, LV_ALIGN_TOP_LEFT, 0, 48);

    s_lbl_line3 = lv_label_create(scr);
    lv_label_set_text(s_lbl_line3, "");
    lv_obj_align(s_lbl_line3, LV_ALIGN_TOP_LEFT, 0, 72);

    s_lbl_line4 = lv_label_create(scr);
    lv_label_set_text(s_lbl_line4, "");
    lv_obj_align(s_lbl_line4, LV_ALIGN_TOP_LEFT, 0, 96);

    // Стартовый рендер
    memset(&s_cur, 0, sizeof(s_cur));
    s_cur.last_change_epoch_s = 0;
    // redraw_locked();

    unlock_lvgl();

    ESP_LOGI(TAG, "Display UI initialized");
}

void display_ui_set_status(const display_status_t *st) {
    if (!st) return;
    s_cur = *st;
    lock_lvgl();
    redraw_locked();
    unlock_lvgl();
}

void display_ui_set_setup_mode(bool setup) {
    s_cur.setup_mode = setup;
    lock_lvgl(); redraw_locked(); unlock_lvgl();
}

void display_ui_set_wifi(bool ok) {
    s_cur.wifi_ok = ok;
    lock_lvgl(); redraw_locked(); unlock_lvgl();
}

void display_ui_set_ping(bool recent_ok) {
    s_cur.ping_recent_ok = recent_ok;
    lock_lvgl(); redraw_locked(); unlock_lvgl();
}

void display_ui_set_power(bool on, int64_t last_change_epoch_s) {
    s_cur.power_on = on;
    if (last_change_epoch_s >= 0) s_cur.last_change_epoch_s = last_change_epoch_s;
    lock_lvgl(); redraw_locked(); unlock_lvgl();
}

void display_ui_set_last_ping_try(int64_t last_change_epoch_s) {
    s_cur.last_ping_try_epoch_s = last_change_epoch_s;
    lock_lvgl(); redraw_locked(); unlock_lvgl();
}

void display_ui_toast(const char *msg) {
    if (!msg) return;
    lock_lvgl();

    // Простейший “toast”: временно меняем title (можешь сделать отдельный label/overlay)
    if (s_lbl_title) {
        lv_label_set_text(s_lbl_title, "");
        lv_label_set_text(s_lbl_line1, "");
        lv_label_set_text(s_lbl_line2, msg);
        lv_label_set_text(s_lbl_line3, "");
        lv_label_set_text(s_lbl_line4, "");
    }

    unlock_lvgl();
}
