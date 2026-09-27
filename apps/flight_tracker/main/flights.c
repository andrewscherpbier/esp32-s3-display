/*
 * Aircraft data from the community ADS-B networks adsb.lol and adsb.fi (both free, no
 * key, same "readsb" JSON). adsb.lol is asked first and adsb.fi takes over when it fails.
 *
 * Every FLIGHTS_REFRESH_SEC one task fetches the aircraft around the area of interest
 * (the map view, or the followed plane) and, when following, the followed aircraft
 * itself, looked up anywhere in the world.
 */
#include "flights.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airlines.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "geoip.h"
#include "net_http.h"
#include "wifi.h"

#define RESPONSE_MAX        (384 * 1024)
#define RADIUS_MIN_NM       5
#define RADIUS_MAX_NM       100
#define LOST_AFTER_MISSES   3
#define PROVIDER_BACKOFF_US (5 * 60 * 1000000LL)   // after a failure (e.g. HTTP 429), prefer the other one

typedef enum {
    QUERY_POINT,
    QUERY_CALLSIGN,
    QUERY_REG,
    QUERY_HEX,
} query_t;

typedef enum {
    REQUEST_NONE,
    REQUEST_QUERY,
    REQUEST_HEX,
    REQUEST_OFF,
} request_t;

static const char *TAG = "flights";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char *s_response;
static net_http_session_t s_sessions[2];  // one per provider, kept connected
static int64_t s_backoff_until[2];
static flight_t *s_flights;         // published, guarded by s_lock
static flight_t *s_scratch;         // the task's working copy
static flights_info_t s_info;

// Set by the UI, guarded by s_lock
static double s_area_lat, s_area_lon, s_area_radius;
static bool s_area_set;
static request_t s_request;
static char s_request_text[16];
static char s_request_label[16];

// Task-only follow state
static char s_follow_query[16];
static char s_follow_hex[8];
static int s_follow_misses;

// Upper case, no spaces or dashes: "ua 123" -> "UA123"
static void normalize(const char *in, char *out, size_t len)
{
    size_t n = 0;
    for (; *in && n < len - 1; in++) {
        if (!isspace((unsigned char)*in) && *in != '-') {
            out[n++] = (char)toupper((unsigned char)*in);
        }
    }
    out[n] = '\0';
}

static void *psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void make_url(char *url, size_t len, int provider, query_t query, const char *value,
                     double lat, double lon, int radius)
{
    static const char *const lol[] = {"point", "callsign", "reg", "hex"};
    static const char *const fi[] = {"", "callsign", "registration", "hex"};
    if (query == QUERY_POINT) {
        snprintf(url, len, provider == 0 ? "https://api.adsb.lol/v2/point/%.4f/%.4f/%d"
                                         : "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d",
                 lat, lon, radius);
    } else {
        snprintf(url, len, provider == 0 ? "https://api.adsb.lol/v2/%s/%s" : "https://opendata.adsb.fi/api/v2/%s/%s",
                 provider == 0 ? lol[query] : fi[query], value);
    }
}

static void copy_string(char *dst, size_t len, const cJSON *item)
{
    const char *s = cJSON_IsString(item) ? item->valuestring : "";
    while (*s == ' ') {
        s++;
    }
    strlcpy(dst, s, len);
    for (int i = strlen(dst) - 1; i >= 0 && dst[i] == ' '; i--) {
        dst[i] = '\0';      // callsigns come space-padded
    }
}

static double number(const cJSON *obj, const char *key, const char *fallback, double otherwise)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(item) && fallback) {
        item = cJSON_GetObjectItem(obj, fallback);
    }
    return cJSON_IsNumber(item) ? item->valuedouble : otherwise;
}

