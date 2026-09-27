#include "geoip.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "net_http.h"

// Two no-key IP geolocation services with the same field names; the second is a fallback
static const char *const URLS[] = {
    "https://ipapi.co/json/",
    "https://ipwho.is/",
};

static const char *TAG = "geoip";

bool geoip_locate(geoip_location_t *out, char *buf, int size)
{
    for (int i = 0; i < sizeof(URLS) / sizeof(URLS[0]); i++) {
        if (net_https_get(URLS[i], buf, size) < 0) {
            continue;
        }
        cJSON *root = cJSON_Parse(buf);
        const cJSON *lat = cJSON_GetObjectItem(root, "latitude");
        const cJSON *lon = cJSON_GetObjectItem(root, "longitude");
        const cJSON *city = cJSON_GetObjectItem(root, "city");
        const cJSON *country = cJSON_GetObjectItem(root, "country_code");
        bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
        if (ok) {
            memset(out, 0, sizeof(*out));
            out->latitude = lat->valuedouble;
            out->longitude = lon->valuedouble;
            strlcpy(out->city, cJSON_IsString(city) ? city->valuestring : "", sizeof(out->city));
            strlcpy(out->country, cJSON_IsString(country) ? country->valuestring : "", sizeof(out->country));
        }
        cJSON_Delete(root);
        if (ok) {
            ESP_LOGI(TAG, "%s, %s (%.2f, %.2f)", out->city, out->country, out->latitude, out->longitude);
            return true;
        }
    }
    return false;
}
