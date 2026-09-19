#include "clock.h"

#include <stdlib.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"

// POSIX TZ string for local time; defaults to US Pacific (America/Los_Angeles).
// For other zones see e.g. https://github.com/nayarsystems/posix_tz_db
#define CLOCK_TZ        "PST8PDT,M3.2.0,M11.1.0"
#define CLOCK_NTP_SERVER "pool.ntp.org"

static const char *TAG = "clock";

static void on_time_sync(struct timeval *tv)
{
    struct tm now;
    localtime_r(&tv->tv_sec, &now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &now);
    ESP_LOGI(TAG, "time synced: %s", buf);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_netif_sntp_start();
}

void clock_init(void)
{
    setenv("TZ", CLOCK_TZ, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CLOCK_NTP_SERVER);
    cfg.start = false;              // started (or restarted) once there's an IP address
    cfg.sync_cb = on_time_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));
}

bool clock_now(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
    // Before the first sync the RTC counts up from 1970
    return out->tm_year + 1900 >= 2025;
}
