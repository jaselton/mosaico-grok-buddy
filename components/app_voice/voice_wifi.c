// SPDX-License-Identifier: Apache-2.0
/* Wi-Fi station: connect with Vibe Mode credentials and keep reconnecting. */
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "voice_internal.h"

static const char *TAG = "voice_wifi";

#define BIT_CONNECTED BIT0
#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000

static EventGroupHandle_t s_events;
static TimerHandle_t s_retry_timer;
static uint32_t s_retry_ms = RETRY_MIN_MS;

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        ESP_LOGW(TAG, "disconnected (reason %d); retry in %lu ms", d ? d->reason : -1,
                 (unsigned long)s_retry_ms);
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_retry_ms), 0);
        xTimerStart(s_retry_timer, 0);
        s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_ms = RETRY_MIN_MS;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t voice_wifi_start(const char *ssid, const char *password)
{
    s_events = xEventGroupCreate();
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(RETRY_MIN_MS), pdFALSE, NULL, retry_cb);
    if (!s_events || !s_retry_timer) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_FAIL;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wc = { 0 };
    /* Fields need no NUL at full length (32-byte SSID, 64-hex PSK). */
    const size_t ssid_len = strnlen(ssid, sizeof(wc.sta.ssid));
    const size_t pass_len = strnlen(password, sizeof(wc.sta.password));
    memcpy(wc.sta.ssid, ssid, ssid_len);
    memcpy(wc.sta.password, password, pass_len);
    wc.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    memset(&wc, 0, sizeof(wc));
    if (err == ESP_OK) {
        /* Modem sleep adds latency to streamed audio; the device is USB powered. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        err = esp_wifi_start();
    }
    ESP_LOGI(TAG, "station starting for saved network");
    return err;
}

bool voice_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_events) {
        return false;
    }
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) &
           BIT_CONNECTED;
}
