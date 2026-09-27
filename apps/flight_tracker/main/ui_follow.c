/*
 * Typing in a flight to follow: an upper-case keyboard and a text field. The flight task
 * does the lookup; the map's status line reports how it went.
 */
#include "flights.h"
#include "ui.h"

static lv_obj_t *s_screen;
static lv_obj_t *s_text;

static void close_screen(void)
{
    lv_textarea_set_text(s_text, "");
    lv_screen_load(ui_map_screen());
}

static void keyboard_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        const char *query = lv_textarea_get_text(s_text);
        if (!query[0]) {
            return;
        }
        flights_follow(query);
    }
    close_screen();
}

static void cancel_cb(lv_event_t *e)
{
    close_screen();
}

static void build(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(s_screen, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_screen, &lv_font_montserrat_16, 0);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_text(title, "Follow a flight");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

    lv_obj_t *cancel = lv_button_create(s_screen);
    lv_obj_set_height(cancel, 40);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(cancel);
    lv_label_set_text(label, "Cancel");
    lv_obj_center(label);

    s_text = lv_textarea_create(s_screen);
    lv_textarea_set_one_line(s_text, true);
    lv_textarea_set_max_length(s_text, 12);
    lv_textarea_set_placeholder_text(s_text, "UA123, UAL123 or N12345");
    lv_obj_set_width(s_text, 456);
    lv_obj_align(s_text, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *keyboard = lv_keyboard_create(s_screen);
    lv_obj_set_height(keyboard, 196);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_textarea(keyboard, s_text);
    lv_obj_add_event_cb(keyboard, keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_cb, LV_EVENT_CANCEL, NULL);
}

void ui_follow_open(void)
{
    if (!s_screen) {
        build();
    }
    lv_screen_load(s_screen);
}
