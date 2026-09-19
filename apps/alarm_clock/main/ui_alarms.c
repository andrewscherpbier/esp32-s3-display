/*
 * Alarm list (with on/off switches and a chime test) and the alarm editor: 24-hour hour
 * and minute rollers plus seven weekday toggles.
 */
#include <stdio.h>
#include <string.h>

#include "alarms.h"
#include "ui.h"
#include "wake.h"

static lv_obj_t *s_return_screen;
static lv_obj_t *s_list_screen;
static lv_obj_t *s_list;
static lv_obj_t *s_add;

static lv_obj_t *s_edit_screen;
static lv_obj_t *s_edit_title;
static lv_obj_t *s_hour;
static lv_obj_t *s_minute;
static lv_obj_t *s_day_buttons[7];
static lv_obj_t *s_delete;
static lv_obj_t *s_hint;
static int s_edit_index;        // alarm being edited; == count for a new one

static void list_rebuild(void);

/* ---------- editor ---------- */

static void edit_close(void)
{
    list_rebuild();
    lv_screen_load(s_list_screen);
}

static void edit_cancel_cb(lv_event_t *e)
{
    edit_close();
}

static void edit_save_cb(lv_event_t *e)
{
    alarm_t alarm = {.enabled = true};
    alarm.hour = lv_roller_get_selected(s_hour);
    alarm.minute = lv_roller_get_selected(s_minute);
    for (int d = 0; d < 7; d++) {
        if (lv_obj_has_state(s_day_buttons[d], LV_STATE_CHECKED)) {
            alarm.days |= 1 << d;
        }
    }
    if (!alarm.days) {
        lv_label_set_text(s_hint, "Pick at least one day");
        return;
    }
    alarms_put(s_edit_index, &alarm);
    edit_close();
}

static void edit_delete_cb(lv_event_t *e)
{
    alarms_remove(s_edit_index);
    edit_close();
}

static lv_obj_t *add_roller(lv_obj_t *parent, const char *options, lv_roller_mode_t mode, int width, int x)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, mode);
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_set_width(roller, width);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_32, 0);
    lv_obj_set_style_bg_color(roller, lv_color_hex(0x18212B), 0);
    lv_obj_set_style_text_color(roller, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_width(roller, 0, 0);
    lv_obj_align(roller, LV_ALIGN_TOP_MID, x, 58);
    return roller;
}

static void edit_build(void)
{
    s_edit_screen = ui_new_screen();
    ui_header_button(s_edit_screen, "Cancel", LV_ALIGN_TOP_LEFT, edit_cancel_cb);
    lv_obj_t *save = ui_header_button(s_edit_screen, LV_SYMBOL_OK " Save", LV_ALIGN_TOP_RIGHT, edit_save_cb);
    lv_obj_set_style_bg_color(save, lv_color_hex(0xD9822B), 0);
    s_edit_title = lv_label_create(s_edit_screen);
    lv_obj_set_style_text_font(s_edit_title, &lv_font_montserrat_24, 0);
    lv_obj_align(s_edit_title, LV_ALIGN_TOP_MID, 0, 16);

    static char hours[96];
    static char minutes[256];
    hours[0] = minutes[0] = '\0';
    for (int h = 0; h < 24; h++) {
        snprintf(hours + strlen(hours), sizeof(hours) - strlen(hours), h ? "\n%02d" : "%02d", h);
    }
    for (int m = 0; m < 60; m++) {
        snprintf(minutes + strlen(minutes), sizeof(minutes) - strlen(minutes), m ? "\n%02d" : "%02d", m);
    }
    s_hour = add_roller(s_edit_screen, hours, LV_ROLLER_MODE_INFINITE, 90, -55);
    s_minute = add_roller(s_edit_screen, minutes, LV_ROLLER_MODE_INFINITE, 90, 55);

    static const char *const letters[] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int d = 0; d < 7; d++) {
        lv_obj_t *b = lv_button_create(s_edit_screen);
        lv_obj_set_checkable(b, true);
        lv_obj_set_size(b, 50, 44);
        lv_obj_align(b, LV_ALIGN_TOP_MID, (d - 3) * 58, 218);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3440), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xD9822B), LV_STATE_CHECKED);
        lv_obj_t *label = lv_label_create(b);
        lv_label_set_text(label, letters[d]);
        lv_obj_center(label);
        s_day_buttons[d] = b;
    }

    s_delete = ui_header_button(s_edit_screen, LV_SYMBOL_TRASH " Delete", LV_ALIGN_BOTTOM_LEFT, edit_delete_cb);
    lv_obj_set_style_bg_color(s_delete, lv_color_hex(0x6B2A2A), 0);
    s_hint = lv_label_create(s_edit_screen);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xFF8A80), 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_RIGHT, -16, -20);
}

