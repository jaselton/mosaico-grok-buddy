// SPDX-License-Identifier: Apache-2.0
/* Wi-Fi station: connect with Vibe Mode credentials and keep reconnecting.
 * An optional backup network (e.g. a phone hotspot) is tried after the current
 * one fails twice in a row, alternating until one connects. */
#include <ctype.h>
#include <stdlib.h>
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
#define FAILS_BEFORE_SWITCH 2

static uint32_t s_retry_ms = RETRY_MIN_MS;
static wifi_config_t s_nets[2];
static char s_backup_name[33]; /* as typed; matched loosely against scans */
static int s_net_count;
static int s_net;
static int s_fails;
static bool s_switch;

static void fill_config(wifi_config_t *wc, const char *ssid, const char *password)
{
    memset(wc, 0, sizeof(*wc));
    /* Fields need no NUL at full length (32-byte SSID, 64-hex PSK). */
    memcpy(wc->sta.ssid, ssid, strnlen(ssid, sizeof(wc->sta.ssid)));
    memcpy(wc->sta.password, password, strnlen(password, sizeof(wc->sta.password)));
    wc->sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc->sta.pmf_cfg.capable = true;
}

/* Lower-case ASCII and fold curly apostrophes to ' so "Sam's Iphone" matches
 * the phone's broadcast "Sam’s iPhone". */
static void fold(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && in[i] && o + 1 < cap; ++i) {
        if (i + 2 < n && in[i] == 0xE2 && in[i + 1] == 0x80 && (in[i + 2] == 0x98 || in[i + 2] == 0x99)) {
            out[o++] = '\'';
            i += 2;
        } else {
            out[o++] = (char)tolower(in[i]);
        }
    }
    out[o] = '\0';
}

/* Scan finished while looking for the backup: adopt the visible SSID that
 * matches loosely (exact bytes are required to join), then connect. */
static void on_scan_done(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) {
        n = 40;
    }
    wifi_ap_record_t *aps = n ? calloc(n, sizeof(*aps)) : NULL;
    bool found = false;
    if (aps && esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
        char want[40], got[40];
        fold((const uint8_t *)s_backup_name, strlen(s_backup_name), want, sizeof(want));
        for (int i = 0; i < n && !found; ++i) {
            fold(aps[i].ssid, sizeof(aps[i].ssid), got, sizeof(got));
            if (!strcmp(want, got)) {
                memset(s_nets[1].sta.ssid, 0, sizeof(s_nets[1].sta.ssid));
                memcpy(s_nets[1].sta.ssid, aps[i].ssid, strnlen((const char *)aps[i].ssid, 32));
                found = true;
                ESP_LOGI(TAG, "backup network visible (rssi %d, ch %d, auth %d)", aps[i].rssi, aps[i].primary,
                         aps[i].authmode);
            }
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    free(aps);
    if (!found) {
        ESP_LOGW(TAG, "backup network not in scan (%u networks seen); trying its name anyway", n);
    }
    esp_wifi_set_config(WIFI_IF_STA, &s_nets[1]);
    esp_wifi_connect();
}

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    if (s_switch) {
        s_switch = false;
        s_net = (s_net + 1) % s_net_count;
        ESP_LOGI(TAG, "trying %s network", s_net ? "backup" : "saved");
        if (s_net == 1) {
            if (esp_wifi_scan_start(NULL, false) == ESP_OK) {
                return; /* connect from WIFI_EVENT_SCAN_DONE */
            }
        }
        esp_wifi_set_config(WIFI_IF_STA, &s_nets[s_net]);
    }
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_net == 1) {
            on_scan_done();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        if (s_net_count > 1 && ++s_fails >= FAILS_BEFORE_SWITCH) {
            s_fails = 0;
            s_switch = true;
            s_retry_ms = RETRY_MIN_MS;
        }
        ESP_LOGW(TAG, "disconnected (reason %d); retry in %lu ms", d ? d->reason : -1,
                 (unsigned long)s_retry_ms);
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_retry_ms), 0);
        xTimerStart(s_retry_timer, 0);
        s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_ms = RETRY_MIN_MS;
        s_fails = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t voice_wifi_start(const char *ssid, const char *password, const char *backup_ssid,
                           const char *backup_password)
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

    s_net_count = 0;
    if (ssid && ssid[0]) {
        fill_config(&s_nets[s_net_count++], ssid, password ? password : "");
    }
    if (backup_ssid && backup_ssid[0]) {
        const char *bp = backup_password ? backup_password : "";
        fill_config(&s_nets[s_net_count++], backup_ssid, bp);
        snprintf(s_backup_name, sizeof(s_backup_name), "%s", backup_ssid);
    }
    if (!s_net_count) {
        return ESP_ERR_NOT_FOUND;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &s_nets[0]);
    }
    if (err == ESP_OK) {
        /* Modem sleep adds latency to streamed audio; the device is USB powered. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        err = esp_wifi_start();
    }
    ESP_LOGI(TAG, "station starting (%d network%s)", s_net_count, s_net_count > 1 ? "s, backup after 2 failures" : "");
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
