#include <stdio.h>

#include "audio.h"
#include "board.h"
#include "clock.h"
#include "display.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "led.h"
#include "sdcard.h"
#include "freertos/task.h"
#include "touch.h"
#include "wifi.h"
#include "wifi_screen.h"

#define UI_WIDTH            360
#define BUTTON_GAP          6
#define BUTTON_WIDTH        ((UI_WIDTH - 2 * BUTTON_GAP) / 3)
#define DEFAULT_BRIGHTNESS  80
#define MIN_BRIGHTNESS      10      // keeps the slider from blacking out the screen

static const char *TAG = "main";

static lv_obj_t *s_beep_button;
static lv_obj_t *s_record_button;
static lv_obj_t *s_mic_bar;
static lv_obj_t *s_status;
static lv_obj_t *s_clock;
static lv_obj_t *s_sd;
static lv_obj_t *s_wifi_label;

static void beep_clicked_cb(lv_event_t *e)
{
    audio_beep();
}

static void record_clicked_cb(lv_event_t *e)
{
    audio_record_and_play();
}

static void led_clicked_cb(lv_event_t *e)
{
    static const struct {
        const char *name;
        uint8_t r, g, b;
    } colors[] = {
        {"Off", 0, 0, 0},
        {"Red", 255, 0, 0},
        {"Green", 0, 255, 0},
        {"Blue", 0, 0, 255},
        {"White", 255, 255, 255},
    };
    static int index;
    index = (index + 1) % (int)(sizeof(colors) / sizeof(colors[0]));
    led_set(colors[index].r, colors[index].g, colors[index].b);

    lv_obj_t *label = lv_obj_get_child(lv_event_get_target(e), 0);
    lv_label_set_text_fmt(label, "LED: %s", colors[index].name);
}

static void volume_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *label = lv_event_get_user_data(e);
    int32_t volume = lv_slider_get_value(slider);
    lv_label_set_text_fmt(label, "Volume %" LV_PRId32 "%%", volume);
    audio_set_volume(volume);
}

static void brightness_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *label = lv_event_get_user_data(e);
    int32_t brightness = lv_slider_get_value(slider);
    lv_label_set_text_fmt(label, "Brightness %" LV_PRId32 "%%", brightness);
    display_set_brightness(brightness);
}

static void audio_status_cb(lv_timer_t *timer)
{
    static const char *const status_text[] = {
        [AUDIO_IDLE] = "Speak to see the mic level",
        [AUDIO_BEEPING] = "Beeping",
        [AUDIO_RECORDING] = "Recording... speak now",
        [AUDIO_PLAYING] = "Playing back",
    };
    audio_state_t state = audio_get_state();
    lv_label_set_text(s_status, status_text[state]);
    lv_bar_set_value(s_mic_bar, audio_get_mic_level(), LV_ANIM_OFF);

    bool busy = state != AUDIO_IDLE;
    lv_obj_set_state(s_beep_button, LV_STATE_DISABLED, busy);
    lv_obj_set_state(s_record_button, LV_STATE_DISABLED, busy);
}

static void wifi_clicked_cb(lv_event_t *e)
{
    wifi_screen_open();
}

static void status_bar_cb(lv_timer_t *timer)
{
    struct tm now;
    if (clock_now(&now)) {
        char text[32];
        strftime(text, sizeof(text), "%a %b %e  %H:%M", &now);
        lv_label_set_text(s_clock, text);
    } else {
        lv_label_set_text(s_clock, "--:--");
    }

    uint64_t sd_total, sd_free;
    if (sdcard_get_space(&sd_total, &sd_free)) {
        // LVGL's own printf has no float support, so format with the C library
        char text[32];
        snprintf(text, sizeof(text), LV_SYMBOL_SD_CARD " %.1f GB free", sd_free / 1e9);
        lv_label_set_text(s_sd, text);
    } else {
        lv_label_set_text(s_sd, LV_SYMBOL_SD_CARD " none");
    }
    lv_obj_align_to(s_sd, s_clock, LV_ALIGN_OUT_RIGHT_MID, 18, 0);

    wifi_status_t wifi;
    wifi_get_status(&wifi);
    switch (wifi.state) {
    case WIFI_STATE_CONNECTED:
        lv_label_set_text_fmt(s_wifi_label, LV_SYMBOL_WIFI "  %s", wifi.ssid);
        break;
    case WIFI_STATE_CONNECTING:
        lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI "  Connecting...");
        break;
    default:
        lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI "  Set up Wi-Fi");
        break;
    }
}

