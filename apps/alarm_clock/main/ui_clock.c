/*
 * The clock face: big time, date, weather and the next alarm, plus the full-screen
 * overlay shown while an alarm rings, and the backlight policy.
 */
#include <stdio.h>
#include <string.h>

#include "alarms.h"
#include "bsp_display.h"
#include "clock.h"
#include "ui.h"
#include "wake.h"
#include "weather.h"
#include "wifi.h"
#include "wifi_screen.h"

// Backlight: dim at night, back to day level for a while after a touch, and never
// below the sunrise ramp while an alarm is coming up.
#define DAY_BRIGHTNESS      70
#define NIGHT_BRIGHTNESS    6
#define NIGHT_START_HOUR    22
#define NIGHT_END_HOUR      7
#define TOUCH_WAKE_MS       15000

static lv_obj_t *s_screen;
static lv_obj_t *s_time;
static lv_obj_t *s_date;
static lv_obj_t *s_weather;
static lv_obj_t *s_weather_detail;
static lv_obj_t *s_next_alarm;
static lv_obj_t *s_skip;
static lv_obj_t *s_wifi_warning;

static lv_obj_t *s_overlay;
static lv_obj_t *s_overlay_time;
static lv_obj_t *s_overlay_greeting;

lv_obj_t *ui_new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_16, 0);
    return scr;
}

lv_obj_t *ui_header_button(lv_obj_t *parent, const char *text, lv_align_t align, lv_event_cb_t cb)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_height(button, 44);
    lv_obj_align(button, align, align == LV_ALIGN_TOP_LEFT || align == LV_ALIGN_BOTTOM_LEFT ? 8 : -8,
                 align == LV_ALIGN_BOTTOM_LEFT || align == LV_ALIGN_BOTTOM_RIGHT ? -8 : 8);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void alarms_cb(lv_event_t *e)
{
    ui_alarms_open();
}

static void wifi_cb(lv_event_t *e)
{
    wifi_screen_open();
}

static void skip_cb(lv_event_t *e)
{
    wake_stop();
}

static void stop_cb(lv_event_t *e)
{
    wake_stop();
}

static void snooze_cb(lv_event_t *e)
{
    wake_snooze();
}

static void update_face(const struct tm *now, bool synced, const wake_status_t *wake)
{
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December"};
    char text[128];

    if (synced) {
        alarms_format_time(now->tm_hour, now->tm_min, text, sizeof(text));
        lv_label_set_text(s_time, text);
        lv_label_set_text_fmt(s_date, "%s, %s %d", days[now->tm_wday], months[now->tm_mon], now->tm_mday);
    } else {
        lv_label_set_text(s_time, "--:--");
        lv_label_set_text(s_date, "Waiting for the time...");
    }

    // LVGL's own printf has no float support, so format with the C library
    weather_t w;
    weather_get(&w);
    if (w.valid) {
        snprintf(text, sizeof(text), "%.0f\xC2\xB0%c  %s", w.temperature, w.unit, weather_describe(w.code));
        lv_label_set_text(s_weather, text);
        snprintf(text, sizeof(text), "High %.0f\xC2\xB0  Low %.0f\xC2\xB0%s%s", w.high, w.low,
                 w.city[0] ? "   " : "", w.city);
        lv_label_set_text(s_weather_detail, text);
    } else {
        lv_label_set_text(s_weather, "");
        lv_label_set_text(s_weather_detail, "");
    }

    wifi_status_t wifi;
    wifi_get_status(&wifi);
    lv_obj_set_hidden(s_wifi_warning, wifi.state == WIFI_STATE_CONNECTED);

    // Bottom line: what the wake-up sequence is doing, or the next alarm
    char when[16];
    struct tm t;
    bool sunrise = wake->state == WAKE_SUNRISE;
    lv_obj_set_hidden(s_skip, !sunrise);
    if (sunrise || wake->state == WAKE_SNOOZED) {
        localtime_r(&wake->target, &t);
        alarms_format_time(t.tm_hour, t.tm_min, when, sizeof(when));
        lv_label_set_text_fmt(s_next_alarm, sunrise ? LV_SYMBOL_BELL "  Sunrise - alarm at %s" : LV_SYMBOL_BELL "  Snoozed until %s", when);
    } else {
        time_t next;
        if (synced && alarms_next(time(NULL), &next)) {
            localtime_r(&next, &t);
            alarms_format_time(t.tm_hour, t.tm_min, when, sizeof(when));
            lv_label_set_text_fmt(s_next_alarm, LV_SYMBOL_BELL "  %.3s %s", days[t.tm_wday], when);
        } else {
            lv_label_set_text(s_next_alarm, LV_SYMBOL_BELL "  No alarms set");
        }
    }
}

static void update_overlay(const struct tm *now, bool synced, const wake_status_t *wake)
{
    bool ringing = wake->state == WAKE_RINGING;
    lv_obj_set_hidden(s_overlay, !ringing);
    if (ringing && synced) {
        char text[16];
        alarms_format_time(now->tm_hour, now->tm_min, text, sizeof(text));
        lv_label_set_text(s_overlay_time, text);
        lv_label_set_text(s_overlay_greeting, now->tm_hour < 12 ? "Good morning"
                                              : now->tm_hour < 18 ? "Good afternoon" : "Good evening");
    }
}

