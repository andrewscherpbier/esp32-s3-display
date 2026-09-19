#include "wifi_screen.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "wifi.h"

#define MAX_NETWORKS        20
#define WPA_MIN_PASSWORD    8

#define COLOR_BG            0x101820
#define COLOR_MUTED         0x9AA5B1
#define COLOR_ERROR         0xFF6B6B
#define COLOR_LIST          0x18222D
#define COLOR_ROW           0x223040

static lv_obj_t *s_return_screen;
static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_error;
static lv_obj_t *s_forget;
static lv_obj_t *s_list;
static lv_timer_t *s_timer;
static int s_seen_scan;

static lv_obj_t *s_pw_screen;
static lv_obj_t *s_pw_title;
static lv_obj_t *s_pw_text;
static lv_obj_t *s_pw_hint;

// Backs the list: each network button's user data points into this array, which is only
// rewritten together with the list.
static wifi_ap_record_t s_networks[MAX_NETWORKS];
static char s_pw_ssid[33];

static bool is_supported(wifi_auth_mode_t mode)
{
    // WEP needs a different auth threshold than the WPA family wifi.c uses
    switch (mode) {
    case WIFI_AUTH_WEP:
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA_ENTERPRISE:
    case WIFI_AUTH_WAPI_PSK:
    case WIFI_AUTH_DPP:
        return false;
    default:
        return true;
    }
}

static bool needs_password(wifi_auth_mode_t mode)
{
    return mode != WIFI_AUTH_OPEN && mode != WIFI_AUTH_OWE;
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    return scr;
}

static lv_obj_t *add_header_button(lv_obj_t *parent, const char *text, lv_align_t align,
                                   lv_event_cb_t cb)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_height(button, 40);
    lv_obj_align(button, align, align == LV_ALIGN_TOP_LEFT ? 8 : -8, 6);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

/* ---------- password entry ---------- */

static void pw_close(void)
{
    lv_screen_load(s_screen);
}

static void pw_submit(void)
{
    const char *password = lv_textarea_get_text(s_pw_text);
    if (strlen(password) < WPA_MIN_PASSWORD) {
        lv_label_set_text_fmt(s_pw_hint, "Wi-Fi passwords are at least %d characters", WPA_MIN_PASSWORD);
        return;
    }
    wifi_connect(s_pw_ssid, password);
    lv_textarea_set_text(s_pw_text, "");
    pw_close();
}

static void pw_keyboard_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        pw_submit();
    } else {
        pw_close();
    }
}

static void pw_cancel_cb(lv_event_t *e)
{
    lv_textarea_set_text(s_pw_text, "");
    pw_close();
}

static void pw_show_cb(lv_event_t *e)
{
    lv_obj_t *button = lv_event_get_target(e);
    bool show = lv_obj_has_state(button, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_pw_text, !show);
}

static void pw_build(void)
{
    s_pw_screen = new_screen();

    s_pw_title = lv_label_create(s_pw_screen);
    lv_obj_set_width(s_pw_title, 330);
    lv_label_set_long_mode(s_pw_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_pw_title, LV_ALIGN_TOP_LEFT, 12, 16);
    add_header_button(s_pw_screen, "Cancel", LV_ALIGN_TOP_RIGHT, pw_cancel_cb);

    s_pw_text = lv_textarea_create(s_pw_screen);
    lv_textarea_set_one_line(s_pw_text, true);
    lv_textarea_set_password_mode(s_pw_text, true);
    lv_textarea_set_max_length(s_pw_text, 64);
    lv_textarea_set_placeholder_text(s_pw_text, "Password");
    lv_obj_set_width(s_pw_text, 360);
    lv_obj_align(s_pw_text, LV_ALIGN_TOP_LEFT, 12, 56);

    lv_obj_t *show = lv_button_create(s_pw_screen);
    lv_obj_set_checkable(show, true);
    lv_obj_set_size(show, 84, 40);
    lv_obj_align_to(show, s_pw_text, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_obj_add_event_cb(show, pw_show_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *show_label = lv_label_create(show);
    lv_label_set_text(show_label, "Show");
    lv_obj_center(show_label);

    s_pw_hint = lv_label_create(s_pw_screen);
    lv_obj_set_style_text_color(s_pw_hint, lv_color_hex(COLOR_ERROR), 0);
    lv_obj_align_to(s_pw_hint, s_pw_text, LV_ALIGN_OUT_BOTTOM_LEFT, 4, 6);

    lv_obj_t *keyboard = lv_keyboard_create(s_pw_screen);
    lv_obj_set_height(keyboard, 180);
    lv_keyboard_set_textarea(keyboard, s_pw_text);
    lv_obj_add_event_cb(keyboard, pw_keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, pw_keyboard_cb, LV_EVENT_CANCEL, NULL);
}

static void pw_open(const char *ssid)
{
    if (!s_pw_screen) {
        pw_build();
    }
    strlcpy(s_pw_ssid, ssid, sizeof(s_pw_ssid));
    lv_label_set_text_fmt(s_pw_title, "Password for %s", ssid);
    lv_label_set_text(s_pw_hint, "");
    lv_textarea_set_text(s_pw_text, "");
    lv_screen_load(s_pw_screen);
}

/* ---------- network list ---------- */

static void network_clicked_cb(lv_event_t *e)
{
    const wifi_ap_record_t *ap = lv_event_get_user_data(e);
    const char *ssid = (const char *)ap->ssid;
    if (needs_password(ap->authmode)) {
        pw_open(ssid);
    } else {
        wifi_connect(ssid, "");
    }
}

static void list_message(const char *text)
{
    lv_obj_clean(s_list);
    lv_obj_t *label = lv_label_create(s_list);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_pad_all(label, 10, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_MUTED), 0);
    lv_label_set_text(label, text);
}

// One network per row: icon, name (truncated to fit) and security/signal on the right.
static lv_obj_t *list_add_row(const char *icon, const char *name, const char *info)
{
    lv_obj_t *row = lv_button_create(s_list);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);

    lv_obj_t *icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);

    lv_obj_t *name_label = lv_label_create(row);
    lv_label_set_text(name_label, name);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name_label, 1);

    lv_obj_t *info_label = lv_label_create(row);
    lv_obj_set_style_text_color(info_label, lv_color_hex(COLOR_MUTED), 0);
    lv_label_set_text(info_label, info);
    return row;
}