// Clock on the left; Wi-Fi state on the right, which opens the Wi-Fi screen when tapped.
static void build_status_bar(lv_obj_t *scr)
{
    s_clock = lv_label_create(scr);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(0x9AA5B1), 0);
    lv_obj_align(s_clock, LV_ALIGN_TOP_LEFT, 12, 10);

    s_sd = lv_label_create(scr);
    lv_obj_set_style_text_color(s_sd, lv_color_hex(0x9AA5B1), 0);

    lv_obj_t *wifi = lv_button_create(scr);
    lv_obj_set_style_bg_opa(wifi, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(wifi, 0, 0);
    lv_obj_set_style_pad_all(wifi, 6, 0);
    lv_obj_align(wifi, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_add_event_cb(wifi, wifi_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_wifi_label = lv_label_create(wifi);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x9AA5B1), 0);
    lv_obj_set_style_max_width(s_wifi_label, 150, 0);
    lv_label_set_long_mode(s_wifi_label, LV_LABEL_LONG_DOT);

    status_bar_cb(NULL);
    lv_timer_create(status_bar_cb, 1000, NULL);
}

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int x_ofs)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, BUTTON_WIDTH, 56);
    lv_obj_align(button, LV_ALIGN_TOP_MID, x_ofs, 100);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *add_caption(lv_obj_t *parent, const char *text, lv_obj_t *below)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9AA5B1), 0);
    lv_obj_align_to(label, below, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    return label;
}

static void build_ui(bool touch_ok, bool audio_ok)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_16, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Hello, world!");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

    build_status_bar(scr);

    if (!touch_ok || !audio_ok) {
        lv_obj_t *err = lv_label_create(scr);
        lv_label_set_text(err, !touch_ok ? "Touch controller not found" : "Audio codec not found");
        lv_obj_set_style_text_color(err, lv_color_hex(0xFF6B6B), 0);
        lv_obj_center(err);
        return;
    }

    // Three buttons spanning UI_WIDTH, so their edges line up with the mic bar below
    const int button_step = BUTTON_WIDTH + BUTTON_GAP;
    s_beep_button = add_button(scr, "Beep", beep_clicked_cb, -button_step);
    char record_text[16];
    lv_snprintf(record_text, sizeof(record_text), "Record %d s", AUDIO_RECORD_SECONDS);
    s_record_button = add_button(scr, record_text, record_clicked_cb, 0);
    add_button(scr, "LED: Off", led_clicked_cb, button_step);

    lv_obj_t *mic_caption = add_caption(scr, "Microphone", s_beep_button);
    s_mic_bar = lv_bar_create(scr);
    lv_obj_set_size(s_mic_bar, UI_WIDTH, 14);
    lv_obj_align_to(s_mic_bar, mic_caption, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);

    // Volume and brightness side by side, in the same columns as the two buttons
    lv_obj_t *volume_caption = add_caption(scr, "", s_mic_bar);
    lv_obj_t *volume = lv_slider_create(scr);
    lv_obj_set_width(volume, UI_WIDTH / 2 - 30);
    lv_obj_align_to(volume, volume_caption, LV_ALIGN_OUT_BOTTOM_LEFT, 10, 14);
    lv_obj_add_event_cb(volume, volume_changed_cb, LV_EVENT_VALUE_CHANGED, volume_caption);
    lv_slider_set_value(volume, 60, LV_ANIM_OFF);
    lv_obj_send_event(volume, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *brightness_caption = add_caption(scr, "", s_mic_bar);
    lv_obj_align_to(brightness_caption, s_mic_bar, LV_ALIGN_OUT_BOTTOM_LEFT, UI_WIDTH / 2 + 10, 16);
    lv_label_set_text_fmt(brightness_caption, "Brightness %d%%", DEFAULT_BRIGHTNESS);
    lv_obj_t *brightness = lv_slider_create(scr);
    lv_obj_set_width(brightness, UI_WIDTH / 2 - 30);
    lv_obj_align_to(brightness, brightness_caption, LV_ALIGN_OUT_BOTTOM_LEFT, 10, 14);
    lv_slider_set_range(brightness, MIN_BRIGHTNESS, 100);
    lv_slider_set_value(brightness, DEFAULT_BRIGHTNESS, LV_ANIM_OFF);
    // No initial event here: app_main turns the backlight on once the first frame is drawn
    lv_obj_add_event_cb(brightness, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, brightness_caption);

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_timer_create(audio_status_cb, 50, NULL);
}

void app_main(void)
{
    lv_display_t *disp = display_init();
    i2c_master_bus_handle_t i2c_bus = board_i2c_init();
    lv_indev_t *touch = touch_init(disp, i2c_bus);
    esp_err_t audio_err = audio_init(i2c_bus);
    led_init();     // logs its own failure; the LED button is then a no-op
    sdcard_init();  // logs its own failure; the status bar then shows no card
    ESP_ERROR_CHECK(wifi_init());
    clock_init();

    lvgl_port_lock(0);
    build_ui(touch != NULL, audio_err == ESP_OK);
    lvgl_port_unlock();

    // Let LVGL flush the first frame before lighting the panel
    vTaskDelay(pdMS_TO_TICKS(100));
    display_set_brightness(DEFAULT_BRIGHTNESS);
    ESP_LOGI(TAG, "UI is up, touch %s, audio %s; free internal RAM %u KB, PSRAM %u KB",
             touch ? "enabled" : "unavailable", audio_err == ESP_OK ? "enabled" : "unavailable",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}
