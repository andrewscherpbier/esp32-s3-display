/*
 * Wi-Fi station with credentials entered on screen.
 *
 * The Wi-Fi driver's own flash storage is turned off; this module saves the network in
 * its own NVS namespace, and only after the connection has succeeded. All state changes
 * happen in the default event loop task or in wifi_connect()/wifi_forget(), and are
 * guarded by s_lock so the UI can read a consistent snapshot.
 */
#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE       "wifi"
#define MAX_SCAN_RESULTS    20
#define NEW_NETWORK_TRIES   3       // attempts before a newly entered network is reported failed
#define RETRY_MIN_MS        2000
#define RETRY_MAX_MS        30000

static const char *TAG = "wifi";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_status_t s_status;
static bool s_trying_new;           // current credentials haven't connected yet
static int s_attempts;
static esp_timer_handle_t s_retry_timer;

static wifi_ap_record_t s_scan[MAX_SCAN_RESULTS];
static int s_scan_count;
static int s_scan_generation;

static void set_state(wifi_state_t state)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    portEXIT_CRITICAL(&s_lock);
}

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(nvs, "ssid", ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(nvs, "pass", pass, &pass_len) == ESP_OK;
    nvs_close(nvs);
    return ok && ssid[0];
}

static void save_credentials(const wifi_config_t *cfg)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "ssid", (const char *)cfg->sta.ssid);
    nvs_set_str(nvs, "pass", (const char *)cfg->sta.password);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void erase_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static esp_err_t apply_config(const char *ssid, const char *password)
{
    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "invalid network settings");

    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
    s_status.state = WIFI_STATE_CONNECTING;
    portEXIT_CRITICAL(&s_lock);
    s_attempts = 0;
    return ESP_OK;
}

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

static bool is_auth_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_CONNECTION_FAIL:
        return true;
    default:
        return false;
    }
}

static bool is_not_found(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static void on_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    // Our own esp_wifi_disconnect() (switching networks, forgetting) isn't a failure, and
    // events arriving after we've given up or forgotten the network must not restart it.
    wifi_status_t current;
    wifi_get_status(&current);
    if (ev->reason == WIFI_REASON_ASSOC_LEAVE ||
        current.state == WIFI_STATE_UNCONFIGURED || current.state == WIFI_STATE_FAILED) {
        return;
    }
    s_attempts++;
    ESP_LOGW(TAG, "disconnected from \"%.*s\", reason %d (attempt %d)", ev->ssid_len,
             (const char *)ev->ssid, ev->reason, s_attempts);

    if (s_trying_new && s_attempts >= NEW_NETWORK_TRIES) {
        s_trying_new = false;
        portENTER_CRITICAL(&s_lock);
        s_status.error = is_auth_failure(ev->reason) ? "Wrong password"
                       : is_not_found(ev->reason)    ? "Network not found"
                                                     : "Could not connect";
        strlcpy(s_status.error_ssid, s_status.ssid, sizeof(s_status.error_ssid));
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "giving up on \"%s\": %s", current.ssid, s_status.error);

        char ssid[33];
        char pass[65];
        if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) && apply_config(ssid, pass) == ESP_OK) {
            ESP_LOGI(TAG, "falling back to saved network \"%s\"", ssid);
            esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_MIN_MS * 1000);
        } else {
            set_state(WIFI_STATE_FAILED);
        }
        return;
    }

    set_state(WIFI_STATE_CONNECTING);
    // A new network is retried promptly; a saved one backs off while it's unreachable.
    int delay_ms = RETRY_MIN_MS;
    if (!s_trying_new) {
        for (int i = 1; i < s_attempts && delay_ms < RETRY_MAX_MS; i++) {
            delay_ms *= 2;
        }
        delay_ms = delay_ms > RETRY_MAX_MS ? RETRY_MAX_MS : delay_ms;
    }
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void on_got_ip(const ip_event_got_ip_t *ev)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = WIFI_STATE_CONNECTED;
    snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ev->ip_info.ip));
    portEXIT_CRITICAL(&s_lock);
    s_attempts = 0;
    ESP_LOGI(TAG, "connected to \"%s\", IP " IPSTR, s_status.ssid, IP2STR(&ev->ip_info.ip));

    if (s_trying_new) {
        s_trying_new = false;
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            save_credentials(&cfg);
        }
    }
}

static int compare_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

static void on_scan_done(void)
{
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t *records = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
    if (!records) {
        esp_wifi_clear_ap_list();
        return;
    }
    esp_wifi_scan_get_ap_records(&count, records);
    qsort(records, count, sizeof(wifi_ap_record_t), compare_rssi);

    // Keep the strongest access point for each name and drop hidden networks
    portENTER_CRITICAL(&s_lock);
    s_scan_count = 0;
    for (int i = 0; i < count && s_scan_count < MAX_SCAN_RESULTS; i++) {
        if (!records[i].ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < s_scan_count && !dup; j++) {
            dup = strcmp((const char *)s_scan[j].ssid, (const char *)records[i].ssid) == 0;
        }
        if (!dup) {
            s_scan[s_scan_count++] = records[i];
        }
    }
    s_scan_generation++;
    portEXIT_CRITICAL(&s_lock);
    free(records);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_status.state == WIFI_STATE_CONNECTING) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        on_disconnected(data);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        on_scan_done();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip(data);
    }
}

esp_err_t wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL),
                        TAG, "register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL),
                        TAG, "register failed");

    const esp_timer_create_args_t timer_args = {
        .callback = retry_cb,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "timer failed");

    char ssid[33];
    char pass[65];
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "connecting to saved network \"%s\"", ssid);
        apply_config(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no saved network");
    }
    return esp_wifi_start();
}

void wifi_get_status(wifi_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);

    wifi_ap_record_t ap;
    if (out->state == WIFI_STATE_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
    }
}

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();

    portENTER_CRITICAL(&s_lock);
    s_status.error = NULL;
    s_status.error_ssid[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    ESP_RETURN_ON_ERROR(apply_config(ssid, password), TAG, "config failed");
    s_trying_new = true;
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return esp_wifi_connect();
}

void wifi_forget(void)
{
    esp_timer_stop(s_retry_timer);
    s_trying_new = false;
    // Reset to UNCONFIGURED first so the disconnect event that follows is ignored
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    portEXIT_CRITICAL(&s_lock);
    esp_wifi_disconnect();
    erase_credentials();
    ESP_LOGI(TAG, "forgot saved network");
}

esp_err_t wifi_scan_start(void)
{
    return esp_wifi_scan_start(NULL, false);
}

int wifi_scan_generation(void)
{
    portENTER_CRITICAL(&s_lock);
    int gen = s_scan_generation;
    portEXIT_CRITICAL(&s_lock);
    return gen;
}

int wifi_scan_results(wifi_ap_record_t *out, int max)
{
    portENTER_CRITICAL(&s_lock);
    int count = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, count * sizeof(wifi_ap_record_t));
    portEXIT_CRITICAL(&s_lock);
    return count;
}