static void list_rebuild(void)
{
    wifi_status_t status;
    wifi_get_status(&status);

    int count = wifi_scan_results(s_networks, MAX_NETWORKS);
    if (count == 0) {
        list_message("No networks found");
        return;
    }
    lv_obj_clean(s_list);
    for (int i = 0; i < count; i++) {
        const wifi_ap_record_t *ap = &s_networks[i];
        bool supported = is_supported(ap->authmode);
        bool current = status.state == WIFI_STATE_CONNECTED &&
                       strcmp(status.ssid, (const char *)ap->ssid) == 0;

        char info[32];
        snprintf(info, sizeof(info), "%s  %d dBm",
                 !supported ? "Not supported" : needs_password(ap->authmode) ? "Secured" : "Open",
                 ap->rssi);
        lv_obj_t *row = list_add_row(current ? LV_SYMBOL_OK : LV_SYMBOL_WIFI, (const char *)ap->ssid, info);
        if (supported) {
            lv_obj_add_event_cb(row, network_clicked_cb, LV_EVENT_CLICKED, (void *)ap);
        } else {
            lv_obj_set_state(row, LV_STATE_DISABLED, true);
        }
    }
}

static void start_scan(void)
{
    if (wifi_scan_start() == ESP_OK) {
        list_message("Scanning...");
    } else {
        list_message("Scan failed - tap Scan to try again");
    }
}

/* ---------- main Wi-Fi screen ---------- */

static void update_status(void)
{
    wifi_status_t status;
    wifi_get_status(&status);

    switch (status.state) {
    case WIFI_STATE_CONNECTED:
        lv_label_set_text_fmt(s_status, "Connected to %s\n%s   %d dBm", status.ssid, status.ip, status.rssi);
        break;
    case WIFI_STATE_CONNECTING:
        lv_label_set_text_fmt(s_status, "Connecting to %s...", status.ssid);
        break;
    default:
        lv_label_set_text(s_status, "Not connected - choose a network");
        break;
    }
    if (status.error) {
        lv_label_set_text_fmt(s_error, "Couldn't join %s: %s", status.error_ssid, status.error);
    } else {
        lv_label_set_text(s_error, "");
    }

    bool saved = status.state == WIFI_STATE_CONNECTED || status.state == WIFI_STATE_CONNECTING;
    lv_obj_set_hidden(s_forget, !saved);
}

static void refresh_cb(lv_timer_t *timer)
{
    update_status();
    int gen = wifi_scan_generation();
    if (gen != s_seen_scan) {
        s_seen_scan = gen;
        list_rebuild();
    }
}

static void back_cb(lv_event_t *e)
{
    lv_timer_pause(s_timer);
    lv_screen_load(s_return_screen);
}

static void scan_cb(lv_event_t *e)
{
    start_scan();
}

static void forget_cb(lv_event_t *e)
{
    wifi_forget();
    update_status();
    list_rebuild();
}

static void build(void)
{
    s_screen = new_screen();

    add_header_button(s_screen, LV_SYMBOL_LEFT " Back", LV_ALIGN_TOP_LEFT, back_cb);
    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    add_header_button(s_screen, LV_SYMBOL_REFRESH " Scan", LV_ALIGN_TOP_RIGHT, scan_cb);

    s_status = lv_label_create(s_screen);
    lv_obj_set_width(s_status, 340);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 12, 56);

    s_error = lv_label_create(s_screen);
    lv_obj_set_width(s_error, 340);
    lv_obj_set_style_text_color(s_error, lv_color_hex(COLOR_ERROR), 0);
    lv_obj_align(s_error, LV_ALIGN_TOP_LEFT, 12, 100);

    s_forget = lv_button_create(s_screen);
    lv_obj_set_size(s_forget, 100, 40);
    lv_obj_align(s_forget, LV_ALIGN_TOP_RIGHT, -8, 58);
    lv_obj_set_style_bg_color(s_forget, lv_color_hex(0x5A2A2A), 0);
    lv_obj_add_event_cb(s_forget, forget_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *forget_label = lv_label_create(s_forget);
    lv_label_set_text(forget_label, "Forget");
    lv_obj_center(forget_label);

    s_list = lv_obj_create(s_screen);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(COLOR_LIST), 0);
    lv_obj_set_size(s_list, 464, 190);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_timer = lv_timer_create(refresh_cb, 500, NULL);
    lv_timer_pause(s_timer);
}

void wifi_screen_open(void)
{
    if (!s_screen) {
        build();
    }
    s_return_screen = lv_screen_active();
    s_seen_scan = wifi_scan_generation();
    update_status();
    start_scan();
    lv_timer_resume(s_timer);
    lv_screen_load(s_screen);
}