static void edit_open(int index)
{
    if (!s_edit_screen) {
        edit_build();
    }
    alarm_t alarms[ALARMS_MAX];
    int count = alarms_get(alarms);
    s_edit_index = index;
    // A new alarm starts at 07:00 on weekdays
    alarm_t alarm = index < count ? alarms[index] : (alarm_t){.hour = 7, .minute = 0, .days = ALARM_WEEKDAYS};

    lv_label_set_text(s_edit_title, index < count ? "Edit alarm" : "New alarm");
    lv_roller_set_selected(s_hour, alarm.hour, LV_ANIM_OFF);
    lv_roller_set_selected(s_minute, alarm.minute, LV_ANIM_OFF);
    for (int d = 0; d < 7; d++) {
        lv_obj_set_state(s_day_buttons[d], LV_STATE_CHECKED, alarm.days & (1 << d));
    }
    lv_obj_set_hidden(s_delete, index >= count);
    lv_label_set_text(s_hint, "");
    lv_screen_load(s_edit_screen);
}

/* ---------- list ---------- */

static void row_clicked_cb(lv_event_t *e)
{
    edit_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void switch_changed_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    alarm_t alarms[ALARMS_MAX];
    if (index < alarms_get(alarms)) {
        alarms[index].enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        alarms_put(index, &alarms[index]);
    }
}

static void list_rebuild(void)
{
    alarm_t alarms[ALARMS_MAX];
    int count = alarms_get(alarms);
    lv_obj_clean(s_list);
    lv_obj_set_state(s_add, LV_STATE_DISABLED, count >= ALARMS_MAX);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_obj_set_style_text_color(empty, lv_color_hex(UI_COLOR_MUTED), 0);
        lv_label_set_text(empty, "No alarms yet. Tap \"Add\" to create one.");
        return;
    }
    for (int i = 0; i < count; i++) {
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_size(row, LV_PCT(100), 62);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x18212B), 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char text[32];
        lv_obj_t *time = lv_label_create(row);
        lv_obj_set_style_text_font(time, &lv_font_montserrat_32, 0);
        alarms_format_time(alarms[i].hour, alarms[i].minute, text, sizeof(text));
        lv_label_set_text(time, text);
        lv_obj_align(time, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_text_color(time, lv_color_hex(alarms[i].enabled ? UI_COLOR_TEXT : UI_COLOR_MUTED), 0);

        lv_obj_t *days = lv_label_create(row);
        lv_obj_set_style_text_color(days, lv_color_hex(UI_COLOR_MUTED), 0);
        alarms_describe_days(alarms[i].days, text, sizeof(text));
        lv_label_set_text(days, text);
        lv_obj_align(days, LV_ALIGN_LEFT_MID, 130, 0);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_state(sw, LV_STATE_CHECKED, alarms[i].enabled);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0xD9822B), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, switch_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
}

static void back_cb(lv_event_t *e)
{
    lv_screen_load(s_return_screen);
}

static void add_cb(lv_event_t *e)
{
    alarm_t alarms[ALARMS_MAX];
    edit_open(alarms_get(alarms));
}

static void test_cb(lv_event_t *e)
{
    wake_test();
}

static void list_build(void)
{
    s_list_screen = ui_new_screen();
    ui_header_button(s_list_screen, LV_SYMBOL_LEFT " Back", LV_ALIGN_TOP_LEFT, back_cb);
    lv_obj_t *title = lv_label_create(s_list_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_text(title, "Alarms");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
    s_add = ui_header_button(s_list_screen, LV_SYMBOL_PLUS " Add", LV_ALIGN_TOP_RIGHT, add_cb);

    s_list = lv_obj_create(s_list_screen);
    lv_obj_set_size(s_list, 464, 200);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    ui_header_button(s_list_screen, LV_SYMBOL_AUDIO " Test alarm", LV_ALIGN_BOTTOM_LEFT, test_cb);
    lv_obj_t *note = lv_label_create(s_list_screen);
    lv_obj_set_style_text_color(note, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_label_set_text_fmt(note, "Lights start %d min before", WAKE_SUNRISE_MINUTES);
    lv_obj_align(note, LV_ALIGN_BOTTOM_RIGHT, -12, -20);
}

void ui_alarms_open(void)
{
    if (!s_list_screen) {
        list_build();
    }
    s_return_screen = lv_screen_active();
    list_rebuild();
    lv_screen_load(s_list_screen);
}