static void update_backlight(const struct tm *now, bool synced, const wake_status_t *wake)
{
    static int applied = -1;
    int level = DAY_BRIGHTNESS;
    bool night = synced && (now->tm_hour >= NIGHT_START_HOUR || now->tm_hour < NIGHT_END_HOUR);
    if (night && lv_display_get_inactive_time(NULL) > TOUCH_WAKE_MS) {
        level = NIGHT_BRIGHTNESS;
    }
    if (wake->state == WAKE_SUNRISE) {
        int ramp = 5 + (int)(95 * wake->sunrise);
        level = ramp > level ? ramp : level;
    } else if (wake->state == WAKE_RINGING) {
        level = 100;
    } else if (wake->state == WAKE_SNOOZED && level < DAY_BRIGHTNESS) {
        level = DAY_BRIGHTNESS;
    }
    if (level != applied) {
        bsp_display_set_brightness(level);
        applied = level;
    }
}

static void tick_cb(lv_timer_t *timer)
{
    struct tm now;
    bool synced = clock_now(&now);
    wake_status_t wake;
    wake_get_status(&wake);

    update_face(&now, synced, &wake);
    update_overlay(&now, synced, &wake);
    update_backlight(&now, synced, &wake);
}

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    return label;
}

static void build_overlay(void)
{
    // On the top layer so it covers whichever screen is showing
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x1A0F00), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);

    s_overlay_time = add_label(s_overlay, &font_clock_120, UI_COLOR_ACCENT);
    lv_obj_align(s_overlay_time, LV_ALIGN_TOP_MID, 0, 24);
    s_overlay_greeting = add_label(s_overlay, &lv_font_montserrat_32, UI_COLOR_TEXT);
    lv_label_set_text(s_overlay_greeting, "Alarm");
    lv_obj_align(s_overlay_greeting, LV_ALIGN_TOP_MID, 0, 138);

    lv_obj_t *snooze = lv_button_create(s_overlay);
    lv_obj_set_size(snooze, 210, 90);
    lv_obj_align(snooze, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_style_bg_color(snooze, lv_color_hex(0x3A4452), 0);
    lv_obj_add_event_cb(snooze, snooze_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = add_label(snooze, &lv_font_montserrat_24, UI_COLOR_TEXT);
    lv_label_set_text_fmt(label, "Snooze %d min", WAKE_SNOOZE_MINUTES);
    lv_obj_center(label);

    lv_obj_t *stop = lv_button_create(s_overlay);
    lv_obj_set_size(stop, 210, 90);
    lv_obj_align(stop, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(stop, lv_color_hex(0xD9822B), 0);
    lv_obj_add_event_cb(stop, stop_cb, LV_EVENT_CLICKED, NULL);
    label = add_label(stop, &lv_font_montserrat_32, 0x1A0F00);
    lv_label_set_text(label, "Stop");
    lv_obj_center(label);

    lv_obj_set_hidden(s_overlay, true);
}

void ui_init(void)
{
    s_screen = ui_new_screen();

    s_time = add_label(s_screen, &font_clock_120, UI_COLOR_TEXT);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 22);

    s_date = add_label(s_screen, &lv_font_montserrat_24, UI_COLOR_TEXT);
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 128);

    s_weather = add_label(s_screen, &lv_font_montserrat_24, UI_COLOR_ACCENT);
    lv_obj_align(s_weather, LV_ALIGN_TOP_MID, 0, 176);
    s_weather_detail = add_label(s_screen, &lv_font_montserrat_16, UI_COLOR_MUTED);
    lv_obj_align(s_weather_detail, LV_ALIGN_TOP_MID, 0, 210);

    s_wifi_warning = add_label(s_screen, &lv_font_montserrat_16, UI_COLOR_MUTED);
    lv_label_set_text(s_wifi_warning, LV_SYMBOL_WARNING " No Wi-Fi");
    lv_obj_align(s_wifi_warning, LV_ALIGN_TOP_RIGHT, -12, 10);

    s_next_alarm = add_label(s_screen, &lv_font_montserrat_16, UI_COLOR_TEXT);
    lv_obj_set_width(s_next_alarm, 250);
    lv_label_set_long_mode(s_next_alarm, LV_LABEL_LONG_DOT);
    lv_obj_align(s_next_alarm, LV_ALIGN_BOTTOM_LEFT, 14, -22);

    ui_header_button(s_screen, LV_SYMBOL_WIFI, LV_ALIGN_BOTTOM_RIGHT, wifi_cb);
    lv_obj_t *alarms = ui_header_button(s_screen, LV_SYMBOL_BELL "  Alarms", LV_ALIGN_BOTTOM_RIGHT, alarms_cb);
    lv_obj_align(alarms, LV_ALIGN_BOTTOM_RIGHT, -72, -8);
    s_skip = ui_header_button(s_screen, "Skip this alarm", LV_ALIGN_TOP_LEFT, skip_cb);
    lv_obj_set_style_bg_color(s_skip, lv_color_hex(0x7A4A12), 0);

    build_overlay();
    lv_screen_load(s_screen);

    // The first tick also turns the backlight on, after LVGL has drawn a frame
    lv_timer_create(tick_cb, 250, NULL);
}