// Parse a readsb-style response. Returns the number of aircraft with a position, or -1
// if the request or the JSON failed.
static int query(query_t query, const char *value, double lat, double lon, int radius,
                 flight_t *out, int max)
{
    // adsb.lol first, unless it failed recently and adsb.fi didn't
    const int64_t now = esp_timer_get_time();
    const int first = now < s_backoff_until[0] && now >= s_backoff_until[1] ? 1 : 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        const int provider = attempt == 0 ? first : 1 - first;
        char url[128];
        make_url(url, sizeof(url), provider, query, value, lat, lon, radius);
        if (net_http_session_get(s_sessions[provider], url, s_response, RESPONSE_MAX) < 0) {
            s_backoff_until[provider] = esp_timer_get_time() + PROVIDER_BACKOFF_US;
            continue;
        }
        cJSON *root = cJSON_Parse(s_response);
        const cJSON *list = cJSON_GetObjectItem(root, "ac");
        if (!cJSON_IsArray(list)) {
            list = cJSON_GetObjectItem(root, "aircraft");   // adsb.fi's area query
        }
        if (!cJSON_IsArray(list)) {
            cJSON_Delete(root);
            continue;
        }
        int n = 0;
        const cJSON *a;
        cJSON_ArrayForEach(a, list) {
            const cJSON *la = cJSON_GetObjectItem(a, "lat");
            const cJSON *lo = cJSON_GetObjectItem(a, "lon");
            if (n >= max || !cJSON_IsNumber(la) || !cJSON_IsNumber(lo)) {
                continue;
            }
            flight_t *f = &out[n++];
            memset(f, 0, sizeof(*f));
            copy_string(f->hex, sizeof(f->hex), cJSON_GetObjectItem(a, "hex"));
            copy_string(f->callsign, sizeof(f->callsign), cJSON_GetObjectItem(a, "flight"));
            copy_string(f->reg, sizeof(f->reg), cJSON_GetObjectItem(a, "r"));
            copy_string(f->type, sizeof(f->type), cJSON_GetObjectItem(a, "t"));
            f->lat = la->valuedouble;
            f->lon = lo->valuedouble;
            const cJSON *alt = cJSON_GetObjectItem(a, "alt_baro");
            f->on_ground = cJSON_IsString(alt) && !strcmp(alt->valuestring, "ground");
            f->altitude = cJSON_IsNumber(alt) ? alt->valueint : 0;
            f->track = number(a, "track", "true_heading", number(a, "mag_heading", NULL, 0));
            f->speed = number(a, "gs", NULL, 0);
            f->vertical_rate = number(a, "baro_rate", "geom_rate", 0);
            f->age = number(a, "seen_pos", "seen", 0);
        }
        cJSON_Delete(root);
        return n;
    }
    return -1;
}

static bool wifi_up(void)
{
    wifi_status_t status;
    wifi_get_status(&status);
    return status.state == WIFI_STATE_CONNECTED;
}

static void set_follow(follow_state_t state, const flight_t *target)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.follow = state;
    if (target) {
        s_info.target = *target;
    }
    xSemaphoreGive(s_lock);
}

// Resolve what the user typed (already normalized): as a flight number or callsign,
// then as a registration.
static void follow_search(void)
{
    char callsign[16];
    bool translated = airlines_to_callsign(s_follow_query, callsign, sizeof(callsign));

    flight_t found;
    int n = query(QUERY_CALLSIGN, callsign, 0, 0, 0, &found, 1);
    if (n <= 0 && translated) {
        n = query(QUERY_CALLSIGN, s_follow_query, 0, 0, 0, &found, 1);
    }
    if (n <= 0) {
        n = query(QUERY_REG, s_follow_query, 0, 0, 0, &found, 1);
    }
    if (n > 0) {
        ESP_LOGI(TAG, "following %s: %s (%s)", s_follow_query, found.callsign, found.hex);
        strlcpy(s_follow_hex, found.hex, sizeof(s_follow_hex));
        s_follow_misses = 0;
        set_follow(FOLLOW_TRACKING, &found);
    } else {
        ESP_LOGI(TAG, "%s not found", s_follow_query);
        set_follow(FOLLOW_NOT_FOUND, NULL);
    }
}

static void follow_update(void)
{
    flight_t found;
    if (query(QUERY_HEX, s_follow_hex, 0, 0, 0, &found, 1) > 0) {
        s_follow_misses = 0;
        set_follow(FOLLOW_TRACKING, &found);
    } else if (++s_follow_misses >= LOST_AFTER_MISSES) {
        set_follow(FOLLOW_LOST, NULL);
    }
}

// Pick up a follow/unfollow request from the UI.
static void take_request(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    request_t request = s_request;
    s_request = REQUEST_NONE;
    if (request == REQUEST_QUERY || request == REQUEST_HEX) {
        s_info.follow = FOLLOW_SEARCHING;
        strlcpy(s_info.follow_label, s_request_label, sizeof(s_info.follow_label));
        memset(&s_info.target, 0, sizeof(s_info.target));
    } else if (request == REQUEST_OFF) {
        s_info.follow = FOLLOW_OFF;
    }
    xSemaphoreGive(s_lock);

    if (request == REQUEST_QUERY) {
        strlcpy(s_follow_query, s_request_text, sizeof(s_follow_query));
        s_follow_hex[0] = '\0';
    } else if (request == REQUEST_HEX) {
        strlcpy(s_follow_hex, s_request_text, sizeof(s_follow_hex));
        s_follow_query[0] = '\0';
        s_follow_misses = 0;
    }
}

