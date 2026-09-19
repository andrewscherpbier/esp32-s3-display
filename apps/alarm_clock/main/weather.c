#include "weather.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi.h"

#define REFRESH_MIN         30
#define RETRY_SEC           60
#define RESPONSE_MAX        4096
#define NVS_NAMESPACE       "weather"

// Two no-key IP geolocation services with the same field names; the second is a fallback
static const char *const LOCATE_URLS[] = {
    "https://ipapi.co/json/",
    "https://ipwho.is/",
};

typedef struct {
    double latitude;
    double longitude;
    char city[48];
    char country[4];
} location_t;

static const char *TAG = "weather";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_t s_weather;

// GET `url` into `buf`; returns the body length, or -1 on any failure.
static int https_get(const char *url, char *buf, int size)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }
    int total = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        total = 0;
        int n;
        while (total < size - 1 && (n = esp_http_client_read(client, buf + total, size - 1 - total)) > 0) {
            total += n;
        }
        buf[total] = '\0';
        if (status != 200) {
            ESP_LOGW(TAG, "%s: HTTP %d", url, status);
            total = -1;
        }
    } else {
        ESP_LOGW(TAG, "%s: connection failed", url);
    }
    esp_http_client_cleanup(client);
    return total;
}

static bool load_location(location_t *loc)
{
    nvs_handle_t nvs;
    size_t size = sizeof(*loc);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_blob(nvs, "location", loc, &size) == ESP_OK && size == sizeof(*loc);
    nvs_close(nvs);
    return ok;
}

static void save_location(const location_t *loc)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "location", loc, sizeof(*loc));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool locate(location_t *loc, char *buf)
{
    for (int i = 0; i < sizeof(LOCATE_URLS) / sizeof(LOCATE_URLS[0]); i++) {
        if (https_get(LOCATE_URLS[i], buf, RESPONSE_MAX) < 0) {
            continue;
        }
        cJSON *root = cJSON_Parse(buf);
        const cJSON *lat = cJSON_GetObjectItem(root, "latitude");
        const cJSON *lon = cJSON_GetObjectItem(root, "longitude");
        const cJSON *city = cJSON_GetObjectItem(root, "city");
        const cJSON *country = cJSON_GetObjectItem(root, "country_code");
        bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
        if (ok) {
            memset(loc, 0, sizeof(*loc));
            loc->latitude = lat->valuedouble;
            loc->longitude = lon->valuedouble;
            strlcpy(loc->city, cJSON_IsString(city) ? city->valuestring : "", sizeof(loc->city));
            strlcpy(loc->country, cJSON_IsString(country) ? country->valuestring : "", sizeof(loc->country));
        }
        cJSON_Delete(root);
        if (ok) {
            ESP_LOGI(TAG, "location: %s, %s (%.2f, %.2f)", loc->city, loc->country, loc->latitude, loc->longitude);
            return true;
        }
    }
    return false;
}

static bool fetch(const location_t *loc, char *buf)
{
    // The US (and a couple of others) use Fahrenheit
    const bool fahrenheit = !strcmp(loc->country, "US") || !strcmp(loc->country, "LR") ||
                            !strcmp(loc->country, "MM");
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,is_day"
             "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto%s",
             loc->latitude, loc->longitude, fahrenheit ? "&temperature_unit=fahrenheit" : "");
    if (https_get(url, buf, RESPONSE_MAX) < 0) {
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    const cJSON *current = cJSON_GetObjectItem(root, "current");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    const cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
    const cJSON *code = cJSON_GetObjectItem(current, "weather_code");
    const cJSON *is_day = cJSON_GetObjectItem(current, "is_day");
    const cJSON *high = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_max"), 0);
    const cJSON *low = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_min"), 0);
    bool ok = cJSON_IsNumber(temp) && cJSON_IsNumber(code) && cJSON_IsNumber(high) && cJSON_IsNumber(low);
    if (ok) {
        portENTER_CRITICAL(&s_lock);
        s_weather.valid = true;
        s_weather.temperature = temp->valuedouble;
        s_weather.high = high->valuedouble;
        s_weather.low = low->valuedouble;
        s_weather.code = code->valueint;
        s_weather.is_day = cJSON_IsNumber(is_day) ? is_day->valueint : true;
        s_weather.unit = fahrenheit ? 'F' : 'C';
        strlcpy(s_weather.city, loc->city, sizeof(s_weather.city));
        s_weather.updated = time(NULL);
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "%.1f %c, %s, high %.0f low %.0f", temp->valuedouble, fahrenheit ? 'F' : 'C',
                 weather_describe(code->valueint), high->valuedouble, low->valuedouble);
    }
    cJSON_Delete(root);
    return ok;
}

static bool wifi_connected(void)
{
    wifi_status_t status;
    wifi_get_status(&status);
    return status.state == WIFI_STATE_CONNECTED;
}

static void weather_task(void *arg)
{
    char *buf = heap_caps_malloc(RESPONSE_MAX, MALLOC_CAP_SPIRAM);
    location_t loc;
    bool located = load_location(&loc);
    if (located) {
        ESP_LOGI(TAG, "cached location: %s, %s", loc.city, loc.country);
    }

    for (;;) {
        if (!wifi_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!located && (located = locate(&loc, buf))) {
            save_location(&loc);
        }
        bool ok = located && fetch(&loc, buf);
        ESP_LOGI(TAG, "free internal RAM %u KB", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        vTaskDelay(pdMS_TO_TICKS((ok ? REFRESH_MIN * 60 : RETRY_SEC) * 1000));
    }
}

void weather_start(void)
{
    xTaskCreate(weather_task, "weather", 8192, NULL, 3, NULL);
}

void weather_get(weather_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_weather;
    portEXIT_CRITICAL(&s_lock);
}

const char *weather_describe(int code)
{
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: case 82: return "Showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    default: return "Unknown";
    }
}
