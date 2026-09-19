#include "alarms.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define NVS_NAMESPACE   "alarm_clock"
#define NVS_KEY         "alarms"
#define STORE_VERSION   1

// Layout saved to NVS; bump STORE_VERSION if it changes
typedef struct {
    uint8_t version;
    uint8_t count;
    alarm_t items[ALARMS_MAX];
} alarm_store_t;

static const char *TAG = "alarms";

static SemaphoreHandle_t s_lock;
static alarm_store_t s_store = {.version = STORE_VERSION};

static void save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "can't open NVS; alarms not saved");
        return;
    }
    nvs_set_blob(nvs, NVS_KEY, &s_store, sizeof(s_store));
    nvs_commit(nvs);
    nvs_close(nvs);
}

void alarms_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    nvs_handle_t nvs;
    alarm_store_t loaded;
    size_t size = sizeof(loaded);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_blob(nvs, NVS_KEY, &loaded, &size) == ESP_OK && size == sizeof(loaded) &&
            loaded.version == STORE_VERSION && loaded.count <= ALARMS_MAX) {
            s_store = loaded;
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "%d alarm(s) loaded", s_store.count);
}

int alarms_get(alarm_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = s_store.count;
    memcpy(out, s_store.items, count * sizeof(alarm_t));
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t alarms_put(int index, const alarm_t *alarm)
{
    if (alarm->hour > 23 || alarm->minute > 59 || !(alarm->days & ALARM_EVERY_DAY)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (index >= 0 && index < s_store.count) {
        s_store.items[index] = *alarm;
    } else if (index == s_store.count && s_store.count < ALARMS_MAX) {
        s_store.items[s_store.count++] = *alarm;
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        save();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t alarms_remove(int index)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (index >= 0 && index < s_store.count) {
        memmove(&s_store.items[index], &s_store.items[index + 1],
                (s_store.count - index - 1) * sizeof(alarm_t));
        s_store.count--;
        save();
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool alarms_next(time_t after, time_t *when)
{
    alarm_t alarms[ALARMS_MAX];
    int count = alarms_get(alarms);

    struct tm base;
    localtime_r(&after, &base);
    bool found = false;
    // Today plus the next seven days covers every weekly pattern
    for (int day = 0; day <= 7; day++) {
        for (int i = 0; i < count; i++) {
            if (!alarms[i].enabled) {
                continue;
            }
            struct tm t = base;
            t.tm_mday += day;
            t.tm_hour = alarms[i].hour;
            t.tm_min = alarms[i].minute;
            t.tm_sec = 0;
            t.tm_isdst = -1;            // let mktime work out DST for that date
            time_t candidate = mktime(&t);  // also normalizes t.tm_wday
            if (candidate > after && (alarms[i].days & (1 << t.tm_wday)) &&
                (!found || candidate < *when)) {
                *when = candidate;
                found = true;
            }
        }
    }
    return found;
}

void alarms_describe_days(uint8_t days, char *buf, size_t len)
{
    static const char *const names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    days &= ALARM_EVERY_DAY;
    if (days == ALARM_EVERY_DAY) {
        snprintf(buf, len, "Every day");
    } else if (days == ALARM_WEEKDAYS) {
        snprintf(buf, len, "Weekdays");
    } else if (days == (ALARM_EVERY_DAY & ~ALARM_WEEKDAYS)) {
        snprintf(buf, len, "Weekends");
    } else {
        buf[0] = '\0';
        for (int d = 0; d < 7; d++) {
            if (days & (1 << d)) {
                size_t used = strlen(buf);
                snprintf(buf + used, len - used, "%s%s", used ? " " : "", names[d]);
            }
        }
    }
}

void alarms_format_time(int hour, int minute, char *buf, size_t len)
{
    snprintf(buf, len, "%02d:%02d", hour, minute);
}
