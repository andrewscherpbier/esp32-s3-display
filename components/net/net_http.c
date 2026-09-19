#include "net_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define RX_BUFFER   4096    // the default 512 makes large bodies read slowly

struct net_http_session {
    esp_http_client_handle_t client;
    char host[64];
};

static const char *TAG = "net_http";

// "https://host/path" -> "host"
static void url_host(const char *url, char *host, size_t len)
{
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    size_t n = strcspn(start, "/:?");
    n = n < len - 1 ? n : len - 1;
    memcpy(host, start, n);
    host[n] = '\0';
}

static void drop_connection(net_http_session_t s)
{
    if (s->client) {
        esp_http_client_cleanup(s->client);
        s->client = NULL;
    }
    s->host[0] = '\0';
}

net_http_session_t net_http_session_create(void)
{
    return calloc(1, sizeof(struct net_http_session));
}

void net_http_session_destroy(net_http_session_t s)
{
    if (s) {
        drop_connection(s);
        free(s);
    }
}

int net_http_session_get(net_http_session_t s, const char *url, char *buf, int size)
{
    char host[sizeof(s->host)];
    url_host(url, host, sizeof(host));
    if (s->client && strcmp(host, s->host) != 0) {
        drop_connection(s);
    }
    if (!s->client) {
        const esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .buffer_size = RX_BUFFER,
            .keep_alive_enable = true,
        };
        s->client = esp_http_client_init(&cfg);
        if (!s->client) {
            return -1;
        }
        strlcpy(s->host, host, sizeof(s->host));
    } else {
        esp_http_client_set_url(s->client, url);
    }

    // open() reuses the connection if the previous response was read to the end
    if (esp_http_client_open(s->client, 0) != ESP_OK) {
        ESP_LOGW(TAG, "%s: connection failed", url);
        drop_connection(s);
        return -1;
    }
    esp_http_client_fetch_headers(s->client);
    int status = esp_http_client_get_status_code(s->client);
    int total = 0;
    int n;
    while (total < size - 1 && (n = esp_http_client_read(s->client, buf + total, size - 1 - total)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    bool complete = esp_http_client_is_complete_data_received(s->client);
    if (status != 200) {
        ESP_LOGW(TAG, "%s: HTTP %d", url, status);
        total = -1;
    } else if (!complete) {
        ESP_LOGW(TAG, "%s: response larger than %d bytes, or cut short", url, size - 1);
        total = -1;
    }
    if (!complete) {
        drop_connection(s);     // unread data would corrupt the next response
    }
    return total;
}

int net_https_get(const char *url, char *buf, int size)
{
    net_http_session_t s = net_http_session_create();
    if (!s) {
        return -1;
    }
    int n = net_http_session_get(s, url, buf, size);
    net_http_session_destroy(s);
    return n;
}