static void flights_task(void *arg)
{
    s_sessions[0] = net_http_session_create();
    s_sessions[1] = net_http_session_create();
    for (;;) {
        const int64_t started = esp_timer_get_time();
        if (!wifi_up()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!s_info.home_known) {
            geoip_location_t loc;
            if (!geoip_locate(&loc, s_response, RESPONSE_MAX)) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_info.home_lat = loc.latitude;
            s_info.home_lon = loc.longitude;
            s_info.home_known = true;
            xSemaphoreGive(s_lock);
        }

        take_request();
        follow_state_t follow = s_info.follow;
        if (follow == FOLLOW_SEARCHING && s_follow_query[0]) {
            follow_search();
        } else if ((follow == FOLLOW_SEARCHING || follow == FOLLOW_TRACKING || follow == FOLLOW_LOST) && s_follow_hex[0]) {
            follow_update();
        }

        // Around the followed plane if there is one, otherwise the map area
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool around_target = (s_info.follow == FOLLOW_TRACKING || s_info.follow == FOLLOW_LOST);
        double lat = around_target ? s_info.target.lat : s_area_set ? s_area_lat : s_info.home_lat;
        double lon = around_target ? s_info.target.lon : s_area_set ? s_area_lon : s_info.home_lon;
        double radius = s_area_set ? s_area_radius : 25;
        xSemaphoreGive(s_lock);
        int r = radius < RADIUS_MIN_NM ? RADIUS_MIN_NM : radius > RADIUS_MAX_NM ? RADIUS_MAX_NM : (int)radius;

        int n = query(QUERY_POINT, NULL, lat, lon, r, s_scratch, FLIGHTS_MAX);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.ok = n >= 0;
        if (n >= 0) {
            memcpy(s_flights, s_scratch, n * sizeof(flight_t));
            s_info.count = n;
            s_info.fetched_us = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
        ESP_LOGD(TAG, "%d aircraft within %d nm", n, r);

        // Sleep until the next refresh is due, or until the UI changes the area or what to follow
        int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
        int64_t wait_ms = FLIGHTS_REFRESH_SEC * 1000 - elapsed_ms;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms > 100 ? wait_ms : 100));
    }
}

void flights_start(bool home_known, double home_lat, double home_lon)
{
    // cJSON allocates a node per value; keep the parse trees out of internal RAM
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);

    s_lock = xSemaphoreCreateMutex();
    s_response = heap_caps_malloc(RESPONSE_MAX, MALLOC_CAP_SPIRAM);
    s_flights = heap_caps_calloc(FLIGHTS_MAX, sizeof(flight_t), MALLOC_CAP_SPIRAM);
    s_scratch = heap_caps_calloc(FLIGHTS_MAX, sizeof(flight_t), MALLOC_CAP_SPIRAM);
    s_info.home_known = home_known;
    s_info.home_lat = home_lat;
    s_info.home_lon = home_lon;
    // Stack for TLS and cJSON
    xTaskCreate(flights_task, "flights", 8192, NULL, 4, &s_task);
}

void flights_set_area(double lat, double lon, double radius_nm)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Refresh at once only if the view moved or zoomed noticeably
    bool big_change = !s_area_set || fabs(radius_nm - s_area_radius) > s_area_radius * 0.3 ||
                      fabs(lat - s_area_lat) * 60 > s_area_radius * 0.3 ||
                      fabs(lon - s_area_lon) * 60 > s_area_radius * 0.3;
    s_area_lat = lat;
    s_area_lon = lon;
    s_area_radius = radius_nm;
    s_area_set = true;
    xSemaphoreGive(s_lock);
    if (big_change) {
        xTaskNotifyGive(s_task);
    }
}

static void request(request_t kind, const char *text, const char *label)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_request = kind;
    strlcpy(s_request_text, text, sizeof(s_request_text));
    strlcpy(s_request_label, label, sizeof(s_request_label));
    if (kind == REQUEST_QUERY || kind == REQUEST_HEX) {
        s_info.follow = FOLLOW_SEARCHING;   // show it straight away
        strlcpy(s_info.follow_label, label, sizeof(s_info.follow_label));
    } else {
        s_info.follow = FOLLOW_OFF;
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void flights_follow(const char *query_text)
{
    // The label shows what the user typed; the task translates it to a callsign
    char normalized[16];
    normalize(query_text, normalized, sizeof(normalized));
    request(REQUEST_QUERY, normalized, normalized);
}

void flights_follow_hex(const char *hex, const char *label)
{
    request(REQUEST_HEX, hex, label);
}

void flights_unfollow(void)
{
    request(REQUEST_OFF, "", "");
}

int flights_get(flight_t *out, int max, flights_info_t *info)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_info.count < max ? s_info.count : max;
    memcpy(out, s_flights, n * sizeof(flight_t));
    *info = s_info;
    xSemaphoreGive(s_lock);
    return n;
}
